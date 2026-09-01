#include "MapZoom.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "Log.h"

// Disasm map (GTA:SA Android 2.11.311 arm64, libGame.so):
//   Menu_MapUpdate(float)  @0x71bd88 — per-frame map-page logic; called ONLY
//                                       while the map screen is displayed.
//   gMobileMenu (exported data)      — the mobile menu state block.
//     +0x78 float map size   (zoom: the map covers [cx-size, cx+size] x
//     +0x7c float centre x    [cy-size, cy+size] in the 640x448 UI space —
//     +0x80 float centre y    offsets verified in Menu_MapInitPause's edge
//                             clamps against the 640/448 constants)
//   Engine bounds seen in Menu_MapUpdate: sizes 152..800.
namespace savr::mapzoom {
namespace {

using MapUpdateFn = void (*)(float dt);

MapUpdateFn   g_origMapUpdate = nullptr;
std::uint8_t* g_mobileMenu    = nullptr;

std::atomic<float> g_gripL{0.0f};
std::atomic<float> g_gripR{0.0f};

constexpr float kMinSize   = 152.0f;   // engine's own zoom-out limit
constexpr float kMaxSize   = 800.0f;   // engine's own zoom-in limit
constexpr float kZoomSpeed = 1.1f;     // e-fold per second of held grip
constexpr float kGripOn    = 0.6f;

bool PatchAbsoluteJump(void* target, void* replacement) {
    if (!target || !replacement) return false;
    const std::uintptr_t pageSize =
        static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    const std::uintptr_t start =
        reinterpret_cast<std::uintptr_t>(code) & ~(pageSize - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(code) + 16 + pageSize - 1) &
        ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    code[0] = 0x58000051u;   // LDR X17, #8
    code[1] = 0xD61F0220u;   // BR  X17
    *reinterpret_cast<void**>(code + 2) = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    return true;
}

void* InstallVerifiedTrampoline(void* target, void* replacement,
                                const std::uint32_t expected[4],
                                const char* name) {
    if (!target || !replacement) return nullptr;
    std::uint32_t observed[4]{};
    std::memcpy(observed, target, sizeof(observed));
    if (std::memcmp(observed, expected, sizeof(observed)) != 0) {
        LOGE("[mapzoom] %s prologue mismatch: %08x %08x %08x %08x", name,
             observed[0], observed[1], observed[2], observed[3]);
        return nullptr;
    }
    const std::uintptr_t pageSize =
        static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline = mmap(nullptr, pageSize,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return nullptr;
    auto* t = reinterpret_cast<std::uint32_t*>(trampoline);
    std::memcpy(t, target, 16);
    t[4] = 0x58000051u;
    t[5] = 0xD61F0220u;
    *reinterpret_cast<void**>(t + 6) = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 32);
    if (!PatchAbsoluteJump(target, replacement)) {
        munmap(trampoline, pageSize);
        return nullptr;
    }
    LOGI("[mapzoom] %s hook installed", name);
    return trampoline;
}

float ReadF(std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, g_mobileMenu + offset, sizeof(value));
    return value;
}

void WriteF(std::size_t offset, float value) {
    std::memcpy(g_mobileMenu + offset, &value, sizeof(value));
}

// Re-apply the engine's own edge rules after our zoom moved the rect: the
// map must cover the screen edge-to-edge on any axis where it is large
// enough, and sit centred on an axis it cannot fill.
float ClampCentre(float centre, float size, float half, float full) {
    if (size * 2.0f <= full) return half;
    if (centre - size > 0.0f) centre = size;
    if (centre + size < full) centre = full - size;
    return centre;
}

void OnMapUpdate(float dt) {
    if (g_origMapUpdate) g_origMapUpdate(dt);
    if (!g_mobileMenu) return;
    if (!std::isfinite(dt) || dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    const bool zoomIn  = g_gripR.load(std::memory_order_relaxed) > kGripOn;
    const bool zoomOut = g_gripL.load(std::memory_order_relaxed) > kGripOn;
    if (zoomIn == zoomOut) return;   // neither, or both cancelling

    const float size = ReadF(0x78);
    if (!std::isfinite(size) || size <= 1.0f) return;
    const float factor = std::exp((zoomIn ? kZoomSpeed : -kZoomSpeed) * dt);
    const float next = std::clamp(size * factor, kMinSize, kMaxSize);
    if (next == size) return;
    const float applied = next / size;

    // Zoom about the middle of the 640x448 UI screen, then restore the
    // engine's edge invariants.
    float cx = 320.0f + (ReadF(0x7c) - 320.0f) * applied;
    float cy = 224.0f + (ReadF(0x80) - 224.0f) * applied;
    cx = ClampCentre(cx, next, 320.0f, 640.0f);
    cy = ClampCentre(cy, next, 224.0f, 448.0f);
    if (!std::isfinite(cx) || !std::isfinite(cy)) return;
    WriteF(0x78, next);
    WriteF(0x7c, cx);
    WriteF(0x80, cy);
}

}  // namespace

void Install(void* handle) {
    if (!handle) return;
    g_mobileMenu =
        static_cast<std::uint8_t*>(dlsym(handle, "gMobileMenu"));
    void* target = dlsym(handle, "_Z14Menu_MapUpdatef");
    if (!g_mobileMenu || !target) {
        LOGW("[mapzoom] symbols missing (menu=%d update=%d)",
             g_mobileMenu != nullptr, target != nullptr);
        return;
    }
    // sub sp,#0xf0 / stp d15,d14 / stp d13,d12 / stp d11,d10 - all
    // position-independent, safe to relocate into the trampoline.
    constexpr std::uint32_t kPrologue[4] = {
        0xd103c3ffu, 0x6d053befu, 0x6d0633edu, 0x6d072bebu};
    g_origMapUpdate = reinterpret_cast<MapUpdateFn>(InstallVerifiedTrampoline(
        target, reinterpret_cast<void*>(&OnMapUpdate), kPrologue,
        "Menu_MapUpdate"));
}

void SetGrips(float left, float right) {
    g_gripL.store(left, std::memory_order_relaxed);
    g_gripR.store(right, std::memory_order_relaxed);
}

}  // namespace savr::mapzoom
