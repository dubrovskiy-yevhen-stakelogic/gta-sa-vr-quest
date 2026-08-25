#include "EyeDepth.h"

#include "Log.h"

#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace savr::eye_depth {
namespace {

constexpr char kProperty[] = "debug.savr.eye_depth24";
constexpr bool kDefaultEnabled = true;

// Rockstar retail 2.11 ARM64. _rwOpenGLRasterCreate calls this through the
// named PLT slot with color=0, depth=1 for camera-texture rasters.
constexpr std::uintptr_t kCreateRva = 0x793f28u;
constexpr std::uintptr_t kCreatePltRva = 0x812090u;
constexpr std::uintptr_t kCreateGotRva = 0x84da88u;
constexpr std::uintptr_t kCameraTargetCallRva = 0x775848u;
constexpr std::uintptr_t kLazyResolverRva = 0x7f4680u;

constexpr std::uint32_t kCreatePrologue[4] = {
    0xd10183ffu, 0xa9027bfdu, 0xa9035ff8u, 0xa90457f6u,
};
constexpr std::uint32_t kCreatePlt[4] = {
    0xf00001d0u, 0xf9454611u, 0x912a2210u, 0xd61f0220u,
};
constexpr std::uint32_t kCameraTargetCall[4] = {
    0x2a1f03e2u, 0x52800023u, 0x94027210u, 0xf90012c0u,
};
constexpr std::uint32_t kLazyResolver[4] = {
    0xa9bf7bf0u, 0xd0000250u, 0xf946c611u, 0x91362210u,
};

using RQCreateFn = void* (*)(std::uint32_t width, std::uint32_t height,
                             int colorType, int depthType);

struct EyeCreateScope {
    int width{};
    int height{};
    bool armed{};
    bool upgraded{};
};

thread_local EyeCreateScope g_scope{};
std::atomic<RQCreateFn> g_originalCreate{nullptr};
std::atomic<bool> g_requested{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_installAttempted{false};
std::atomic<std::uint32_t> g_upgradeSubmissions{0};

bool RequestedByProperty() {
    char text[PROP_VALUE_MAX]{};
    bool enabled = kDefaultEnabled;
    if (__system_property_get(kProperty, text) > 0) {
        if (std::strcmp(text, "0") == 0) enabled = false;
        else if (std::strcmp(text, "1") == 0) enabled = true;
        else LOGW("[stereo.depth] ignoring invalid %s=%s (valid 0 or 1)",
                  kProperty, text);
    }
    return enabled;
}

void* OnRQRenderTargetCreate(std::uint32_t width, std::uint32_t height,
                             int colorType, int depthType) {
    int effectiveDepth = depthType;
    bool upgraded = false;
    if (g_scope.armed && colorType == 0 && depthType == 1 &&
        width == static_cast<std::uint32_t>(g_scope.width) &&
        height == static_cast<std::uint32_t>(g_scope.height)) {
        // Consume before entering retail code so a nested target allocation
        // cannot inherit the marker.
        g_scope.armed = false;
        g_scope.upgraded = true;
        effectiveDepth = 2;
        upgraded = true;
    }

    const RQCreateFn original =
        g_originalCreate.load(std::memory_order_acquire);
    void* const result = original
        ? original(width, height, colorType, effectiveDepth)
        : nullptr;
    if (upgraded) {
        const std::uint32_t submission =
            g_upgradeSubmissions.fetch_add(1, std::memory_order_relaxed) + 1;
        LOGI("[stereo.depth] eye target #%u %ux%u submitted depth=1->2 result=%p",
             submission, width, height, result);
    }
    return result;
}

bool InstallGuardedGotHook(std::uintptr_t base) {
    const auto fingerprintMatches = [base](std::uintptr_t rva,
                                           const std::uint32_t expected[4]) {
        std::uint32_t observed[4]{};
        std::memcpy(observed, reinterpret_cast<const void*>(base + rva),
                    sizeof(observed));
        return std::memcmp(observed, expected, sizeof(observed)) == 0;
    };
    if (!fingerprintMatches(kCreateRva, kCreatePrologue) ||
        !fingerprintMatches(kCreatePltRva, kCreatePlt) ||
        !fingerprintMatches(kCameraTargetCallRva, kCameraTargetCall)) {
        LOGW("[stereo.depth] D24 disabled: retail fingerprint mismatch");
        return false;
    }

    g_originalCreate.store(reinterpret_cast<RQCreateFn>(base + kCreateRva),
                           std::memory_order_release);
    auto* const slot = reinterpret_cast<std::uintptr_t*>(base + kCreateGotRva);
    const std::uintptr_t direct = base + kCreateRva;
    const std::uintptr_t hook =
        reinterpret_cast<std::uintptr_t>(&OnRQRenderTargetCreate);
    const std::uintptr_t lazyResolver = base + kLazyResolverRva;
    std::uintptr_t observed = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (observed == hook) return true;

    const bool directSlot = observed == direct;
    bool lazySlot = observed == lazyResolver;
    if (lazySlot) {
        std::uint32_t resolver[4]{};
        std::memcpy(resolver, reinterpret_cast<const void*>(lazyResolver),
                    sizeof(resolver));
        lazySlot = std::memcmp(resolver, kLazyResolver, sizeof(resolver)) == 0;
    }
    if (!directSlot && !lazySlot) {
        LOGW("[stereo.depth] D24 disabled: RQ create GOT already owned (%p)",
             reinterpret_cast<void*>(observed));
        return false;
    }

    const long rawPageSize = sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) {
        LOGW("[stereo.depth] D24 disabled: invalid page size");
        return false;
    }
    const auto pageSize = static_cast<std::uintptr_t>(rawPageSize);
    const std::uintptr_t page =
        reinterpret_cast<std::uintptr_t>(slot) & ~(pageSize - 1u);
    if (mprotect(reinterpret_cast<void*>(page), pageSize,
                 PROT_READ | PROT_WRITE) != 0) {
        LOGW("[stereo.depth] D24 disabled: GOT mprotect RW failed");
        return false;
    }

    std::uintptr_t expected = observed;
    const bool swapped = __atomic_compare_exchange_n(
        slot, &expected, hook, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    const bool readbackOk = swapped &&
        __atomic_load_n(slot, __ATOMIC_ACQUIRE) == hook;
    if (!readbackOk) {
        mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ);
        LOGW("[stereo.depth] D24 disabled: guarded GOT swap lost");
        return false;
    }
    if (mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ) != 0) {
        if (mprotect(reinterpret_cast<void*>(page), pageSize,
                     PROT_READ | PROT_WRITE) == 0) {
            std::uintptr_t rollbackExpected = hook;
            __atomic_compare_exchange_n(slot, &rollbackExpected, observed, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE);
            mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ);
        }
        LOGW("[stereo.depth] D24 disabled: RELRO restore failed");
        return false;
    }

    LOGI("[stereo.depth] guarded RQ create hook active got=%p prior=%s",
         slot, lazySlot ? "lazy" : "direct");
    return true;
}

}  // namespace

bool Install(std::uintptr_t gameLoadBase) {
    if (g_installAttempted.exchange(true, std::memory_order_acq_rel))
        return g_active.load(std::memory_order_acquire);

    const bool requested = RequestedByProperty();
    g_requested.store(requested, std::memory_order_release);
    bool active = false;
    if (requested && gameLoadBase != 0)
        active = InstallGuardedGotHook(gameLoadBase);
    g_active.store(active, std::memory_order_release);
    LOGI("[stereo.depth] requested=%d active=%d property=%s scope=eye_rasters_only",
         requested ? 1 : 0, active ? 1 : 0, kProperty);
    return active;
}

void* CreateStereoEyeRaster(RwRasterCreateFn create,
                            int width, int height, int depth, int flags) {
    if (!create) return nullptr;
    if (!g_requested.load(std::memory_order_acquire) ||
        !g_active.load(std::memory_order_acquire) ||
        depth != 32 || flags != 5) {
        return create(width, height, depth, flags);
    }

    const EyeCreateScope previous = g_scope;
    g_scope = EyeCreateScope{width, height, true, false};
    void* const raster = create(width, height, depth, flags);
    const bool upgraded = g_scope.upgraded;
    g_scope = previous;

    if (!upgraded) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            LOGW("[stereo.depth] eye raster %dx%d was not intercepted; stock D16 retained",
                 width, height);
        }
    }
    return raster;
}

}  // namespace savr::eye_depth
