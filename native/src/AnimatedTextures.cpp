#include "AnimatedTextures.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "Log.h"
#include "Symbols.h"

// Animated (UV-scrolling) textures on GTA:SA Android 2.11.311 arm64 (libGame.so,
// sha 4C6A7445…). Disasm-verified, simulated against all 28 shipped UV anims.
//
// TWO separate defects keep them frozen on this build:
//
//  (1) The per-frame advance is gated OFF. RtAnimInterpolatorAddAnimTime
//      (@0x787730) only re-interpolates keyframes while the transient byte
//      RunUVAnim (@0xd13498) reads 1, and the engine only raises it around a
//      BIND (RpMaterialUVAnimStreamRead: Set360UVAnimHack(1) -> SetCurrentAnim
//      -> Set360UVAnimHack(0)). At advance time it is always 0 -> the clock
//      ticks but nothing re-interpolates. (Every UV bind IS overlay-seeded; the
//      binder and the advance both run on the game thread; a static poke of the
//      byte black-screened, and wrapping the byte around the advance did not
//      help either — so we do NOT touch the byte at all.)
//
//  (2) A real 64-bit porting bug: RpUVAnimParamKeyFrameInterpolate (@0x731944)
//      writes the interpolated frame at {theta@0x10,s0@0x14,s1@0x18,skew@0x1c,
//      x@0x20,y@0x24} (16-byte {kf1,kf2} header on arm64), but the two Param
//      APPLY readers — RpUVAnimParamKeyFrameApply (@0x73184c) and the inlined
//      path in RpMaterialUVAnimApplyUpdate (@0x732818..0x73287c) — still read
//      the 32-bit keyframe offsets {0xc,0x10,0x14,0x18,0x1c,0x20}: +4 off. So
//      rotation = pointer bits, 'y' is never read, and 'x' lands on the wrong
//      axis. All 28 shipped anims are Param type, so even a perfect advance
//      looks frozen. Fix = bump those 11 load immediates by +4.
//
// FIX (hang-proof by construction):
//  - Hook RpMaterialUVAnimAddAnimTime (@0x732bcc) and perform the advance
//    ourselves for the material's 8 interpolator slots: tick time, wrap+reseed
//    on duration, step the keyframe cursor with every pointer range-checked and
//    every loop bounded, then call the anim's own interpolate callback. The
//    engine's gated loops (the ones that could spin) are never executed and
//    RunUVAnim stays 0, so binds/skeletal/streaming are untouched.
//  - Patch the 11 Param-apply words so the interpolated values are applied.
namespace savr::animtex {
namespace {

constexpr std::uintptr_t kAddAnimTimeRVA   = 0x732bccu;   // RpMaterialUVAnimAddAnimTime
constexpr std::uintptr_t kUVGlobalsRVA     = 0xd0df40u;   // RpUVAnimMaterialGlobals: int extOffset
// Expected RpMaterialUVAnimAddAnimTime prologue (all 4 relocatable):
//   str d8,[sp,#-0x30]! ; stp x29,x30,[sp,#0x10] ; stp x20,x19,[sp,#0x20] ; add x29,sp,#0x10
constexpr std::uint32_t kAddPrologue[4] = {
    0xfc1d0fe8u, 0xa9017bfdu, 0xa9024ff4u, 0x910043fdu};

// arm64 layouts (disasm-verified).
constexpr int kInterpAnim      = 0x00;   // RtAnimAnimation*
constexpr int kInterpTime      = 0x08;   // float currentTime
constexpr int kInterpNext      = 0x10;   // next keyframe cursor (ptr)
constexpr int kInterpKFSize    = 0x44;   // int  interp-frame size
constexpr int kInterpAnimKF    = 0x48;   // int  anim-keyframe size
constexpr int kInterpNumNodes  = 0x4c;   // int
constexpr int kInterpCB        = 0x70;   // interpolate callback
constexpr int kInterpFrames    = 0x80;   // frames[numNodes], each kInterpKFSize
constexpr int kAnimNumFrames   = 0x08;   // int
constexpr int kAnimDuration    = 0x10;   // float
constexpr int kAnimFrames      = 0x18;   // keyframe array (ptr)
constexpr int kAnimCustomData  = 0x20;   // ptr
constexpr int kKFPrev          = 0x00;   // keyframe: prev keyframe of same node (ptr)
constexpr int kKFTime          = 0x08;   // keyframe: float time
constexpr int kFrameKF1        = 0x00;   // interp frame: ptr
constexpr int kFrameKF2        = 0x08;   // interp frame: ptr
constexpr int kMaterialSlots   = 8;      // ext+0x10 + 8*i
constexpr int kMaxNodes        = 64;     // sanity bound
// libGame.so spans ~14 MB (its .bss ends at RVA 0xd1b280). The interpolate
// callback always lives in that image (it comes from the static interpolator
// scheme tables), so anything outside this window is not a callback and must
// never be called.
constexpr std::uintptr_t kModuleSpan = 0x1000000u;   // 16 MB, generous bound
constexpr int kMaxKeyFrameSize = 4096;
constexpr int kMaxFrames       = 65536;

struct Patch { std::uintptr_t rva; std::uint32_t oldWord; std::uint32_t newWord; };
// +4 on each Param-apply load immediate (ApplyUpdate inline + KeyFrameApply).
constexpr Patch kParamApplyPatch[] = {
    {0x732818u, 0x2d420ac0u, 0x2d428ac0u},   // ldp s0,s2,[x22,#0x10] -> #0x14
    {0x732820u, 0xbd401ac1u, 0xbd401ec1u},   // ldr s1,[x22,#0x18]    -> #0x1c
    {0x732834u, 0xbd401ec0u, 0xbd4022c0u},   // ldr s0,[x22,#0x1c]    -> #0x20
    {0x732840u, 0xbd4022c0u, 0xbd4026c0u},   // ldr s0,[x22,#0x20]    -> #0x24
    {0x73287cu, 0xbd400ec0u, 0xbd4012c0u},   // ldr s0,[x22,#0xc]     -> #0x10
    {0x731878u, 0xbd401020u, 0xbd401420u},
    {0x731888u, 0xbd401820u, 0xbd401c20u},
    {0x731898u, 0xbd401420u, 0xbd401820u},
    {0x7318acu, 0xbd401c20u, 0xbd402020u},
    {0x7318b4u, 0xbd402020u, 0xbd402420u},
    {0x7318dcu, 0xbd400e80u, 0xbd401280u},
};

using AddAnimTimeFn = void* (*)(void* material, float dt);
using InterpCB      = void  (*)(void* frame, void* kf1, void* kf2, float t, void* custom);
AddAnimTimeFn g_origAddAnimTime = nullptr;   // trampoline to the original
const std::int32_t* g_uvGlobals = nullptr;   // -> material extension offset

std::atomic<bool> g_enabled{true};
// libGame.so address window, used to validate interpolate callbacks.
std::uintptr_t g_libLo = 0, g_libHi = 0;

template <typename T> T& At(std::uint8_t* base, int off) {
    return *reinterpret_cast<T*>(base + off);
}

// Bounded re-implementation of the engine's ==1 advance for one interpolator.
void AdvanceInterp(std::uint8_t* interp, float dt) {
    if (!(dt >= 0.f) || dt > 100.f) return;   // NaN/absurd dt would poison time
    // The slot sweep cannot prove a non-null word is really an interpolator, so
    // EVERYTHING is validated before we dereference `anim` or write a single
    // byte: the callback must point into libGame, and both pointers must be
    // 8-byte aligned. Validating only the indirect call (as this did first) is
    // not enough - the reads and the kf1/kf2 writes happen before it.
    if ((reinterpret_cast<std::uintptr_t>(interp) & 7u) != 0) return;
    InterpCB cbEarly = At<InterpCB>(interp, kInterpCB);
    const std::uintptr_t cbAddrEarly = reinterpret_cast<std::uintptr_t>(cbEarly);
    if (g_libLo == 0 || cbAddrEarly < g_libLo || cbAddrEarly >= g_libHi) return;
    std::uint8_t* anim = At<std::uint8_t*>(interp, kInterpAnim);
    if (anim == nullptr) return;
    if ((reinterpret_cast<std::uintptr_t>(anim) & 7u) != 0) return;
    const int numNodes = At<std::int32_t>(interp, kInterpNumNodes);
    const int interpKF = At<std::int32_t>(interp, kInterpKFSize);
    const int animKF   = At<std::int32_t>(interp, kInterpAnimKF);
    if (numNodes <= 0 || numNodes > kMaxNodes) return;
    if (interpKF <= 0 || interpKF > kMaxKeyFrameSize) return;
    if (animKF   <= 0 || animKF   > kMaxKeyFrameSize) return;
    const int   numFrames  = At<std::int32_t>(anim, kAnimNumFrames);
    const float duration   = At<float>(anim, kAnimDuration);
    std::uint8_t* pFrames  = At<std::uint8_t*>(anim, kAnimFrames);
    void*  customData      = At<void*>(anim, kAnimCustomData);
    InterpCB cb            = At<InterpCB>(interp, kInterpCB);
    if (pFrames == nullptr || cb == nullptr) return;
    if (numFrames < 2 * numNodes || numFrames > kMaxFrames) return;
    if (!(duration >= 0.f) || !std::isfinite(duration)) return;
    std::uint8_t* const end = pFrames + static_cast<std::size_t>(numFrames) * animKF;
    std::uint8_t* const frames = interp + kInterpFrames;

    auto frameAt = [&](int n) { return frames + static_cast<std::size_t>(n) * interpKF; };
    auto kf1 = [&](int n) -> std::uint8_t*& { return At<std::uint8_t*>(frameAt(n), kFrameKF1); };
    auto kf2 = [&](int n) -> std::uint8_t*& { return At<std::uint8_t*>(frameAt(n), kFrameKF2); };
    auto reseed = [&]() {
        for (int n = 0; n < numNodes; ++n) {
            kf1(n) = pFrames + static_cast<std::size_t>(n) * animKF;
            kf2(n) = pFrames + static_cast<std::size_t>(numNodes + n) * animKF;
        }
        At<std::uint8_t*>(interp, kInterpNext) =
            pFrames + static_cast<std::size_t>(2 * numNodes) * animKF;
    };

    // Tick + wrap (bounded), reseed the cursor on every wrap.
    float time = At<float>(interp, kInterpTime) + dt;
    if (!std::isfinite(time)) time = 0.f;   // a NaN already in the pool
    if (duration > 0.f) {
        bool wrapped = false; int guard = 0;
        while (time > duration && guard++ < 64) { time -= duration; wrapped = true; }
        if (guard >= 64) time = 0.f;
        if (wrapped) reseed();
    } else {
        time = 0.f;
    }
    At<float>(interp, kInterpTime) = time;

    // Step the keyframe cursor: while the next keyframe's predecessor (= that
    // node's current kf2) is due, shift that node forward. Everything bounded
    // and range-checked; any inconsistency reseeds and stops.
    std::uint8_t* pNext = At<std::uint8_t*>(interp, kInterpNext);
    if (pNext < pFrames || pNext > end) { reseed(); pNext = At<std::uint8_t*>(interp, kInterpNext); }
    for (int steps = 0; pNext < end && steps < numFrames; ++steps) {
        std::uint8_t* prev = At<std::uint8_t*>(pNext, kKFPrev);
        if (prev < pFrames || prev >= end) break;          // garbage link: stop
        if (!(At<float>(prev, kKFTime) <= time)) break;    // not due yet
        int node = -1;
        for (int n = 0; n < numNodes; ++n) if (kf2(n) == prev) { node = n; break; }
        if (node < 0) { reseed(); pNext = At<std::uint8_t*>(interp, kInterpNext); break; }
        kf1(node) = kf2(node);
        kf2(node) = pNext;
        pNext += animKF;
    }
    At<std::uint8_t*>(interp, kInterpNext) = pNext;

    for (int n = 0; n < numNodes; ++n) cb(frameAt(n), kf1(n), kf2(n), time, customData);
}

// Hooked RpMaterialUVAnimAddAnimTime(material, dt).
void* OnAddAnimTime(void* material, float dt) {
    if (!g_enabled.load(std::memory_order_acquire) || g_uvGlobals == nullptr ||
        material == nullptr) {
        return g_origAddAnimTime(material, dt);   // OFF: stock (frozen) behaviour
    }
    const std::int32_t extOff = *g_uvGlobals;
    if (extOff <= 0 || extOff > 0x1000) return g_origAddAnimTime(material, dt);
    std::uint8_t* ext = static_cast<std::uint8_t*>(material) + extOff;
    for (int i = 0; i < kMaterialSlots; ++i) {
        std::uint8_t* interp = At<std::uint8_t*>(ext, 0x10 + 8 * i);
        if (interp != nullptr) AdvanceInterp(interp, dt);
    }
    return material;
}

bool MakeWritable(std::uintptr_t lo, std::uintptr_t hi) {
    const long pageLong = sysconf(_SC_PAGESIZE);
    if (pageLong <= 0) return false;
    const std::uintptr_t pageSize = static_cast<std::uintptr_t>(pageLong);
    const std::uintptr_t start = lo & ~(pageSize - 1);
    const std::uintptr_t end   = (hi + pageSize - 1) & ~(pageSize - 1);
    return mprotect(reinterpret_cast<void*>(start), end - start,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

bool InstallAddAnimTimeHook(void* target) {
    if (g_origAddAnimTime != nullptr) return true;
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    std::uint32_t observed[4]{};
    std::memcpy(observed, code, sizeof(observed));
    if (std::memcmp(observed, kAddPrologue, sizeof(observed)) != 0) {
        LOGE("[animtex] AddAnimTime prologue mismatch: %08x %08x %08x %08x",
             observed[0], observed[1], observed[2], observed[3]);
        return false;
    }
    const long pageLong = sysconf(_SC_PAGESIZE);
    if (pageLong <= 0) return false;
    const std::size_t pageSize = static_cast<std::size_t>(pageLong);
    void* tramp = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return false;
    auto* t = reinterpret_cast<std::uint32_t*>(tramp);
    std::memcpy(t, code, 16);
    t[4] = 0x58000051u;   // LDR X17, #8 (resume literal)
    t[5] = 0xD61F0220u;   // BR X17
    *reinterpret_cast<void**>(t + 6) = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 32);
    const std::uintptr_t c = reinterpret_cast<std::uintptr_t>(code);
    if (!MakeWritable(c, c + 16)) { munmap(tramp, pageSize); return false; }
    // Publish the trampoline BEFORE redirecting the target: a call landing in
    // the window between the two would otherwise reach the hook with a null
    // original.
    g_origAddAnimTime = reinterpret_cast<AddAnimTimeFn>(tramp);
    // Publish the literal and the BR first, then arm the branch with a release
    // store: a thread already executing the target must never see the LDR while
    // the literal slot still holds the original instructions.
    *reinterpret_cast<void**>(code + 2) = reinterpret_cast<void*>(&OnAddAnimTime);
    code[1] = 0xD61F0220u;   // BR X17
    __atomic_store_n(&code[0], 0x58000051u, __ATOMIC_RELEASE);   // LDR X17, #8
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    return true;
}

// Apply the 11-word Param-apply offset fix. Every word is verified against its
// expected original before anything is written; one mismatch aborts the whole
// patch so we never poke a different build.
bool ApplyParamPatch(std::uintptr_t base) {
    std::uintptr_t lo = ~std::uintptr_t{0}, hi = 0;
    for (const Patch& p : kParamApplyPatch) {
        const std::uint32_t cur = *reinterpret_cast<std::uint32_t*>(base + p.rva);
        if (cur != p.oldWord && cur != p.newWord) {
            LOGE("[animtex] param-apply word @+0x%lx is %08x (expected %08x) - not patching",
                 static_cast<unsigned long>(p.rva), cur, p.oldWord);
            return false;
        }
        lo = std::min(lo, base + p.rva);
        hi = std::max(hi, base + p.rva + 4);
    }
    if (!MakeWritable(lo, hi)) {
        LOGE("[animtex] could not unprotect param-apply code");
        return false;
    }
    for (const Patch& p : kParamApplyPatch)
        *reinterpret_cast<std::uint32_t*>(base + p.rva) = p.newWord;
    __builtin___clear_cache(reinterpret_cast<char*>(lo), reinterpret_cast<char*>(hi));
    return true;
}

}  // namespace

void Install(void* /*handle*/) {
    const std::uintptr_t base = g.LoadBase;
    if (base == 0) {
        LOGE("[animtex] no libGame.so base - UV animation unavailable");
        return;
    }
    g_uvGlobals = reinterpret_cast<const std::int32_t*>(base + kUVGlobalsRVA);
    g_libLo = base;
    g_libHi = base + kModuleSpan;
    // Hook FIRST. The 11-word apply patch is only correct in combination with
    // our advance: patching it while the hook failed would ship shifted UV
    // reads with nothing driving them.
    const bool hooked  = InstallAddAnimTimeHook(
        reinterpret_cast<void*>(base + kAddAnimTimeRVA));
    const bool patched = hooked && ApplyParamPatch(base);
    LOGI("[animtex] extOffset=%d paramPatch=%s advanceHook=%s default=%s",
         *g_uvGlobals, patched ? "OK" : "FAILED", hooked ? "OK" : "FAILED",
         g_enabled.load(std::memory_order_acquire) ? "ON" : "OFF");
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    LOGI("[animtex] animated textures %s via menu", enabled ? "ON" : "OFF");
}

bool IsEnabled() {
    return g_enabled.load(std::memory_order_acquire);
}

}  // namespace savr::animtex
