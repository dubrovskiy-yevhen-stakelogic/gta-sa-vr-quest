#include "GraphicsEffects.h"

#include "Calib.h"
#include "Driving.h"
#include "Log.h"
#include "PhysicalWeapon.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>

namespace savr::graphicsfx {
namespace {

constexpr char kEffectsProperty[] = "debug.savr.effects";
constexpr char kFxProbeProperty[] = "debug.savr.fxprobe";
// Keep parallel performance work bit-identical until the effect A/B is
// explicitly requested on the device. Profile 1 is the intended candidate,
// but process-start opt-in avoids silently contaminating another agent's run.
constexpr int kDefaultProfile = 0;
constexpr std::uint64_t kReportIntervalCalls = 240; // 120 stereo frames
constexpr std::uint64_t kFlameLifecycleGraceMs = 120;

using VoidFn = void (*)();
using RenderStateSetFn = int (*)(int, void*);
using RenderStateGetFn = int (*)(int, void*);
using FxRenderFn = void (*)(void*, void*, unsigned char);
using Im3DTransformFn = void* (*)(void*, unsigned int, void*, unsigned int);
using Im3DRenderPrimitiveFn = int (*)(int);
using FxManagerUpdateFn = void (*)(void*, void*, float);
using FxFrustumCollisionFn = int (*)(void*, void*);
using RwFrameGetLtmFn = void* (*)(void*);
using CMatrixUpdateFn = void (*)(void*);
using RwCameraBeginUpdateFn = void (*)(void*);
using RqRenderTargetSelectFn = void (*)(void*, bool);
using ResolveShadowTargetFn = void (*)(float);
using ProcessLightsFn = void (*)(void*);
using Get2dEffectFn = void* (*)(void*, std::int32_t);
using StoreStaticShadowFn = bool (*)(
    std::uint64_t, std::uint8_t, void*, GameSymbols::Vec3*,
    float, float, float, float,
    std::int16_t, std::uint8_t, std::uint8_t, std::uint8_t,
    float, float, float, bool, float);
using RenderReflectionsFn = void (*)();
using DisplayActualLightFn = void (*)(void*);
using FireAreaEffectFn = bool (*)(void*, void*, GameSymbols::Vec3*, void*,
                                  GameSymbols::Vec3*);
using CreateFxSystemFn = void* (*)(void*, char*, void*, void*, std::uint8_t);
using WeaponUpdateFn = void (*)(void*, void*);
using StoreCarLightShadowFn = void (*)(
    void*, std::int32_t, void*, GameSymbols::Vec3*,
    float, float, float, float,
    std::uint8_t, std::uint8_t, std::uint8_t, float);
using EditVehicleMaterialsFn = void (*)(void*);
using RpAtomicCallbackFn = void* (*)(void*, void*);
using RpMaterialCallbackFn = void* (*)(void*, void*);
using RpClumpForAllAtomicsFn = void* (*)(
    void*, RpAtomicCallbackFn, void*);
using RpGeometryForAllMaterialsFn = void* (*)(
    void*, RpMaterialCallbackFn, void*);

struct Functions {
    VoidFn definedState{};
    RenderStateSetFn renderStateSet{};
    RenderStateGetFn renderStateGet{};
    VoidFn birds{};
    VoidFn skidmarks{};
    VoidFn ropes{};
    VoidFn glass{};
    VoidFn movingThings{};
    VoidFn coronas{};
    CMatrixUpdateFn matrixUpdate{};
    FxRenderFn fxRender{};
    void* fx{};
    VoidFn waterCannons{};
    VoidFn heliPreSearchlight{};
    VoidFn heliSearchlights{};
    VoidFn scriptSearchlights{};
    VoidFn heliPostSearchlight{};
    VoidFn brightLights{};
    VoidFn shinyTexts{};
    VoidFn markers{};
    VoidFn checkpoints{};
    VoidFn pointLightFog{};
};

Functions g_fn{};
Im3DTransformFn g_origIm3DTransform{};
Im3DRenderPrimitiveFn g_origIm3DRenderPrimitive{};
FxManagerUpdateFn g_origFxManagerUpdate{};
FxFrustumCollisionFn g_origFxFrustumCollision{};
RwFrameGetLtmFn g_rwFrameGetLtm{};
RwCameraBeginUpdateFn g_origRwCameraBeginUpdate{};
RqRenderTargetSelectFn g_rqRenderTargetSelect{};
ResolveShadowTargetFn g_origResolveShadowTarget{};
ProcessLightsFn g_origProcessLights{};
Get2dEffectFn g_get2dEffect{};
StoreStaticShadowFn g_storeStaticShadow{};
RenderReflectionsFn g_origRenderReflections{};
DisplayActualLightFn g_origDisplayActualLight{};
FireAreaEffectFn g_origFireAreaEffect{};
CreateFxSystemFn g_origCreateFxSystem{};
WeaponUpdateFn g_origWeaponUpdate{};
StoreCarLightShadowFn g_origStoreCarLightShadow{};
EditVehicleMaterialsFn g_origSetEditableMaterials{};
EditVehicleMaterialsFn g_origResetEditableMaterials{};
RpClumpForAllAtomicsFn g_rpClumpForAllAtomics{};
RpGeometryForAllMaterialsFn g_rpGeometryForAllMaterials{};
int* g_rasterExtOffset{};
void** g_rqSelectedTarget{};
void** g_backTarget{};
std::uint8_t* g_scene{};
std::uint8_t* g_staticShadows{};
void** g_shadowHeadLightsTex{};
void** g_shadowHeadLightsTex2{};
int* g_numPointLights{};
void** g_modelInfoPtrs{};
float* g_weatherWetRoads{};
float* g_weatherUnderWaterness{};
float* g_weatherWaterDepth{};
std::atomic<bool> g_resolveAttempted{false};
std::atomic<bool> g_flatPassSuppressed{false};
std::atomic<bool> g_fireFlatPassSuppressed{false};
std::atomic<std::uint64_t> g_renderAttempts{0};
bool g_ready = false;
bool g_im3dHooksReady = false;
bool g_fxFrustumFixReady = false;
bool g_eyeTargetRoutingReady = false;
bool g_shadowTailRoutingReady = false;
bool g_areaWeaponRoutingReady = false;
bool g_flameCreateRetryReady = false;
bool g_flameLifecycleReady = false;
bool g_headlightDiagnosticsReady = false;
bool g_coronaViewSyncReady = false;
bool g_eyeBeginViewSyncReady = false;
bool g_trafficLightQualityReady = false;
bool g_coronaBillboardReady = false;
bool g_embeddedCanopyHideReady = false;
bool g_lampGroundPoolsReady = false;
bool g_wetReflectionsReady = false;
bool g_underwaterStateReady = false;
std::atomic<bool> g_skyResolveAttempted{false};
bool g_skyStateReady = false;
void** g_skyCoronaTextures = nullptr;
std::uint8_t* g_skyClockHours = nullptr;
std::uint8_t* g_skyClockMinutes = nullptr;
std::uint32_t* g_skyMoonSize = nullptr;
float* g_skyCloudCoverage = nullptr;
float* g_skyFoggyness = nullptr;
thread_local bool g_insideFxRender = false;
thread_local int g_fxEye = -1;
thread_local bool g_fxCameraPositionValid = false;
thread_local float g_fxCameraPosition[3]{};
thread_local bool g_insideLocalFlameProducer = false;
std::atomic<std::uint64_t> g_fxNearChecks{0};
std::atomic<std::uint64_t> g_fxNearRescues{0};
std::atomic<std::uint64_t> g_headlightStoreCalls{0};
std::atomic<std::uint64_t> g_headlightTextureCalls{0};
std::atomic<std::uint64_t> g_flameRoutedCalls{0};
std::atomic<std::uint64_t> g_flameFallbackCalls{0};
std::atomic<std::uint64_t> g_flameCreateAttempts{0};
std::atomic<std::uint64_t> g_flameCreateStockSuccess{0};
std::atomic<std::uint64_t> g_flameCreateRetries{0};
std::atomic<std::uint64_t> g_flameCreateRetrySuccess{0};
std::atomic<std::uint64_t> g_flameCreateFailures{0};
std::atomic<std::uint64_t> g_flameSystemPresentCalls{0};
std::atomic<std::uint64_t> g_flameSystemNewCalls{0};
std::atomic<std::uint64_t> g_flameLastRoutedMs{0};
std::atomic<std::uint64_t> g_flameLifecyclePreserves{0};
std::atomic<std::uint64_t> g_flameLifecycleKills{0};
std::atomic<std::uint64_t> g_eyeBeginViewSyncCalls{0};
std::atomic<std::uint64_t> g_trafficLightQualityRaises{0};
std::atomic<std::uint64_t> g_coronaBillboardRecords{0};
std::atomic<std::uint64_t> g_coronaBillboardDraws{0};
std::atomic<std::uint64_t> g_coronaBillboardAttachedSkips{0};
std::atomic<std::uint64_t> g_coronaBillboardSupportedRecords{0};
std::atomic<std::uint64_t> g_coronaBillboardVisibleRecords{0};
std::atomic<std::uint64_t> g_lampCandidates{0};
std::atomic<std::uint64_t> g_lampCoronaMatches{0};
std::atomic<std::uint64_t> g_lampStoreSuccess{0};
std::atomic<std::uint64_t> g_lampStoreFailures{0};
std::atomic<std::uint64_t> g_wetReflectionCalls{0};
std::atomic<std::uint64_t> g_wetReflectionCandidates{0};
std::atomic<std::uint64_t> g_wetReflectionRecords{0};
std::atomic<std::uint64_t> g_wetReflectionDraws{0};

// Traffic-light lenses and pedestrian signals are recorded into two small
// destructive retail queues.  Their Render functions clear the counts, so a
// second eye must replay the exact first-eye records rather than merely
// restoring the counters over memory that the right RenderScene has reused.
// Street-lamp and traffic-light coronas are persistent and need no snapshot.
constexpr int kEssentialLightQueueCapacity = 32;
constexpr std::size_t kBrightLightStride = 0x38;
constexpr std::size_t kShinyTextStride = 0x58;
// CSprite::CalcScreenCoors projects every corona through the cached
// TheCamera.m_mViewMatrix rather than through the active RwCamera directly.
// Retail refreshes this attached CMatrix after each RwCameraBeginUpdate.  The
// stereo eye loop owns a separate BeginUpdate for each headset view, so keep a
// scoped eye copy here and restore the centre-camera cache after the effects
// pass.  CMatrix::Update writes the twelve vector floats through offset 0x38;
// its attachment pointer at +0x40 must remain untouched.
constexpr std::size_t kCameraViewMatrixOffset = 0xa10;
constexpr std::size_t kCMatrixCachedBytes = 0x40;
constexpr std::size_t kCMatrixAttachmentOffset = 0x40;
constexpr int kCoronaCount = 64;
constexpr std::size_t kCoronaStride = 0x50;
constexpr std::size_t kCoronaIdOffset = 0x10;
constexpr std::size_t kCoronaTextureOffset = 0x18;
constexpr std::size_t kCoronaSizeOffset = 0x20;
constexpr std::size_t kCoronaFarClipOffset = 0x28;
constexpr std::size_t kCoronaPullTowardCameraOffset = 0x2c;
constexpr std::size_t kCoronaHeightAboveGroundOffset = 0x30;
constexpr std::size_t kCoronaColorOffset = 0x38;
constexpr std::size_t kCoronaRequestedIntensityOffset = 0x3b;
constexpr std::size_t kCoronaFadedIntensityOffset = 0x3c;
constexpr std::size_t kCoronaRegisteredOffset = 0x3d;
constexpr std::size_t kCoronaReflectionOffset = 0x3f;
constexpr std::size_t kCoronaFlagsOffset = 0x40;
constexpr std::size_t kCoronaMiscFlagsOffset = 0x42;
constexpr std::size_t kCoronaAttachedEntityOffset = 0x48;
constexpr int kCoronaTextureShinyStar = 0;
constexpr int kCoronaTextureHeadlight = 1;
constexpr int kCoronaTextureMoon = 2;
constexpr int kCoronaTextureReflection = 3;
constexpr int kCoronaTextureHeadlightLine = 4;
constexpr int kHydraModelId = 520;
constexpr std::uint8_t kHydraCanopyMaterialAlpha = 128;
constexpr int kHydraCanopyMaterialCapacity = 32;
int* g_numBrightLights = nullptr;
std::uint8_t* g_brightLights = nullptr;
int* g_numShinyTexts = nullptr;
std::uint8_t* g_shinyTexts = nullptr;
bool g_essentialLightStereoReady = false;
int g_brightLightSnapshotCount = 0;
int g_shinyTextSnapshotCount = 0;
std::uint8_t g_brightLightSnapshot[
    kEssentialLightQueueCapacity * kBrightLightStride]{};
std::uint8_t g_shinyTextSnapshot[
    kEssentialLightQueueCapacity * kShinyTextStride]{};
bool g_essentialLightSnapshotValid = false;
std::uint8_t* g_coronaArray = nullptr;
void** g_coronaTextures = nullptr;
float* g_weatherFoggyness = nullptr;
std::int32_t* g_mobileEffectsQuality = nullptr;
std::uint64_t g_coronaLeftSupportedMask = 0;
std::uint64_t g_coronaLeftVisibleMask = 0;
std::uint64_t g_coronaLeftIds[kCoronaCount]{};
bool g_coronaLeftSnapshotValid = false;

struct SavedCanopyMaterial {
    std::uint32_t* color{};
    std::uint32_t rgba{};
};

thread_local SavedCanopyMaterial
    g_savedHydraCanopyMaterials[kHydraCanopyMaterialCapacity]{};
thread_local int g_savedHydraCanopyMaterialCount = 0;
thread_local void* g_savedHydraCanopyClump = nullptr;

class ScopedEyeViewMatrix {
public:
    ScopedEyeViewMatrix(bool requested, int eye) {
        if (!requested || !g_coronaViewSyncReady || !g.TheCamera ||
            !g_fn.matrixUpdate) {
            return;
        }

        view_ = static_cast<std::uint8_t*>(g.TheCamera) +
            kCameraViewMatrixOffset;
        void* const attached = *reinterpret_cast<void**>(
            view_ + kCMatrixAttachmentOffset);
        if (!attached) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_acq_rel)) {
                LOGE("[gfxfx.citylight] eye view sync skipped: null attachment");
            }
            view_ = nullptr;
            return;
        }

        std::memcpy(saved_, view_, sizeof(saved_));
        g_fn.matrixUpdate(view_);
        active_ = true;

        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_acq_rel)) {
            LOGI("[gfxfx.citylight] eye view sync active eye=%d view=%p rw=%p",
                 eye, view_, attached);
        }
    }

    ~ScopedEyeViewMatrix() {
        if (active_) std::memcpy(view_, saved_, sizeof(saved_));
    }

    bool Active() const { return active_; }

private:
    std::uint8_t* view_{};
    std::uint8_t saved_[kCMatrixCachedBytes]{};
    bool active_{};
};

class ScopedTrafficLightQuality {
public:
    explicit ScopedTrafficLightQuality(bool requested) {
        if (!requested || !g_trafficLightQualityReady ||
            !g_mobileEffectsQuality) {
            return;
        }
        prior_ = *g_mobileEffectsQuality;
        if (prior_ < 2) {
            *g_mobileEffectsQuality = 2;
            raised_ = true;
        }
    }

    ~ScopedTrafficLightQuality() {
        if (raised_) *g_mobileEffectsQuality = prior_;
    }

    bool Raised() const { return raised_; }

private:
    std::int32_t prior_{};
    bool raised_{};
};

void OnRwCameraBeginUpdate(void* rwCamera) {
    if (g_origRwCameraBeginUpdate) g_origRwCameraBeginUpdate(rwCamera);
    if (!rwCamera || Profile() == 0 || !vrcam::IsStereoActive() ||
        !g_coronaViewSyncReady || !g_fn.matrixUpdate || !g.TheCamera ||
        !g_scene) {
        return;
    }

    void* const sceneCamera =
        *reinterpret_cast<void**>(g_scene + 0x08);
    if (rwCamera != sceneCamera) return;

    auto* const view = static_cast<std::uint8_t*>(g.TheCamera) +
        kCameraViewMatrixOffset;
    void* const attached = *reinterpret_cast<void**>(
        view + kCMatrixAttachmentOffset);
    void* const expected = static_cast<std::uint8_t*>(rwCamera) + 0x40;
    if (attached != expected) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_acq_rel)) {
            LOGE("[gfxfx.citylight] begin sync attachment mismatch "
                 "camera=%p attached=%p expected=%p", rwCamera, attached,
                 expected);
        }
        return;
    }

    // This runs immediately after the outer per-eye BeginUpdate, before sky
    // and RenderScene. It therefore also fixes RenderScene's internal corona
    // reflections, which the later effects-only scope cannot reach.
    g_fn.matrixUpdate(view);
    const std::uint64_t call = g_eyeBeginViewSyncCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (call == 1) {
        LOGI("[gfxfx.citylight] pre-scene eye view sync active camera=%p "
             "view=%p", rwCamera, view);
    }
}

void OnDisplayActualLight(void* entity) {
    if (!g_origDisplayActualLight) return;
    const bool ownsStereoLights = Profile() > 0 &&
        vrcam::IsStereoActive() &&
        g_flatPassSuppressed.load(std::memory_order_acquire);
    ScopedTrafficLightQuality quality(ownsStereoLights);
    g_origDisplayActualLight(entity);
    if (quality.Raised()) {
        g_trafficLightQualityRaises.fetch_add(1, std::memory_order_relaxed);
    }
}

// The PC renderer creates a persistent additive CStaticShadow for every 2DFX
// lamp that supplies a shadow texture. Retail Android still loads shadowSize
// and the per-effect texture, but its ProcessLightsForEntity omits that one
// producer call. Keep the complete mobile light path and append only the
// missing stock record, using the corona that mobile already registered to
// obtain the final world position, time-of-day colour and flash state.
void OnProcessLightsForEntity(void* entity) {
    if (!g_origProcessLights) return;
    g_origProcessLights(entity);

    if (!g_lampGroundPoolsReady || Profile() == 0 ||
        !vrcam::IsStereoActive() || !entity || !g_modelInfoPtrs ||
        !g_get2dEffect || !g_storeStaticShadow || !g_coronaArray) {
        return;
    }

    const auto* const entityBytes = static_cast<const std::uint8_t*>(entity);
    const std::uint16_t modelIndex =
        *reinterpret_cast<const std::uint16_t*>(entityBytes + 0x32);
    constexpr std::uint16_t kMaxModelInfos = 20000;
    if (modelIndex == 0xffffu || modelIndex >= kMaxModelInfos) return;

    void* const modelInfo = g_modelInfoPtrs[modelIndex];
    if (!modelInfo) return;
    const auto* const modelBytes = static_cast<const std::uint8_t*>(modelInfo);
    const std::uint8_t effectCount = modelBytes[0x27];
    if (effectCount == 0 || effectCount > 64) return;

    constexpr std::uint8_t kEffectLight = 0;
    constexpr std::uint8_t kAdditiveShadow = 2;
    for (std::int32_t i = 0; i < effectCount; ++i) {
        auto* const effect = static_cast<std::uint8_t*>(
            g_get2dEffect(modelInfo, i));
        if (!effect || effect[0x0c] != kEffectLight) continue;

        const float shadowSize =
            *reinterpret_cast<const float*>(effect + 0x20);
        void* const shadowTexture =
            *reinterpret_cast<void* const*>(effect + 0x38);
        if (!shadowTexture || !std::isfinite(shadowSize) ||
            shadowSize <= 0.01f || shadowSize > 100.0f) {
            continue;
        }
        g_lampCandidates.fetch_add(1, std::memory_order_relaxed);

        const std::uint64_t expectedId =
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(entity)) +
            static_cast<std::uint64_t>(i);
        std::uint8_t* corona = nullptr;
        for (int slot = 0; slot < kCoronaCount; ++slot) {
            auto* const record = g_coronaArray + slot * kCoronaStride;
            const std::uint64_t id =
                *reinterpret_cast<const std::uint64_t*>(
                    record + kCoronaIdOffset);
            if (id == expectedId && record[kCoronaRegisteredOffset] != 0) {
                corona = record;
                break;
            }
        }
        if (!corona) continue;
        g_lampCoronaMatches.fetch_add(1, std::memory_order_relaxed);

        const std::uint8_t faded = corona[kCoronaFadedIntensityOffset];
        const std::uint8_t requested =
            corona[kCoronaRequestedIntensityOffset];
        const int lampIntensity = std::max<int>(faded, requested);
        if (lampIntensity <= 0) continue;

        const std::uint8_t multiplier = effect[0x29];
        const auto scaleChannel = [multiplier](std::uint8_t value) {
            return static_cast<std::uint8_t>(
                (static_cast<unsigned int>(value) * multiplier) >> 8u);
        };
        const auto* const rgb = corona + kCoronaColorOffset;
        const std::uint8_t red = scaleChannel(rgb[0]);
        const std::uint8_t green = scaleChannel(rgb[1]);
        const std::uint8_t blue = scaleChannel(rgb[2]);
        if (red == 0 && green == 0 && blue == 0) continue;

        const std::int8_t rawZDistance =
            *reinterpret_cast<const std::int8_t*>(effect + 0x2a);
        const float zDistance = rawZDistance != 0
            ? static_cast<float>(rawZDistance)
            : 15.0f;
        const std::int16_t intensity = static_cast<std::int16_t>(
            std::clamp(128 * lampIntensity / 255, 1, 128));
        const bool stored = g_storeStaticShadow(
            expectedId, kAdditiveShadow, shadowTexture,
            reinterpret_cast<GameSymbols::Vec3*>(corona),
            shadowSize, 0.0f, 0.0f, -shadowSize,
            intensity, red, green, blue,
            zDistance, 1.0f, 40.0f, false, 0.0f);
        if (stored)
            g_lampStoreSuccess.fetch_add(1, std::memory_order_relaxed);
        else
            g_lampStoreFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

// CCoronas::Render is a legacy screen-space path: it projects a world point
// through CSprite::CalcScreenCoors and then bakes a pixel-space Im2D quad.  The
// projected point can be numerically correct for each eye while the resulting
// sprite still has no stable stereo world anchor.  Keep retail registration,
// LOS/fade updates and textures, but draw the useful main lamp sprites as real
// camera-facing Im3D billboards.  The pool is fixed at 64 records, and batching
// by the three relevant textures bounds this to three draws per eye.
struct CoronaV3 {
    float x{}, y{}, z{};
};

CoronaV3 operator+(CoronaV3 a, CoronaV3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

CoronaV3 operator-(CoronaV3 a, CoronaV3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

CoronaV3 operator*(CoronaV3 v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

float CoronaDot(CoronaV3 a, CoronaV3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

CoronaV3 CoronaCross(CoronaV3 a, CoronaV3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

bool CoronaFinite(CoronaV3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) &&
           std::isfinite(v.z);
}

CoronaV3 CoronaNormalized(CoronaV3 v) {
    const float lengthSq = CoronaDot(v, v);
    if (!std::isfinite(lengthSq) || lengthSq < 1.0e-8f) return {};
    return v * (1.0f / std::sqrt(lengthSq));
}

CoronaV3 ReadCoronaV3(const std::uint8_t* bytes) {
    CoronaV3 value{};
    if (bytes) std::memcpy(&value, bytes, sizeof(value));
    return value;
}

struct CoronaVertex {
    CoronaV3 objVertex;
    CoronaV3 objNormal;
    std::uint32_t color;
    float u;
    float v;
};
static_assert(sizeof(CoronaVertex) == 36,
              "RxObjSpace3DVertex layout changed");

struct CoronaEyeBasis {
    CoronaV3 left;
    CoronaV3 forward;
    CoronaV3 up;
    CoronaV3 position;
    float viewWindowX{};
    float viewWindowY{};
    float sizeScale{1.0f};
    float projectionNearDepth{1.0f};
    float projectionFarDepth{300.0f};
};

bool ReadCoronaEyeBasis(void* rwCamera, CoronaEyeBasis* out) {
    if (!rwCamera || !out || !g.TheCamera) return false;

    const auto* const camera = static_cast<const std::uint8_t*>(g.TheCamera);
    out->left = CoronaNormalized(ReadCoronaV3(camera + 0x970));
    out->forward = CoronaNormalized(ReadCoronaV3(camera + 0x980));
    out->up = CoronaNormalized(ReadCoronaV3(camera + 0x990));
    out->position = ReadCoronaV3(camera + 0x9a0);
    if (!CoronaFinite(out->left) || !CoronaFinite(out->forward) ||
        !CoronaFinite(out->up) || !CoronaFinite(out->position) ||
        CoronaDot(out->left, out->left) < 0.9f ||
        CoronaDot(out->forward, out->forward) < 0.9f ||
        CoronaDot(out->up, out->up) < 0.9f) {
        return false;
    }

    const auto* const rw = static_cast<const std::uint8_t*>(rwCamera);
    out->viewWindowX = std::abs(*reinterpret_cast<const float*>(rw + 0x90));
    out->viewWindowY = std::abs(*reinterpret_cast<const float*>(rw + 0x94));
    float fov = g.CDraw_ms_fFOV ? *g.CDraw_ms_fFOV : 70.0f;
    if (!std::isfinite(fov) || fov < 20.0f || fov > 150.0f) fov = 70.0f;

    if (!std::isfinite(out->viewWindowX) || out->viewWindowX < 0.05f ||
        out->viewWindowX > 8.0f) {
        out->viewWindowX = std::tan(
            fov * 0.5f * 3.14159265358979323846f / 180.0f);
    }
    if (!std::isfinite(out->viewWindowY) || out->viewWindowY < 0.05f ||
        out->viewWindowY > 8.0f) {
        int width = 0;
        int height = 0;
        void* const raster = *reinterpret_cast<void* const*>(rw + 0x80);
        if (raster) {
            const auto* const rasterBytes =
                static_cast<const std::uint8_t*>(raster);
            width = *reinterpret_cast<const int*>(rasterBytes + 0x18);
            height = *reinterpret_cast<const int*>(rasterBytes + 0x1c);
        }
        out->viewWindowY = width > 0 && height > 0
            ? out->viewWindowX * static_cast<float>(height) /
                  static_cast<float>(width)
            : out->viewWindowX * 0.75f;
    }

    // CalcScreenCoors uses 70/FOV for the main-corona half-size. RW's view
    // window stores tan(half-FOV); converting that pixel radius back to world
    // units gives this depth-independent multiplier.
    out->sizeScale = 2.0f * out->viewWindowX * 70.0f / fov;
    if (!std::isfinite(out->sizeScale)) out->sizeScale = 1.0f;
    out->sizeScale = std::clamp(out->sizeScale, 0.25f, 4.0f);

    const float nearClipZ = g.CDraw_ms_fNearClipZ
        ? *g.CDraw_ms_fNearClipZ
        : *reinterpret_cast<const float*>(rw + 0xa8);
    const float farClipZ = g.CDraw_ms_fFarClipZ
        ? *g.CDraw_ms_fFarClipZ
        : *reinterpret_cast<const float*>(rw + 0xac);
    out->projectionNearDepth = std::isfinite(nearClipZ)
        ? std::max(0.0f, nearClipZ + 1.0f)
        : 1.0f;
    out->projectionFarDepth = std::isfinite(farClipZ) &&
            farClipZ > out->projectionNearDepth
        ? farClipZ
        : 300.0f;
    return true;
}

int CoronaTextureSlot(void* texture) {
    if (!texture || !g_coronaTextures) return -1;
    if (texture == g_coronaTextures[kCoronaTextureShinyStar])
        return kCoronaTextureShinyStar;
    if (texture == g_coronaTextures[kCoronaTextureHeadlight])
        return kCoronaTextureHeadlight;
    if (texture == g_coronaTextures[kCoronaTextureHeadlightLine])
        return kCoronaTextureHeadlightLine;
    return -1;
}

bool ReadCoronaWorldPosition(const std::uint8_t* record,
                             CoronaV3* world,
                             bool* attachedMatrixMissing) {
    if (!record || !world) return false;
    if (attachedMatrixMissing) *attachedMatrixMissing = false;
    const CoronaV3 local = ReadCoronaV3(record);
    void* const entity = *reinterpret_cast<void* const*>(
        record + kCoronaAttachedEntityOffset);
    if (!entity) {
        *world = local;
        return CoronaFinite(*world);
    }

    const auto* const entityBytes = static_cast<const std::uint8_t*>(entity);
    const std::uint8_t entityType = entityBytes[0x5a] & 0x7u;
    const bool bike = entityType == 2 &&
        *reinterpret_cast<const std::uint32_t*>(entityBytes + 0x738) == 9u;
    const std::uint8_t* matrix = bike
        ? entityBytes + 0x7b0
        : static_cast<const std::uint8_t*>(
              *reinterpret_cast<void* const*>(entityBytes + 0x18));
    if (!matrix) {
        if (attachedMatrixMissing) *attachedMatrixMissing = true;
        return false;
    }

    const CoronaV3 right = ReadCoronaV3(matrix + 0x00);
    const CoronaV3 forward = ReadCoronaV3(matrix + 0x10);
    const CoronaV3 up = ReadCoronaV3(matrix + 0x20);
    const CoronaV3 position = ReadCoronaV3(matrix + 0x30);
    *world = position + right * local.x + forward * local.y + up * local.z;
    return CoronaFinite(*world);
}

CoronaVertex MakeCoronaVertex(CoronaV3 position, std::uint32_t color,
                              float u, float v) {
    return {position, {}, color, u, v};
}

void RestoreHydraCanopyMaterials() {
    for (int i = 0; i < g_savedHydraCanopyMaterialCount; ++i) {
        const SavedCanopyMaterial& saved = g_savedHydraCanopyMaterials[i];
        if (saved.color) *saved.color = saved.rgba;
    }
    g_savedHydraCanopyMaterialCount = 0;
    g_savedHydraCanopyClump = nullptr;
}

void* HideHydraCanopyMaterial(void* material, void*) {
    if (!material ||
        g_savedHydraCanopyMaterialCount >= kHydraCanopyMaterialCapacity) {
        return material;
    }

    auto* const color = reinterpret_cast<std::uint32_t*>(
        static_cast<std::uint8_t*>(material) + 0x08);
    const std::uint32_t rgba = *color;
    const std::uint8_t alpha = static_cast<std::uint8_t>(rgba >> 24);
    if (alpha != kHydraCanopyMaterialAlpha) return material;

    for (int i = 0; i < g_savedHydraCanopyMaterialCount; ++i) {
        if (g_savedHydraCanopyMaterials[i].color == color) return material;
    }

    g_savedHydraCanopyMaterials[g_savedHydraCanopyMaterialCount++] =
        SavedCanopyMaterial{color, rgba};
    *color = rgba & 0x00ffffffu;
    return material;
}

void* HideHydraCanopyAtomic(void* atomic, void*) {
    if (!atomic || !g_rpGeometryForAllMaterials) return atomic;
    void* const geometry = *reinterpret_cast<void**>(
        static_cast<std::uint8_t*>(atomic) + 0x30);
    if (geometry) {
        g_rpGeometryForAllMaterials(
            geometry, &HideHydraCanopyMaterial, nullptr);
    }
    return atomic;
}

void OnSetEditableMaterials(void* clump) {
    if (g_savedHydraCanopyClump && g_savedHydraCanopyClump != clump)
        RestoreHydraCanopyMaterials();
    if (g_origSetEditableMaterials) g_origSetEditableMaterials(clump);

    if (!g_embeddedCanopyHideReady || !clump ||
        !driving::IsInteriorGlassHidden() || !vrcam::IsStereoActive() ||
        !g.FindPlayerVehicle || !g_rpClumpForAllAtomics) {
        return;
    }

    void* const vehicle = g.FindPlayerVehicle(-1, false);
    if (!vehicle) return;
    const auto* const bytes = static_cast<const std::uint8_t*>(vehicle);
    const int modelId = static_cast<int>(
        *reinterpret_cast<const std::int16_t*>(bytes + 0x32));
    void* const playerClump = *reinterpret_cast<void* const*>(bytes + 0x20);
    if (modelId != kHydraModelId || playerClump != clump) return;

    g_savedHydraCanopyMaterialCount = 0;
    g_savedHydraCanopyClump = clump;
    g_rpClumpForAllAtomics(clump, &HideHydraCanopyAtomic, nullptr);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        LOGI("[gfxfx.glass] Hydra embedded canopy hidden materials=%d",
             g_savedHydraCanopyMaterialCount);
    }
}

void OnResetEditableMaterials(void* clump) {
    if (g_origResetEditableMaterials) g_origResetEditableMaterials(clump);
    if (clump && clump == g_savedHydraCanopyClump)
        RestoreHydraCanopyMaterials();
}

bool RenderStereoCoronaBillboards(void* rwCamera, int eye) {
    if (!g_coronaBillboardReady || !g_coronaArray || !g_coronaTextures ||
        !g_fn.renderStateSet || !g.RwIm3DTransform ||
        !g.RwIm3DRenderIndexedPrimitive || !g.RwIm3DEnd) {
        return false;
    }

    if (eye == 0) {
        g_coronaLeftSupportedMask = 0;
        g_coronaLeftVisibleMask = 0;
        std::memset(g_coronaLeftIds, 0, sizeof(g_coronaLeftIds));
        g_coronaLeftSnapshotValid = false;
    }

    CoronaEyeBasis basis{};
    if (!ReadCoronaEyeBasis(rwCamera, &basis)) return false;

    std::uint64_t supportedMask = 0;
    std::uint64_t visibleMask = 0;
    std::uint64_t recordsDrawn = 0;
    std::uint64_t drawCalls = 0;
    std::uint64_t attachedSkips = 0;
    const float foggyness = g_weatherFoggyness &&
            std::isfinite(*g_weatherFoggyness)
        ? std::clamp(*g_weatherFoggyness, 0.0f, 1.0f)
        : 0.0f;

    // Stock CCoronas state. CULLNONE makes the symmetric quad independent of
    // the mobile camera-left convention and of triangle winding.
    g_fn.renderStateSet(8, reinterpret_cast<void*>(0));
    g_fn.renderStateSet(12, reinterpret_cast<void*>(1));
    g_fn.renderStateSet(10, reinterpret_cast<void*>(2));
    g_fn.renderStateSet(11, reinterpret_cast<void*>(2));
    g_fn.renderStateSet(20, reinterpret_cast<void*>(1));

    constexpr int kTextureSlots[] = {
        kCoronaTextureShinyStar,
        kCoronaTextureHeadlight,
        kCoronaTextureHeadlightLine,
    };
    constexpr unsigned int kIm3DFlags = 1u | 8u | 16u; // UV | XYZ | RGBA
    constexpr int kTriangleList = 3;

    for (const int textureSlot : kTextureSlots) {
        void* const texture = g_coronaTextures[textureSlot];
        void* const raster = texture
            ? *reinterpret_cast<void* const*>(texture)
            : nullptr;
        if (!texture || !raster) continue;

        CoronaVertex vertices[kCoronaCount * 4]{};
        unsigned short indices[kCoronaCount * 6]{};
        int vertexCount = 0;
        int indexCount = 0;

        for (int i = 0; i < kCoronaCount; ++i) {
                const auto* const record =
                    g_coronaArray + i * kCoronaStride;
                const std::uint64_t id =
                    *reinterpret_cast<const std::uint64_t*>(
                        record + kCoronaIdOffset);
                // Retail reserves ids 1/2 for the sun glow/flare. The current
                // stereo scope restores world lamps, headlights and fire glow;
                // sky sprites stay owned by the separate sky path.
                if (id <= 2u) continue;
                void* const recordTexture =
                    *reinterpret_cast<void* const*>(
                        record + kCoronaTextureOffset);
                if (CoronaTextureSlot(recordTexture) != textureSlot) continue;
                const std::uint8_t requested =
                    record[kCoronaRequestedIntensityOffset];
                const std::uint8_t faded =
                    record[kCoronaFadedIntensityOffset];
                // Match retail's lifecycle gate: a currently invisible corona
                // with non-zero requested alpha must still refresh offScreen so
                // Update() can fade it back in after it re-enters either eye.
                if (faded == 0 && requested == 0) continue;
                CoronaV3 world{};
                bool attachedMatrixMissing = false;
                if (!ReadCoronaWorldPosition(record, &world,
                                             &attachedMatrixMissing)) {
                    if (attachedMatrixMissing) ++attachedSkips;
                    continue;
                }

                const std::uint64_t bit = std::uint64_t{1} << i;
                supportedMask |= bit;
                const CoronaV3 toCorona = world - basis.position;
                const float depth = CoronaDot(toCorona, basis.forward);
                const float side = CoronaDot(toCorona, basis.left);
                const float vertical = CoronaDot(toCorona, basis.up);
                const bool projectionValid = std::isfinite(depth) &&
                    std::isfinite(side) && std::isfinite(vertical) &&
                    depth > basis.projectionNearDepth &&
                    depth < basis.projectionFarDepth;
                if (projectionValid) {
                    // Stock offScreen is based on the projected centre only;
                    // sprite size, record far clip and faded intensity do not
                    // participate in this lifecycle decision.
                    if (std::abs(side) <= depth * basis.viewWindowX &&
                        std::abs(vertical) <= depth * basis.viewWindowY) {
                        visibleMask |= bit;
                    }
                }
                if (!projectionValid || faded == 0) continue;

                const float farClip = *reinterpret_cast<const float*>(
                    record + kCoronaFarClipOffset);
                const float size = *reinterpret_cast<const float*>(
                    record + kCoronaSizeOffset);
                if (!std::isfinite(farClip) || !std::isfinite(size) ||
                    farClip <= 0.1f || depth >= farClip || size <= 0.0f) {
                    continue;
                }

                const float halfWidth = std::clamp(
                    size * basis.sizeScale, 0.01f, 20.0f);
                const float fogScale = 1.0f +
                    std::min(depth, 40.0f) * foggyness / 40.0f;
                const float halfHeight = std::clamp(
                    halfWidth * fogScale, 0.01f, 30.0f);
                // The stock 2D sprite path clips a projected rectangle.  Our
                // world-space replacement submits triangles instead, so a
                // corrupt-but-finite position far outside the frustum could
                // otherwise expand into a screen-sized clipped wedge.  Keep a
                // conservative one-quad margin while rejecting only geometry
                // that cannot touch the current eye.
                if (std::abs(side) > depth * basis.viewWindowX + halfWidth ||
                    std::abs(vertical) >
                        depth * basis.viewWindowY + halfHeight) {
                    continue;
                }
                const float farFade = std::clamp(
                    2.0f - 2.0f * depth / farClip, 0.0f, 1.0f);
                const int intensity = std::clamp(
                    static_cast<int>(static_cast<float>(faded) * farFade),
                    0, 255);
                if (intensity == 0) continue;
                const auto* const rgb = record + kCoronaColorOffset;
                const auto scaleChannel = [intensity, fogScale](
                                              std::uint8_t channel) {
                    return static_cast<std::uint32_t>(std::clamp(
                        static_cast<int>(
                            (static_cast<float>(channel) * intensity / 255.0f) /
                            fogScale),
                        0, 255));
                };
                const std::uint32_t red = scaleChannel(rgb[0]);
                const std::uint32_t green = scaleChannel(rgb[1]);
                const std::uint32_t blue = scaleChannel(rgb[2]);
                const std::uint32_t color = 0xff000000u |
                    (blue << 16) | (green << 8) | red;

                float pull = *reinterpret_cast<const float*>(
                    record + kCoronaPullTowardCameraOffset);
                if (!std::isfinite(pull)) pull = 0.0f;
                pull = std::clamp(pull, -20.0f, 20.0f);
                const CoronaV3 center = world -
                    CoronaNormalized(toCorona) * pull;
                const CoronaV3 left = basis.left * halfWidth;
                const CoronaV3 up = basis.up * halfHeight;
                const CoronaV3 topLeft = center + left + up;
                const CoronaV3 topRight = center - left + up;
                const CoronaV3 bottomLeft = center + left - up;
                const CoronaV3 bottomRight = center - left - up;
                if (!CoronaFinite(center) || !CoronaFinite(topLeft) ||
                    !CoronaFinite(topRight) || !CoronaFinite(bottomLeft) ||
                    !CoronaFinite(bottomRight)) {
                    continue;
                }
                const int baseVertex = vertexCount;
                vertices[vertexCount++] = MakeCoronaVertex(
                    topLeft, color, 0.0f, 0.0f);
                vertices[vertexCount++] = MakeCoronaVertex(
                    topRight, color, 1.0f, 0.0f);
                vertices[vertexCount++] = MakeCoronaVertex(
                    bottomLeft, color, 0.0f, 1.0f);
                vertices[vertexCount++] = MakeCoronaVertex(
                    bottomRight, color, 1.0f, 1.0f);
                indices[indexCount++] =
                    static_cast<unsigned short>(baseVertex + 0);
                indices[indexCount++] =
                    static_cast<unsigned short>(baseVertex + 2);
                indices[indexCount++] =
                    static_cast<unsigned short>(baseVertex + 1);
                indices[indexCount++] =
                    static_cast<unsigned short>(baseVertex + 1);
                indices[indexCount++] =
                    static_cast<unsigned short>(baseVertex + 2);
                indices[indexCount++] =
                    static_cast<unsigned short>(baseVertex + 3);
            ++recordsDrawn;
        }

        if (vertexCount == 0) continue;
        // Retail briefly toggles this from the LOS flag, then unconditionally
        // restores ZTEST immediately before every textured main-corona draw.
        g_fn.renderStateSet(6, reinterpret_cast<void*>(1));
        g_fn.renderStateSet(1, raster);
        if (g.RwIm3DTransform(vertices,
                              static_cast<unsigned int>(vertexCount),
                              nullptr, kIm3DFlags)) {
            g.RwIm3DRenderIndexedPrimitive(
                kTriangleList, indices, indexCount);
            g.RwIm3DEnd();
            ++drawCalls;
        }
    }

    if (eye == 0) {
        g_coronaLeftSupportedMask = supportedMask;
        g_coronaLeftVisibleMask = visibleMask;
        for (int i = 0; i < kCoronaCount; ++i) {
            if ((supportedMask & (std::uint64_t{1} << i)) != 0) {
                const auto* const record =
                    g_coronaArray + i * kCoronaStride;
                g_coronaLeftIds[i] =
                    *reinterpret_cast<const std::uint64_t*>(
                        record + kCoronaIdOffset);
            }
        }
        g_coronaLeftSnapshotValid = true;
    } else {
        for (int i = 0; i < kCoronaCount; ++i) {
            const std::uint64_t bit = std::uint64_t{1} << i;
            if ((supportedMask & bit) == 0) continue;
            auto* const record = g_coronaArray + i * kCoronaStride;
            const std::uint64_t id =
                *reinterpret_cast<const std::uint64_t*>(
                    record + kCoronaIdOffset);
            const bool leftMatches = g_coronaLeftSnapshotValid &&
                (g_coronaLeftSupportedMask & bit) != 0 &&
                g_coronaLeftIds[i] == id;
            const bool visible = (visibleMask & bit) != 0 ||
                (leftMatches && (g_coronaLeftVisibleMask & bit) != 0);
            if (visible)
                record[kCoronaFlagsOffset] &= static_cast<std::uint8_t>(~0x2u);
            else
                record[kCoronaFlagsOffset] |= 0x2u;
        }
        g_coronaLeftSnapshotValid = false;
    }

    g_coronaBillboardRecords.fetch_add(recordsDrawn,
                                       std::memory_order_relaxed);
    g_coronaBillboardDraws.fetch_add(drawCalls, std::memory_order_relaxed);
    g_coronaBillboardAttachedSkips.fetch_add(attachedSkips,
                                             std::memory_order_relaxed);
    const std::uint64_t supportedCount = static_cast<std::uint64_t>(
        __builtin_popcountll(supportedMask));
    const std::uint64_t visibleCount = static_cast<std::uint64_t>(
        __builtin_popcountll(visibleMask));
    g_coronaBillboardSupportedRecords.fetch_add(
        supportedCount, std::memory_order_relaxed);
    g_coronaBillboardVisibleRecords.fetch_add(
        visibleCount, std::memory_order_relaxed);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        LOGI("[gfxfx.corona3d] active eye=%d supported=%llu visible=%llu "
             "records=%llu draws=%llu attached_skips=%llu size_scale=%.3f "
             "view=(%.3f %.3f) depth=(%.3f %.1f)",
             eye,
             static_cast<unsigned long long>(supportedCount),
             static_cast<unsigned long long>(visibleCount),
             static_cast<unsigned long long>(recordsDrawn),
             static_cast<unsigned long long>(drawCalls),
             static_cast<unsigned long long>(attachedSkips),
             static_cast<double>(basis.sizeScale),
             static_cast<double>(basis.viewWindowX),
             static_cast<double>(basis.viewWindowY),
             static_cast<double>(basis.projectionNearDepth),
             static_cast<double>(basis.projectionFarDepth));
    }
    return true;
}

// Retail 2.11 clears C3dMarker::m_bIsUsed after Render(). Preserve the 32
// active flags across the left-eye call, then restore them immediately before
// the right eye so the stock arrow model is recorded into both eye textures.
// The offsets are guarded against the exact exported retail addresses before
// any game memory is touched.
constexpr int kMarkerCount = 32;
constexpr std::size_t kMarkerStride = 0xb8;
constexpr std::size_t kMarkerUsedOffset = 0x63;
std::uint8_t* g_markerArray = nullptr;
bool g_markerStereoStateReady = false;
bool g_markerUsedSnapshot[kMarkerCount]{};
bool g_markerSnapshotValid = false;

struct Aggregate {
    std::uint64_t calls{};
    std::uint64_t leftCalls{};
    std::uint64_t rightCalls{};
    double totalWallMs{};
    double maxWallMs{};
    std::uint64_t im3dTransformCalls{};
    std::uint64_t im3dTransformLeft{};
    std::uint64_t im3dTransformRight{};
    std::uint64_t im3dTransformSuccess{};
    std::uint64_t im3dVertices{};
    std::uint64_t im3dTextureRaster{};
    std::uint64_t im3dNullTextureRaster{};
    std::uint64_t im3dPrimitiveCalls{};
    std::uint64_t im3dPrimitiveSuccess{};
    std::uint64_t probeApplications{};
    bool renderStateSampled{};
    std::uint32_t zTest{};
    std::uint32_t zWrite{};
    std::uint32_t vertexAlpha{};
    std::uint32_t srcBlend{};
    std::uint32_t dstBlend{};
    std::uint32_t alphaTest{};
    std::uint32_t alphaRef{};
    std::uint64_t targetSelectCalls{};
    std::uint64_t targetBeforeEye{};
    std::uint64_t targetBeforeBack{};
    std::uint64_t targetBeforeOther{};
    std::uint64_t targetAfterEye{};
    std::uint64_t shadowResolveCalls{};
    std::uint64_t shadowResolveStereo{};
    std::uint64_t shadowTailRouted{};
    std::uint64_t shadowTailFlatSkips{};
    std::uint64_t shadowTailFailures{};
    std::uint64_t staticShadowSlotsMax{};
    std::uint64_t headlightShadowSlotsMax{};
    std::uint64_t pointLightsMax{};
    std::uint64_t coronaCalls{};
    std::uint64_t brightLightCalls{};
    std::uint64_t shinyTextCalls{};
    std::uint64_t essentialLightCaptures{};
    std::uint64_t essentialLightRestores{};
    std::uint64_t essentialLightReplayMisses{};
    std::uint64_t brightLightSlotsMax{};
    std::uint64_t shinyTextSlotsMax{};
};

Aggregate g_aggregate{};

double MonotonicMs() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<double>(value.tv_sec) * 1000.0 +
           static_cast<double>(value.tv_nsec) / 1.0e6;
}

template <typename T>
T Resolve(void* handle, const char* name) {
    return reinterpret_cast<T>(dlsym(handle, name));
}

bool ResolveSkyState() {
    if (g_skyResolveAttempted.exchange(true, std::memory_order_acq_rel))
        return g_skyStateReady;

    void* const handle = dlopen("libGame.so", RTLD_NOLOAD | RTLD_NOW);
    if (!handle) {
        LOGE("[sky3d] libGame.so unavailable: %s", dlerror());
        return false;
    }
    void* const cloudsRender = dlsym(handle, "_ZN7CClouds6RenderEv");
    auto** const textures = static_cast<void**>(dlsym(handle, "gpCoronaTexture"));
    auto* const hours = static_cast<std::uint8_t*>(
        dlsym(handle, "_ZN6CClock18ms_nGameClockHoursE"));
    auto* const minutes = static_cast<std::uint8_t*>(
        dlsym(handle, "_ZN6CClock20ms_nGameClockMinutesE"));
    auto* const moonSize = static_cast<std::uint32_t*>(
        dlsym(handle, "_ZN8CCoronas8MoonSizeE"));
    auto* const cloudCoverage = static_cast<float*>(
        dlsym(handle, "_ZN8CWeather13CloudCoverageE"));
    auto* const foggyness = static_cast<float*>(
        dlsym(handle, "_ZN8CWeather9FoggynessE"));

    Dl_info info{};
    const bool ownerReady = cloudsRender &&
        dladdr(cloudsRender, &info) != 0 && info.dli_fbase;
    const auto base = ownerReady
        ? reinterpret_cast<std::uintptr_t>(info.dli_fbase) : 0u;
    const auto atOffset = [base](const void* pointer, std::uintptr_t offset) {
        return base != 0u && pointer &&
            reinterpret_cast<std::uintptr_t>(pointer) - base == offset;
    };
    g_skyStateReady =
        atOffset(cloudsRender, 0x5cab14u) &&
        atOffset(textures, 0xc6c788u) &&
        atOffset(hours, 0x9f9a3au) &&
        atOffset(minutes, 0x9f9a3bu) &&
        atOffset(moonSize, 0x8829d8u) &&
        atOffset(cloudCoverage, 0xcc7464u) &&
        atOffset(foggyness, 0xcc7468u);
    if (g_skyStateReady) {
        g_skyCoronaTextures = textures;
        g_skyClockHours = hours;
        g_skyClockMinutes = minutes;
        g_skyMoonSize = moonSize;
        g_skyCloudCoverage = cloudCoverage;
        g_skyFoggyness = foggyness;
    }
    LOGI("[sky3d] state_ready=%d textures=%p clock=%p/%p moon_size=%p weather=%p/%p",
         g_skyStateReady ? 1 : 0, textures, hours, minutes, moonSize,
         cloudCoverage, foggyness);
    dlclose(handle);
    return g_skyStateReady;
}

std::uint32_t PackSkyColor(int red, int green, int blue, int alpha = 255) {
    const auto channel = [](int value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0, 255));
    };
    return (channel(alpha) << 24) | (channel(blue) << 16) |
           (channel(green) << 8) | channel(red);
}

bool AppendWorldUprightSkyQuad(const CoronaV3& cameraPosition,
                              const CoronaV3& center, float halfSize,
                              std::uint32_t color,
                              CoronaVertex* vertices, int vertexCapacity,
                              unsigned short* indices, int indexCapacity,
                              int& vertexCount, int& indexCount) {
    if (!vertices || !indices || !CoronaFinite(center) ||
        !std::isfinite(halfSize) || halfSize <= 0.0f ||
        vertexCount + 4 > vertexCapacity || indexCount + 6 > indexCapacity) {
        return false;
    }
    const CoronaV3 towardCamera = CoronaNormalized(cameraPosition - center);
    CoronaV3 right = CoronaNormalized(
        CoronaCross({0.0f, 0.0f, 1.0f}, towardCamera));
    if (CoronaDot(right, right) < 0.9f) right = {1.0f, 0.0f, 0.0f};
    const CoronaV3 up = CoronaNormalized(CoronaCross(towardCamera, right));
    if (CoronaDot(up, up) < 0.9f) return false;

    const CoronaV3 horizontal = right * halfSize;
    const CoronaV3 vertical = up * halfSize;
    const int baseVertex = vertexCount;
    vertices[vertexCount++] = MakeCoronaVertex(
        center + horizontal + vertical, color, 0.0f, 0.0f);
    vertices[vertexCount++] = MakeCoronaVertex(
        center - horizontal + vertical, color, 1.0f, 0.0f);
    vertices[vertexCount++] = MakeCoronaVertex(
        center + horizontal - vertical, color, 0.0f, 1.0f);
    vertices[vertexCount++] = MakeCoronaVertex(
        center - horizontal - vertical, color, 1.0f, 1.0f);
    indices[indexCount++] = static_cast<unsigned short>(baseVertex + 0);
    indices[indexCount++] = static_cast<unsigned short>(baseVertex + 2);
    indices[indexCount++] = static_cast<unsigned short>(baseVertex + 1);
    indices[indexCount++] = static_cast<unsigned short>(baseVertex + 1);
    indices[indexCount++] = static_cast<unsigned short>(baseVertex + 2);
    indices[indexCount++] = static_cast<unsigned short>(baseVertex + 3);
    return true;
}

bool DrawSkyBatch(void* raster, CoronaVertex* vertices, int vertexCount,
                  unsigned short* indices, int indexCount) {
    if (!raster || !vertices || vertexCount <= 0 || !indices || indexCount <= 0)
        return false;
    g.RwRenderStateSet(1, raster);
    constexpr unsigned int kIm3DFlags = 1u | 8u | 16u;
    constexpr int kTriangleList = 3;
    if (!g.RwIm3DTransform(vertices, static_cast<unsigned int>(vertexCount),
                           nullptr, kIm3DFlags)) {
        return false;
    }
    g.RwIm3DRenderIndexedPrimitive(kTriangleList, indices, indexCount);
    g.RwIm3DEnd();
    return true;
}

bool RenderStereoSkyObjects(void* rwCamera, int eye) {
    if (!rwCamera || !ResolveSkyState() || !g_skyCoronaTextures ||
        !g_skyClockHours || !g_skyClockMinutes || !g.RwRenderStateSet ||
        !g.RwIm3DTransform || !g.RwIm3DRenderIndexedPrimitive ||
        !g.RwIm3DEnd) {
        return false;
    }
    const int hour = *g_skyClockHours;
    const int minute = *g_skyClockMinutes;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;

    CoronaEyeBasis basis{};
    if (!ReadCoronaEyeBasis(rwCamera, &basis)) return false;
    const float fog = g_skyFoggyness && std::isfinite(*g_skyFoggyness)
        ? std::clamp(*g_skyFoggyness, 0.0f, 1.0f) : 0.0f;
    const float clouds = g_skyCloudCoverage &&
            std::isfinite(*g_skyCloudCoverage)
        ? std::clamp(*g_skyCloudCoverage, 0.0f, 1.0f) : 0.0f;
    const float clearScale = 1.0f - std::max(fog, clouds);

    CoronaVertex moonVertices[4]{};
    unsigned short moonIndices[6]{};
    int moonVertexCount = 0, moonIndexCount = 0;
    void* moonRaster = nullptr;
    const int minutesToday = hour * 60 + minute;
    const int minutesFromMidnight = std::min(
        minutesToday, 24 * 60 - minutesToday);
    if (minutesFromMidnight < 220 && clearScale > 0.001f) {
        void* const moonTexture = g_skyCoronaTextures[kCoronaTextureMoon];
        moonRaster = moonTexture
            ? *reinterpret_cast<void* const*>(moonTexture) : nullptr;
        const int rawBlue = 220 - minutesFromMidnight;
        const int redGreen = static_cast<int>(rawBlue * clearScale);
        const int blue = static_cast<int>(rawBlue * 0.85f * clearScale);
        const float moonHalfSize = std::clamp(
            static_cast<float>(*g_skyMoonSize) * 2.0f + 4.0f,
            4.0f, 20.0f);
        AppendWorldUprightSkyQuad(
            basis.position,
            basis.position + CoronaV3{0.0f, -100.0f, 15.0f},
            moonHalfSize, PackSkyColor(redGreen, redGreen, blue),
            moonVertices, 4, moonIndices, 6,
            moonVertexCount, moonIndexCount);
    }

    constexpr float kStarY[9] = {
        0.00f, 0.05f, 0.13f, 0.40f, 0.70f, 0.60f, 0.27f, 0.55f, 0.75f};
    constexpr float kStarZ[9] = {
        0.00f, 0.45f, 0.90f, 1.00f, 0.85f, 0.52f, 0.48f, 0.35f, 0.20f};
    constexpr float kStarSize[9] = {
        1.00f, 1.40f, 0.90f, 1.00f, 0.60f, 1.50f, 1.30f, 1.00f, 0.80f};
    constexpr float kStarBrightness[11] = {
        0.94f, 0.77f, 0.86f, 0.68f, 0.91f, 0.73f,
        0.82f, 0.96f, 0.71f, 0.79f, 0.88f};
    CoronaVertex starVertices[12 * 4]{};
    unsigned short starIndices[12 * 6]{};
    int starVertexCount = 0, starIndexCount = 0;
    void* starRaster = nullptr;
    if ((hour >= 22 || hour < 6) && clearScale > 0.001f) {
        const int fadeMinutes = hour == 22 ? minute
            : (hour == 5 ? 60 - minute : 60);
        const int baseBrightness = 255 * fadeMinutes / 60;
        void* const starTexture =
            g_skyCoronaTextures[kCoronaTextureShinyStar];
        starRaster = starTexture
            ? *reinterpret_cast<void* const*>(starTexture) : nullptr;
        for (int i = 0; i < 11; ++i) {
            const int position = i % 9;
            const CoronaV3 offset{
                i >= 9 ? -100.0f : 100.0f,
                -90.0f * kStarY[position],
                10.0f + 80.0f * kStarZ[position]};
            const int brightness = static_cast<int>(
                baseBrightness * clearScale * kStarBrightness[i]);
            AppendWorldUprightSkyQuad(
                basis.position, basis.position + offset,
                0.8f * kStarSize[position],
                PackSkyColor(brightness, brightness, brightness),
                starVertices, 12 * 4, starIndices, 12 * 6,
                starVertexCount, starIndexCount);
        }
        const int bigBrightness = static_cast<int>(
            baseBrightness * clearScale * 0.42f);
        AppendWorldUprightSkyQuad(
            basis.position,
            basis.position + CoronaV3{100.0f, -90.0f, 10.0f},
            5.0f, PackSkyColor(bigBrightness, bigBrightness, bigBrightness),
            starVertices, 12 * 4, starIndices, 12 * 6,
            starVertexCount, starIndexCount);
    }

    if (moonVertexCount == 0 && starVertexCount == 0) return false;
    // Same additive/depth-less setup as retail CClouds::Render, but the CPU
    // screen projection is gone: these are real world-oriented Im3D quads.
    g.RwRenderStateSet(6, reinterpret_cast<void*>(0));
    g.RwRenderStateSet(8, reinterpret_cast<void*>(0));
    g.RwRenderStateSet(12, reinterpret_cast<void*>(1));
    g.RwRenderStateSet(10, reinterpret_cast<void*>(2));
    g.RwRenderStateSet(11, reinterpret_cast<void*>(2));
    g.RwRenderStateSet(14, reinterpret_cast<void*>(0));
    g.RwRenderStateSet(20, reinterpret_cast<void*>(1));
    const bool moonDrawn = DrawSkyBatch(
        moonRaster, moonVertices, moonVertexCount, moonIndices, moonIndexCount);
    const bool starsDrawn = DrawSkyBatch(
        starRaster, starVertices, starVertexCount, starIndices, starIndexCount);

    // RenderScene and the opaque weapon path must never inherit sky blend/depth
    // state. DefinedState is safe here, before any world geometry is recorded.
    g.RwRenderStateSet(1, nullptr);
    if (g.DefinedState) {
        g.DefinedState();
    } else {
        g.RwRenderStateSet(10, reinterpret_cast<void*>(5));
        g.RwRenderStateSet(11, reinterpret_cast<void*>(6));
        g.RwRenderStateSet(12, reinterpret_cast<void*>(0));
        g.RwRenderStateSet(8, reinterpret_cast<void*>(1));
        g.RwRenderStateSet(6, reinterpret_cast<void*>(1));
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        LOGI("[sky3d] active eye=%d moon=%d stars=%d time=%02d:%02d clear=%.2f",
             eye, moonDrawn ? 1 : 0, starsDrawn ? 1 : 0,
             hour, minute, static_cast<double>(clearScale));
    }
    return moonDrawn || starsDrawn;
}

int FxProbeMode() {
    static const int mode = [] {
        char text[PROP_VALUE_MAX]{};
        int value = 0;
        if (__system_property_get(kFxProbeProperty, text) > 0) {
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(text, &end, 10);
            if (errno == 0 && end != text && end && *end == '\0' &&
                parsed >= 0 && parsed <= 3) {
                value = static_cast<int>(parsed);
            } else {
                LOGW("[gfxfx.probe] ignoring invalid %s=%s (valid 0..3)",
                     kFxProbeProperty, text);
            }
        }
        LOGI("[gfxfx.probe] process mode=%d property=%s", value,
             kFxProbeProperty);
        return value;
    }();
    return mode;
}

bool ReadRenderState(int state, std::uint32_t& value) {
    value = 0;
    return g_fn.renderStateGet && g_fn.renderStateGet(state, &value);
}

struct RenderStateSnapshot {
    void* textureRaster{};
    std::uint32_t zTest{};
    std::uint32_t zWrite{};
    std::uint32_t srcBlend{};
    std::uint32_t dstBlend{};
    std::uint32_t vertexAlpha{};
    std::uint32_t fog{};
    std::uint32_t cull{};
    std::uint32_t stencil{};
    std::uint32_t alphaTest{};
    std::uint32_t alphaRef{};
    bool valid{};
};

RenderStateSnapshot CaptureRenderState() {
    RenderStateSnapshot snapshot{};
    snapshot.valid = g_fn.renderStateGet &&
        g_fn.renderStateGet(1, &snapshot.textureRaster) &&
        ReadRenderState(6, snapshot.zTest) &&
        ReadRenderState(8, snapshot.zWrite) &&
        ReadRenderState(10, snapshot.srcBlend) &&
        ReadRenderState(11, snapshot.dstBlend) &&
        ReadRenderState(12, snapshot.vertexAlpha) &&
        ReadRenderState(14, snapshot.fog) &&
        ReadRenderState(20, snapshot.cull) &&
        ReadRenderState(21, snapshot.stencil) &&
        ReadRenderState(29, snapshot.alphaTest) &&
        ReadRenderState(30, snapshot.alphaRef);
    return snapshot;
}

void RestoreRenderState(const RenderStateSnapshot& snapshot) {
    if (!snapshot.valid || !g_fn.renderStateSet) return;
    g_fn.renderStateSet(1, snapshot.textureRaster);
    g_fn.renderStateSet(6, reinterpret_cast<void*>(snapshot.zTest));
    g_fn.renderStateSet(8, reinterpret_cast<void*>(snapshot.zWrite));
    g_fn.renderStateSet(10, reinterpret_cast<void*>(snapshot.srcBlend));
    g_fn.renderStateSet(11, reinterpret_cast<void*>(snapshot.dstBlend));
    g_fn.renderStateSet(12, reinterpret_cast<void*>(snapshot.vertexAlpha));
    g_fn.renderStateSet(14, reinterpret_cast<void*>(snapshot.fog));
    g_fn.renderStateSet(20, reinterpret_cast<void*>(snapshot.cull));
    g_fn.renderStateSet(21, reinterpret_cast<void*>(snapshot.stencil));
    g_fn.renderStateSet(29, reinterpret_cast<void*>(snapshot.alphaTest));
    g_fn.renderStateSet(30, reinterpret_cast<void*>(snapshot.alphaRef));
}

void ApplyFxVisibilityProbe(int mode) {
    if (mode <= 0 || !g_fn.renderStateSet) return;

    // These overrides are scoped to the moment immediately before each Fx
    // Im3D batch is transformed. Emitters establish their own states again for
    // the next batch, and DefinedState restores the normal baseline on the next
    // eye. This gives three progressively stronger A/Bs without another APK:
    // 1: keep the stock texture/blend, bypass depth + alpha rejection.
    // 2: keep the texture, additionally force opaque source visibility.
    // 3: remove the texture and draw opaque vertex-coloured geometry.
    g_fn.renderStateSet(6, nullptr);  // rwRENDERSTATEZTESTENABLE
    g_fn.renderStateSet(8, nullptr);  // rwRENDERSTATEZWRITEENABLE
    g_fn.renderStateSet(14, nullptr); // rwRENDERSTATEFOGENABLE
    g_fn.renderStateSet(20, reinterpret_cast<void*>(1)); // cull none
    g_fn.renderStateSet(21, nullptr); // rwRENDERSTATESTENCILENABLE
    g_fn.renderStateSet(29, reinterpret_cast<void*>(8));
    g_fn.renderStateSet(30, nullptr);
    if (mode >= 2) {
        g_fn.renderStateSet(12, reinterpret_cast<void*>(1));
        g_fn.renderStateSet(10, reinterpret_cast<void*>(2)); // rwBLENDONE
        g_fn.renderStateSet(11, reinterpret_cast<void*>(1)); // rwBLENDZERO
    }
    if (mode >= 3) {
        g_fn.renderStateSet(1, nullptr);
        g_fn.renderStateSet(12, nullptr);
        g_fn.renderStateSet(10, reinterpret_cast<void*>(2)); // rwBLENDONE
        g_fn.renderStateSet(11, reinterpret_cast<void*>(1)); // rwBLENDZERO
    }
}

bool ResolveEyeTargetRouting(void* handle) {
    g_rqRenderTargetSelect = Resolve<RqRenderTargetSelectFn>(
        handle, "_ZN14RQRenderTarget6SelectEPS_b");
    g_rasterExtOffset = Resolve<int*>(handle, "RasterExtOffset");
    g_rqSelectedTarget = Resolve<void**>(
        handle, "_ZN14RQRenderTarget8selectedE");
    g_backTarget = Resolve<void**>(handle, "backTarget");

    Dl_info info{};
    if (!g_fn.fxRender || !g_rqRenderTargetSelect || !g_rasterExtOffset ||
        !g_rqSelectedTarget || !g_backTarget ||
        dladdr(reinterpret_cast<void*>(g_fn.fxRender), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.target] disabled: missing retail symbols");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool offsetsMatch =
        reinterpret_cast<std::uintptr_t>(g_rqRenderTargetSelect) - base ==
            0x7941c4u &&
        reinterpret_cast<std::uintptr_t>(g_rasterExtOffset) - base ==
            0xd11818u &&
        reinterpret_cast<std::uintptr_t>(g_rqSelectedTarget) - base ==
            0xd13b88u &&
        reinterpret_cast<std::uintptr_t>(g_backTarget) - base == 0xd0e4f8u;
    static constexpr std::uint32_t kSelectEntry[4] = {
        0xd10143ffu, 0xa9027bfdu, 0xf9001bf5u, 0xa9044ff4u,
    };
    std::uint32_t observedEntry[4]{};
    std::memcpy(observedEntry,
                reinterpret_cast<const void*>(g_rqRenderTargetSelect),
                sizeof(observedEntry));
    const bool fingerprintMatch =
        std::memcmp(observedEntry, kSelectEntry, sizeof(observedEntry)) == 0;
    const bool ready = offsetsMatch && fingerprintMatch;
    LOGI("[gfxfx.target] ready=%d offsets=%d fingerprint=%d ext=%p "
         "selected_slot=%p back_slot=%p",
         ready ? 1 : 0, offsetsMatch ? 1 : 0,
         fingerprintMatch ? 1 : 0, g_rasterExtOffset,
         g_rqSelectedTarget, g_backTarget);
    return ready;
}

void* CurrentCameraRasterTarget(void* rwCamera) {
    if (!rwCamera || !g_rasterExtOffset) return nullptr;
    const int extOffset = *g_rasterExtOffset;
    if (extOffset <= 0 || extOffset > 0x1000) return nullptr;
    void* const raster = *reinterpret_cast<void**>(
        static_cast<std::uint8_t*>(rwCamera) + 0x80);
    if (!raster) return nullptr;
    return *reinterpret_cast<void**>(
        static_cast<std::uint8_t*>(raster) + extOffset + 0x20);
}

bool SelectCurrentCameraRasterTarget(void* rwCamera, int eye) {
    if (!g_eyeTargetRoutingReady || !g_rqSelectedTarget || !g_backTarget ||
        !g_rqRenderTargetSelect) {
        return false;
    }

    const int extOffset = *g_rasterExtOffset;
    void* const eyeTarget = CurrentCameraRasterTarget(rwCamera);
    if (!eyeTarget) return false;

    void* const before = *g_rqSelectedTarget;
    void* const back = *g_backTarget;
    ++g_aggregate.targetSelectCalls;
    if (before == eyeTarget)
        ++g_aggregate.targetBeforeEye;
    else if (before == back)
        ++g_aggregate.targetBeforeBack;
    else
        ++g_aggregate.targetBeforeOther;

    // ResolveShadowTarget at the tail of retail RenderScene deliberately
    // selects the flat Android backTarget without closing currentCamera. Stock
    // RenderEffects expects a later MobileRender composite; the Quest eye pass
    // has no such composite, so reselect the current camera raster before any
    // late Im3D work. Select queues command 27 and its replay also restores the
    // target-sized viewport. EndUpdate still returns to its saved outer target.
    if (before != eyeTarget) g_rqRenderTargetSelect(eyeTarget, false);
    const bool selectedEye = *g_rqSelectedTarget == eyeTarget;
    if (selectedEye) ++g_aggregate.targetAfterEye;

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        LOGI("[gfxfx.target] eye=%d ext=%d before=%p eye_rt=%p back=%p "
             "after=%p selected_eye=%d",
             eye, extOffset, before, eyeTarget, back,
             *g_rqSelectedTarget, selectedEye ? 1 : 0);
    }
    return selectedEye;
}

bool RenderStereoWetReflections(void* rwCamera) {
    if (!g_wetReflectionsReady || !rwCamera || !g_weatherWetRoads ||
        !g_coronaArray || !g_coronaTextures || !g_fn.renderStateSet ||
        !g.RwIm3DTransform || !g.RwIm3DRenderIndexedPrimitive ||
        !g.RwIm3DEnd) {
        return false;
    }

    const float wetRoads = std::isfinite(*g_weatherWetRoads)
        ? std::clamp(*g_weatherWetRoads, 0.0f, 1.0f)
        : 0.0f;
    if (wetRoads <= 0.001f) return false;

    void* const texture = g_coronaTextures[kCoronaTextureReflection];
    void* const raster = texture
        ? *reinterpret_cast<void* const*>(texture)
        : nullptr;
    if (!raster) return false;

    CoronaEyeBasis basis{};
    if (!ReadCoronaEyeBasis(rwCamera, &basis)) return false;

    CoronaVertex vertices[kCoronaCount * 4]{};
    unsigned short indices[kCoronaCount * 6]{};
    int vertexCount = 0;
    int indexCount = 0;
    std::uint64_t candidates = 0;
    std::uint64_t records = 0;

    for (int i = 0; i < kCoronaCount; ++i) {
        const auto* const record = g_coronaArray + i * kCoronaStride;
        const std::uint64_t id =
            *reinterpret_cast<const std::uint64_t*>(
                record + kCoronaIdOffset);
        if (id == 0 || record[kCoronaRegisteredOffset] == 0 ||
            record[kCoronaReflectionOffset] == 0) {
            continue;
        }
        ++candidates;

        // Stock RenderReflections has just refreshed this cached collision
        // height. Bit 2 is the retail valid-height flag; never invent a ground
        // plane for records whose ray has not completed yet.
        if ((record[kCoronaMiscFlagsOffset] & 0x04u) == 0) continue;
        const float height = *reinterpret_cast<const float*>(
            record + kCoronaHeightAboveGroundOffset);
        if (!std::isfinite(height) || height < 0.0f || height >= 20.0f)
            continue;

        CoronaV3 world{};
        if (!ReadCoronaWorldPosition(record, &world, nullptr)) continue;
        const float groundZ = world.z - height;
        if (!std::isfinite(groundZ) || groundZ >= basis.position.z) continue;

        CoronaV3 center = world;
        center.z -= 2.0f * height;
        const CoronaV3 toReflection = center - basis.position;
        const float depth = CoronaDot(toReflection, basis.forward);
        const float distanceSq = CoronaDot(toReflection, toReflection);
        const float farClip = *reinterpret_cast<const float*>(
            record + kCoronaFarClipOffset);
        if (!std::isfinite(depth) || !std::isfinite(distanceSq) ||
            !std::isfinite(farClip) || depth <= basis.projectionNearDepth ||
            distanceSq <= 0.0f) {
            continue;
        }
        const float drawDistance = std::min(55.0f, farClip * 0.75f);
        if (drawDistance <= 0.1f || distanceSq >= drawDistance * drawDistance)
            continue;

        const float size = *reinterpret_cast<const float*>(
            record + kCoronaSizeOffset);
        if (!std::isfinite(size) || size <= 0.0f) continue;
        const float halfWidth = std::clamp(
            size * basis.sizeScale * 0.75f, 0.02f, 20.0f);
        const float halfHeight = std::clamp(
            size * basis.sizeScale * 2.0f, 0.04f, 30.0f);
        const float side = CoronaDot(toReflection, basis.left);
        const float vertical = CoronaDot(toReflection, basis.up);
        if (std::abs(side) > depth * basis.viewWindowX + halfWidth ||
            std::abs(vertical) > depth * basis.viewWindowY + halfHeight) {
            continue;
        }

        const float heightFade = 1.0f - height / 20.0f;
        const float farFade = std::clamp(
            1.0f - std::sqrt(distanceSq) / drawDistance, 0.0f, 1.0f);
        const float intensity = wetRoads * heightFade * farFade *
            (static_cast<float>(record[kCoronaFadedIntensityOffset]) / 255.0f) *
            (230.0f / 256.0f);
        if (intensity <= 0.002f) continue;
        const auto* const rgb = record + kCoronaColorOffset;
        const auto scaleChannel = [intensity](std::uint8_t value) {
            return static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(static_cast<float>(value) * intensity),
                0, 255));
        };
        const std::uint32_t red = scaleChannel(rgb[0]);
        const std::uint32_t green = scaleChannel(rgb[1]);
        const std::uint32_t blue = scaleChannel(rgb[2]);
        if ((red | green | blue) == 0) continue;
        const std::uint32_t color = 0xff000000u |
            (blue << 16) | (green << 8) | red;

        const CoronaV3 left = basis.left * halfWidth;
        const CoronaV3 up = basis.up * halfHeight;
        const int baseVertex = vertexCount;
        vertices[vertexCount++] = MakeCoronaVertex(
            center + left + up, color, 0.0f, 0.0f);
        vertices[vertexCount++] = MakeCoronaVertex(
            center - left + up, color, 1.0f, 0.0f);
        vertices[vertexCount++] = MakeCoronaVertex(
            center + left - up, color, 0.0f, 1.0f);
        vertices[vertexCount++] = MakeCoronaVertex(
            center - left - up, color, 1.0f, 1.0f);
        indices[indexCount++] = static_cast<unsigned short>(baseVertex + 0);
        indices[indexCount++] = static_cast<unsigned short>(baseVertex + 2);
        indices[indexCount++] = static_cast<unsigned short>(baseVertex + 1);
        indices[indexCount++] = static_cast<unsigned short>(baseVertex + 1);
        indices[indexCount++] = static_cast<unsigned short>(baseVertex + 2);
        indices[indexCount++] = static_cast<unsigned short>(baseVertex + 3);
        ++records;
    }

    g_wetReflectionCandidates.fetch_add(candidates, std::memory_order_relaxed);
    if (vertexCount == 0) return false;

    g_fn.renderStateSet(1, raster);
    g_fn.renderStateSet(6, reinterpret_cast<void*>(0));
    g_fn.renderStateSet(8, reinterpret_cast<void*>(0));
    g_fn.renderStateSet(10, reinterpret_cast<void*>(2));
    g_fn.renderStateSet(11, reinterpret_cast<void*>(2));
    g_fn.renderStateSet(12, reinterpret_cast<void*>(1));
    g_fn.renderStateSet(14, reinterpret_cast<void*>(0));

    constexpr unsigned int kIm3DFlags = 1u | 8u | 16u;
    constexpr int kTriangleList = 3;
    bool drawn = false;
    if (g.RwIm3DTransform(vertices,
                          static_cast<unsigned int>(vertexCount),
                          nullptr, kIm3DFlags)) {
        g.RwIm3DRenderIndexedPrimitive(kTriangleList, indices, indexCount);
        g.RwIm3DEnd();
        drawn = true;
    }
    // RenderEverythingBarRoads follows this hook immediately and assumes the
    // exact postcondition left by retail CCoronas::RenderReflections.  Do not
    // use the generic all-or-nothing snapshot here: some GLES states cannot be
    // queried on this backend, making that restore silently skip everything.
    // Replay the five retail tail writes unconditionally so wet reflections
    // can never leak depth/additive state into the walls pass.
    g_fn.renderStateSet(10, reinterpret_cast<void*>(5));
    g_fn.renderStateSet(11, reinterpret_cast<void*>(6));
    g_fn.renderStateSet(12, reinterpret_cast<void*>(0));
    g_fn.renderStateSet(8, reinterpret_cast<void*>(1));
    g_fn.renderStateSet(6, reinterpret_cast<void*>(1));

    if (drawn) {
        g_wetReflectionRecords.fetch_add(records, std::memory_order_relaxed);
        g_wetReflectionDraws.fetch_add(1, std::memory_order_relaxed);
    }
    return drawn;
}

void OnRenderReflections() {
    if (!g_origRenderReflections) return;

    // Preserve the retail scan/collision cache. Its screen-space sprites are
    // retained as an A/B baseline; on Quest they are currently not visible,
    // while the stereo replacement below is a real world-anchored batch.
    g_origRenderReflections();
    g_wetReflectionCalls.fetch_add(1, std::memory_order_relaxed);

    if (!g_wetReflectionsReady || Profile() == 0 ||
        !vrcam::IsStereoActive() || !g_scene || !g_rqSelectedTarget ||
        !g_backTarget) {
        return;
    }
    void* const camera = *reinterpret_cast<void**>(g_scene + 0x08);
    void* const eyeTarget = CurrentCameraRasterTarget(camera);
    if (!camera || !eyeTarget || eyeTarget == *g_backTarget ||
        *g_rqSelectedTarget != eyeTarget) {
        return;
    }
    RenderStereoWetReflections(camera);
}

void SampleHeadlightQueues() {
    if (!g_headlightDiagnosticsReady || !g_staticShadows ||
        !g_shadowHeadLightsTex || !g_shadowHeadLightsTex2 ||
        !g_numPointLights) {
        return;
    }

    // Retail ARM64 CStaticShadow is 0x50 bytes. StoreStaticShadow identifies a
    // live slot by m_pPolyBunch (+0x08); texture and type are +0x38/+0x42.
    // All addresses and the owning libGame build are RVA-guarded at install.
    constexpr std::size_t kStaticShadowCount = 48;
    constexpr std::size_t kStaticShadowStride = 0x50;
    constexpr std::size_t kPolyBunchOffset = 0x08;
    constexpr std::size_t kTextureOffset = 0x38;
    constexpr std::size_t kTypeOffset = 0x42;
    constexpr std::uint8_t kAdditiveShadow = 2;
    const void* const headlight = *g_shadowHeadLightsTex;
    const void* const headlight2 = *g_shadowHeadLightsTex2;
    std::uint64_t active = 0;
    std::uint64_t headlights = 0;
    for (std::size_t i = 0; i < kStaticShadowCount; ++i) {
        const std::uint8_t* const slot =
            g_staticShadows + i * kStaticShadowStride;
        if (!*reinterpret_cast<void* const*>(slot + kPolyBunchOffset))
            continue;
        ++active;
        const void* const texture =
            *reinterpret_cast<void* const*>(slot + kTextureOffset);
        const std::uint8_t type = *(slot + kTypeOffset);
        if (type == kAdditiveShadow &&
            (texture == headlight || texture == headlight2)) {
            ++headlights;
        }
    }
    g_aggregate.staticShadowSlotsMax =
        std::max(g_aggregate.staticShadowSlotsMax, active);
    g_aggregate.headlightShadowSlotsMax =
        std::max(g_aggregate.headlightShadowSlotsMax, headlights);
    const int pointLights = *g_numPointLights;
    if (pointLights >= 0 && pointLights <= 32) {
        g_aggregate.pointLightsMax = std::max(
            g_aggregate.pointLightsMax,
            static_cast<std::uint64_t>(pointLights));
    }
}

void OnResolveShadowTarget(float strength) {
    if (!g_origResolveShadowTarget) return;
    g_origResolveShadowTarget(strength);
    ++g_aggregate.shadowResolveCalls;
    SampleHeadlightQueues();

    // Retail renders blood pools, headlights and explosion shadows in the
    // `true` shadow pass immediately after ResolveShadowTarget. The resolver
    // leaves the producer on the flat Android backTarget, so restore the
    // current XR camera target before that existing pass begins. A flat camera
    // resolves back to the same target and is deliberately left untouched.
    if (Profile() == 0 || !savr::vrcam::IsStereoActive() || !g_scene ||
        !g_backTarget) {
        return;
    }
    ++g_aggregate.shadowResolveStereo;
    void* const camera = *reinterpret_cast<void**>(g_scene + 0x08);
    void* const cameraTarget = CurrentCameraRasterTarget(camera);
    void* const back = *g_backTarget;
    if (!camera || !cameraTarget || !back) {
        ++g_aggregate.shadowTailFailures;
        return;
    }
    if (cameraTarget == back) {
        ++g_aggregate.shadowTailFlatSkips;
        return;
    }

    const bool routed = SelectCurrentCameraRasterTarget(camera, -1);
    if (routed)
        ++g_aggregate.shadowTailRouted;
    else
        ++g_aggregate.shadowTailFailures;

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        LOGI("[gfxfx.blood] shadow_tail camera=%p eye_rt=%p back=%p "
             "routed=%d",
             camera, cameraTarget, back, routed ? 1 : 0);
    }
}

void OnStoreCarLightShadow(
    void* vehicle, std::int32_t id, void* texture,
    GameSymbols::Vec3* position,
    float frontX, float frontY, float sideX, float sideY,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue,
    float maxViewAngle) {
    g_headlightStoreCalls.fetch_add(1, std::memory_order_relaxed);
    if (texture &&
        ((g_shadowHeadLightsTex && texture == *g_shadowHeadLightsTex) ||
        (g_shadowHeadLightsTex2 && texture == *g_shadowHeadLightsTex2))) {
        g_headlightTextureCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_origStoreCarLightShadow) {
        g_origStoreCarLightShadow(
            vehicle, id, texture, position,
            frontX, frontY, sideX, sideY,
            red, green, blue, maxViewAngle);
    }
}

bool InstallRetailGotHook(const char* label,
                          std::uintptr_t base,
                          std::uintptr_t symbolOffset,
                          std::uintptr_t pltOffset,
                          std::uintptr_t gotOffset,
                          const std::uint32_t expectedPlt[4],
                          void* resolvedFunction,
                          void* replacement) {
    if (!base || !resolvedFunction || !replacement ||
        reinterpret_cast<std::uintptr_t>(resolvedFunction) - base !=
            symbolOffset) {
        LOGW("[gfxfx.im3d] %s disabled: retail symbol mismatch", label);
        return false;
    }

    std::uint32_t observedPlt[4]{};
    std::memcpy(observedPlt, reinterpret_cast<const void*>(base + pltOffset),
                sizeof(observedPlt));
    if (std::memcmp(observedPlt, expectedPlt, sizeof(observedPlt)) != 0) {
        LOGW("[gfxfx.im3d] %s disabled: PLT fingerprint mismatch", label);
        return false;
    }

    auto* const slot = reinterpret_cast<std::uintptr_t*>(base + gotOffset);
    const std::uintptr_t direct =
        reinterpret_cast<std::uintptr_t>(resolvedFunction);
    const std::uintptr_t hook = reinterpret_cast<std::uintptr_t>(replacement);
    static constexpr std::uintptr_t kRetailLazyResolverOffset = 0x7f4680u;
    static constexpr std::uint32_t kRetailLazyResolver[4] = {
        0xa9bf7bf0u, 0xd0000250u, 0xf946c611u, 0x91362210u,
    };
    const std::uintptr_t lazyResolver = base + kRetailLazyResolverOffset;
    std::uintptr_t observed = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (observed == hook) return true;
    const bool directSlot = observed == direct;
    bool lazySlot = observed == lazyResolver;
    if (lazySlot) {
        std::uint32_t observedResolver[4]{};
        std::memcpy(observedResolver,
                    reinterpret_cast<const void*>(lazyResolver),
                    sizeof(observedResolver));
        lazySlot = std::memcmp(observedResolver, kRetailLazyResolver,
                               sizeof(observedResolver)) == 0;
    }
    if (!directSlot && !lazySlot) {
        LOGW("[gfxfx.im3d] %s disabled: GOT owned observed=%p", label,
             reinterpret_cast<void*>(observed));
        return false;
    }

    const long rawPageSize = sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) return false;
    const auto pageSize = static_cast<std::uintptr_t>(rawPageSize);
    const std::uintptr_t page =
        reinterpret_cast<std::uintptr_t>(slot) & ~(pageSize - 1u);
    if (mprotect(reinterpret_cast<void*>(page), pageSize,
                 PROT_READ | PROT_WRITE) != 0) {
        LOGW("[gfxfx.im3d] %s disabled: GOT mprotect RW failed", label);
        return false;
    }

    std::uintptr_t expected = observed;
    const bool swapped = __atomic_compare_exchange_n(
        slot, &expected, hook, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    const bool readbackOk = swapped &&
        __atomic_load_n(slot, __ATOMIC_ACQUIRE) == hook;
    if (!readbackOk) {
        mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ);
        LOGW("[gfxfx.im3d] %s disabled: guarded GOT swap lost", label);
        return false;
    }
    if (mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ) != 0) {
        if (mprotect(reinterpret_cast<void*>(page), pageSize,
                     PROT_READ | PROT_WRITE) == 0) {
            std::uintptr_t rollbackExpected = hook;
            __atomic_compare_exchange_n(
                slot, &rollbackExpected, observed, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ);
        }
        LOGW("[gfxfx.im3d] %s disabled: RELRO restore failed", label);
        return false;
    }

    LOGI("[gfxfx.im3d] %s active got=%p prior=%s", label, slot,
         lazySlot ? "lazy" : "direct");
    return true;
}

bool InstallEmbeddedCanopyHide(void* handle) {
    g_origSetEditableMaterials = Resolve<EditVehicleMaterialsFn>(
        handle, "_ZN17CVehicleModelInfo20SetEditableMaterialsEP7RpClump");
    g_origResetEditableMaterials = Resolve<EditVehicleMaterialsFn>(
        handle, "_ZN17CVehicleModelInfo22ResetEditableMaterialsEP7RpClump");
    g_rpClumpForAllAtomics = Resolve<RpClumpForAllAtomicsFn>(
        handle,
        "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_");
    g_rpGeometryForAllMaterials = Resolve<RpGeometryForAllMaterialsFn>(
        handle,
        "_Z25RpGeometryForAllMaterialsP10RpGeometryPFP10RpMaterialS2_PvES3_");

    Dl_info info{};
    if (!g_origSetEditableMaterials || !g_origResetEditableMaterials ||
        !g_rpClumpForAllAtomics || !g_rpGeometryForAllMaterials ||
        dladdr(reinterpret_cast<void*>(g_origSetEditableMaterials), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.glass] embedded canopy hide disabled: missing symbols");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool exactLayout =
        reinterpret_cast<std::uintptr_t>(g_origSetEditableMaterials) - base ==
            0x535888u &&
        reinterpret_cast<std::uintptr_t>(g_origResetEditableMaterials) - base ==
            0x5358ecu &&
        reinterpret_cast<std::uintptr_t>(g_rpClumpForAllAtomics) - base ==
            0x746e20u &&
        reinterpret_cast<std::uintptr_t>(g_rpGeometryForAllMaterials) - base ==
            0x749d7cu;
    if (!exactLayout) {
        LOGW("[gfxfx.glass] embedded canopy hide disabled: layout mismatch");
        return false;
    }

    static constexpr std::uint32_t kSetEditableMaterialsPlt[4] = {
        0x90000210u, 0xf946a211u, 0x91350210u, 0xd61f0220u,
    };
    static constexpr std::uint32_t kResetEditableMaterialsPlt[4] = {
        0x90000210u, 0xf946a611u, 0x91352210u, 0xd61f0220u,
    };
    const bool resetHook = InstallRetailGotHook(
        "Hydra canopy reset", base,
        0x5358ecu, 0x808610u, 0x848d48u, kResetEditableMaterialsPlt,
        reinterpret_cast<void*>(g_origResetEditableMaterials),
        reinterpret_cast<void*>(&OnResetEditableMaterials));
    const bool setHook = resetHook && InstallRetailGotHook(
        "Hydra canopy set", base,
        0x535888u, 0x808600u, 0x848d40u, kSetEditableMaterialsPlt,
        reinterpret_cast<void*>(g_origSetEditableMaterials),
        reinterpret_cast<void*>(&OnSetEditableMaterials));
    const bool ready = resetHook && setHook;
    LOGI("[gfxfx.glass] embedded canopy hide ready=%d set=%d reset=%d",
         ready ? 1 : 0, setHook ? 1 : 0, resetHook ? 1 : 0);
    return ready;
}

bool InstallHeadlightDiagnostics(void* handle) {
    g_origStoreCarLightShadow = Resolve<StoreCarLightShadowFn>(
        handle,
        "_ZN8CShadows19StoreCarLightShadowEP8CVehicleiP9RwTextureP7CVectorffffhhhf");
    g_staticShadows = static_cast<std::uint8_t*>(
        dlsym(handle, "_ZN8CShadows14aStaticShadowsE"));
    g_shadowHeadLightsTex = static_cast<void**>(
        dlsym(handle, "gpShadowHeadLightsTex"));
    g_shadowHeadLightsTex2 = static_cast<void**>(
        dlsym(handle, "gpShadowHeadLightsTex2"));
    g_numPointLights = static_cast<int*>(
        dlsym(handle, "_ZN12CPointLights9NumLightsE"));

    Dl_info info{};
    if (!g_origStoreCarLightShadow || !g_staticShadows ||
        !g_shadowHeadLightsTex || !g_shadowHeadLightsTex2 ||
        !g_numPointLights ||
        dladdr(reinterpret_cast<void*>(g_origStoreCarLightShadow), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.light] diagnostics disabled: missing retail symbols");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool exactLayout =
        reinterpret_cast<std::uintptr_t>(g_staticShadows) - base == 0xc9b928u &&
        reinterpret_cast<std::uintptr_t>(g_shadowHeadLightsTex) - base == 0xc90f70u &&
        reinterpret_cast<std::uintptr_t>(g_shadowHeadLightsTex2) - base == 0xc90f78u &&
        reinterpret_cast<std::uintptr_t>(g_numPointLights) - base == 0xc8f998u;
    if (!exactLayout) {
        LOGW("[gfxfx.light] diagnostics disabled: retail layout mismatch");
        return false;
    }

    static constexpr std::uint32_t kStoreCarLightShadowPlt[4] = {
        0xd00001f0u, 0xf9415a11u, 0x910ac210u, 0xd61f0220u,
    };
    const bool hooked = InstallRetailGotHook(
        "car light producer", base,
        0x5e77dcu, 0x80d0e0u, 0x84b2b0u, kStoreCarLightShadowPlt,
        reinterpret_cast<void*>(g_origStoreCarLightShadow),
        reinterpret_cast<void*>(&OnStoreCarLightShadow));
    g_headlightDiagnosticsReady = true;
    LOGI("[gfxfx.light] diagnostics_ready=1 producer_hook=%d",
         hooked ? 1 : 0);
    return hooked;
}

void ResolveWeatherState(void* handle) {
    auto* const wetRoads = Resolve<float*>(
        handle, "_ZN8CWeather8WetRoadsE");
    auto* const underWaterness = Resolve<float*>(
        handle, "_ZN8CWeather14UnderWaternessE");
    auto* const waterDepth = Resolve<float*>(
        handle, "_ZN8CWeather10WaterDepthE");

    // Base address probe: prefer the resolved coronas entry point, but fall
    // back to the weather global itself - the underwater state must resolve
    // even at effects profile 0, where ResolveFunctions never runs.
    Dl_info info{};
    const void* baseProbe = g_fn.coronas
        ? reinterpret_cast<const void*>(g_fn.coronas)
        : static_cast<const void*>(underWaterness);
    if (!baseProbe || dladdr(baseProbe, &info) == 0 || !info.dli_fbase) {
        LOGW("[gfxfx.weather] disabled: missing retail owner");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool wetMatch = wetRoads &&
        reinterpret_cast<std::uintptr_t>(wetRoads) - base == 0xcc7460u;
    const bool underwaterMatch = underWaterness && waterDepth &&
        reinterpret_cast<std::uintptr_t>(underWaterness) - base == 0xcc7474u &&
        reinterpret_cast<std::uintptr_t>(waterDepth) - base == 0xcc7478u;
    if (wetMatch) g_weatherWetRoads = wetRoads;
    if (underwaterMatch) {
        g_weatherUnderWaterness = underWaterness;
        g_weatherWaterDepth = waterDepth;
        g_underwaterStateReady = true;
    }
    LOGI("[gfxfx.weather] wet=%d underwater=%d wet_ptr=%p under_ptr=%p depth_ptr=%p",
         wetMatch ? 1 : 0, underwaterMatch ? 1 : 0,
         wetRoads, underWaterness, waterDepth);
}

void PublishUnderwaterState(int eye) {
    if (eye != 0) return;
    // Resolve the weather globals on demand: at effects profile 0 the full
    // ResolveFunctions pass never runs, but the underwater grade is an XR
    // compositor pass and must keep working with the effects off.
    if (!g_underwaterStateReady) {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;
        if (void* handle = dlopen("libGame.so", RTLD_NOLOAD | RTLD_NOW)) {
            ResolveWeatherState(handle);
        }
    }
    if (!g_underwaterStateReady ||
        !g_weatherUnderWaterness || !g_weatherWaterDepth) {
        return;
    }
    const float underWaterness = std::isfinite(*g_weatherUnderWaterness)
        ? std::clamp(*g_weatherUnderWaterness, 0.0f, 1.0f)
        : 0.0f;
    const float waterDepth = std::isfinite(*g_weatherWaterDepth)
        ? std::clamp(*g_weatherWaterDepth, 0.0f, 100.0f)
        : 0.0f;
    xr::SetUnderwaterState(underWaterness, waterDepth);
}

bool InstallLampGroundPools(void* handle) {
    if (!g_coronaBillboardReady || !g_coronaArray) {
        LOGW("[gfxfx.lamp] disabled: corona state unavailable");
        return false;
    }

    g_origProcessLights = Resolve<ProcessLightsFn>(
        handle, "_ZN7CEntity22ProcessLightsForEntityEv");
    g_get2dEffect = Resolve<Get2dEffectFn>(
        handle, "_ZN14CBaseModelInfo11Get2dEffectEi");
    g_storeStaticShadow = Resolve<StoreStaticShadowFn>(
        handle,
        "_ZN8CShadows17StoreStaticShadowEyhP9RwTextureP7CVectorffffshhhfffbf");

    Dl_info info{};
    if (!g_origProcessLights || !g_get2dEffect || !g_storeStaticShadow ||
        dladdr(reinterpret_cast<void*>(g_origProcessLights), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.lamp] disabled: missing retail symbols");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    auto** const modelInfoPtrs = reinterpret_cast<void**>(base + 0xbd6d68u);
    const bool offsetsMatch =
        reinterpret_cast<std::uintptr_t>(g_origProcessLights) - base ==
            0x5cff1cu &&
        reinterpret_cast<std::uintptr_t>(g_get2dEffect) - base ==
            0x530704u &&
        reinterpret_cast<std::uintptr_t>(g_storeStaticShadow) - base ==
            0x5e68f0u &&
        reinterpret_cast<std::uintptr_t>(g_coronaArray) - base ==
            0xc6c7f0u;
    static constexpr std::uint32_t kProcessEntry[4] = {
        0xd10483ffu, 0x6d083befu, 0x6d0933edu, 0x6d0a2bebu,
    };
    static constexpr std::uint32_t kGetEffectEntry[4] = {
        0xa9bd7bfdu, 0xf9000bf5u, 0xa9024ff4u, 0x910003fdu,
    };
    static constexpr std::uint32_t kStoreShadowEntry[4] = {
        0xa9be7bfdu, 0xf9000bf3u, 0x910003fdu, 0xb0001288u,
    };
    std::uint32_t processEntry[4]{};
    std::uint32_t getEffectEntry[4]{};
    std::uint32_t storeShadowEntry[4]{};
    std::memcpy(processEntry, reinterpret_cast<void*>(g_origProcessLights),
                sizeof(processEntry));
    std::memcpy(getEffectEntry, reinterpret_cast<void*>(g_get2dEffect),
                sizeof(getEffectEntry));
    std::memcpy(storeShadowEntry, reinterpret_cast<void*>(g_storeStaticShadow),
                sizeof(storeShadowEntry));
    const bool entriesMatch =
        std::memcmp(processEntry, kProcessEntry, sizeof(processEntry)) == 0 &&
        std::memcmp(getEffectEntry, kGetEffectEntry,
                    sizeof(getEffectEntry)) == 0 &&
        std::memcmp(storeShadowEntry, kStoreShadowEntry,
                    sizeof(storeShadowEntry)) == 0;
    if (!offsetsMatch || !entriesMatch) {
        LOGW("[gfxfx.lamp] disabled: retail layout mismatch offsets=%d entries=%d",
             offsetsMatch ? 1 : 0, entriesMatch ? 1 : 0);
        return false;
    }

    static constexpr std::uint32_t kProcessLightsPlt[4] = {
        0xf0000210u, 0xf945e211u, 0x912f0210u, 0xd61f0220u,
    };
    g_modelInfoPtrs = modelInfoPtrs;
    const bool hooked = InstallRetailGotHook(
        "streetlamp ground pool", base,
        0x5cff1cu, 0x802300u, 0x845bc0u, kProcessLightsPlt,
        reinterpret_cast<void*>(g_origProcessLights),
        reinterpret_cast<void*>(&OnProcessLightsForEntity));
    if (!hooked) g_modelInfoPtrs = nullptr;
    LOGI("[gfxfx.lamp] ready=%d model_info=%p get_effect=%p store=%p",
         hooked ? 1 : 0, modelInfoPtrs,
         reinterpret_cast<void*>(g_get2dEffect),
         reinterpret_cast<void*>(g_storeStaticShadow));
    return hooked;
}

bool InstallWetRoadReflections(void* handle) {
    if (!g_coronaBillboardReady || !g_weatherWetRoads ||
        !g_eyeTargetRoutingReady) {
        LOGW("[gfxfx.wet] disabled: stereo corona/weather state unavailable");
        return false;
    }
    g_origRenderReflections = Resolve<RenderReflectionsFn>(
        handle, "_ZN8CCoronas17RenderReflectionsEv");
    auto* const scene = static_cast<std::uint8_t*>(dlsym(handle, "Scene"));
    if (!g_scene) g_scene = scene;

    Dl_info info{};
    if (!g_origRenderReflections || !scene || g_scene != scene ||
        dladdr(reinterpret_cast<void*>(g_origRenderReflections), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.wet] disabled: missing retail symbols");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool offsetsMatch =
        reinterpret_cast<std::uintptr_t>(g_origRenderReflections) - base ==
            0x5ce7c0u &&
        reinterpret_cast<std::uintptr_t>(g_weatherWetRoads) - base ==
            0xcc7460u &&
        reinterpret_cast<std::uintptr_t>(g_scene) - base == 0xcd0868u;
    static constexpr std::uint32_t kReflectionEntry[4] = {
        0xd104c3ffu, 0x6d093befu, 0x6d0a33edu, 0x6d0b2bebu,
    };
    std::uint32_t observedEntry[4]{};
    std::memcpy(observedEntry,
                reinterpret_cast<void*>(g_origRenderReflections),
                sizeof(observedEntry));
    const bool entryMatch =
        std::memcmp(observedEntry, kReflectionEntry,
                    sizeof(observedEntry)) == 0;
    if (!offsetsMatch || !entryMatch) {
        LOGW("[gfxfx.wet] disabled: retail layout mismatch offsets=%d entry=%d",
             offsetsMatch ? 1 : 0, entryMatch ? 1 : 0);
        return false;
    }
    static constexpr std::uint32_t kReflectionPlt[4] = {
        0xf0000210u, 0xf9428211u, 0x91140210u, 0xd61f0220u,
    };
    const bool hooked = InstallRetailGotHook(
        "wet-road stereo reflections", base,
        0x5ce7c0u, 0x803580u, 0x846500u, kReflectionPlt,
        reinterpret_cast<void*>(g_origRenderReflections),
        reinterpret_cast<void*>(&OnRenderReflections));
    LOGI("[gfxfx.wet] ready=%d function=%p wet=%p",
         hooked ? 1 : 0,
         reinterpret_cast<void*>(g_origRenderReflections), g_weatherWetRoads);
    return hooked;
}

bool InstallTrafficLightQualityGate(void* handle) {
    g_origDisplayActualLight = Resolve<DisplayActualLightFn>(
        handle,
        "_ZN14CTrafficLights18DisplayActualLightEP7CEntity");
    auto* const settings = static_cast<std::uint8_t*>(
        dlsym(handle, "_ZN14MobileSettings8settingsE"));

    Dl_info info{};
    if (!g_origDisplayActualLight || !settings ||
        dladdr(reinterpret_cast<void*>(g_origDisplayActualLight), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.traffic] quality gate disabled: missing retail symbols");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool offsetsMatch =
        reinterpret_cast<std::uintptr_t>(g_origDisplayActualLight) - base ==
            0x451910u &&
        reinterpret_cast<std::uintptr_t>(settings) - base == 0x8a7688u;

    static constexpr std::uint32_t kDisplayEntry[4] = {
        0xd105c3ffu, 0x6d0d3befu, 0x6d0e33edu, 0x6d0f2bebu,
    };
    static constexpr std::uint32_t kQualityGate[3] = {
        0xb9401368u, 0x7100091fu, 0x54fffecbu,
    };
    std::uint32_t observedEntry[4]{};
    std::uint32_t observedGate[3]{};
    std::memcpy(observedEntry,
                reinterpret_cast<const void*>(g_origDisplayActualLight),
                sizeof(observedEntry));
    std::memcpy(observedGate,
                reinterpret_cast<const void*>(base + 0x451b70u),
                sizeof(observedGate));
    const bool layoutMatch = offsetsMatch &&
        std::memcmp(observedEntry, kDisplayEntry, sizeof(observedEntry)) == 0 &&
        std::memcmp(observedGate, kQualityGate, sizeof(observedGate)) == 0 &&
        *reinterpret_cast<const std::uintptr_t*>(base + 0x8373d0u) ==
            reinterpret_cast<std::uintptr_t>(settings);
    if (!layoutMatch) {
        LOGW("[gfxfx.traffic] quality gate disabled: retail layout mismatch");
        return false;
    }

    g_mobileEffectsQuality = reinterpret_cast<std::int32_t*>(
        settings + 0x10);
    static constexpr std::uint32_t kDisplayPlt[4] = {
        0x90000230u, 0xf941da11u, 0x910ec210u, 0xd61f0220u,
    };
    const bool hooked = InstallRetailGotHook(
        "traffic light quality", base,
        0x451910u, 0x8012e0u, 0x8453b0u, kDisplayPlt,
        reinterpret_cast<void*>(g_origDisplayActualLight),
        reinterpret_cast<void*>(&OnDisplayActualLight));
    if (!hooked) g_mobileEffectsQuality = nullptr;
    LOGI("[gfxfx.traffic] quality_gate_ready=%d settings=%p quality=%p",
         hooked ? 1 : 0, settings, g_mobileEffectsQuality);
    return hooked;
}

void* OnCreateFxSystem(void* manager, char* name, void* position,
                       void* parentMatrix, std::uint8_t ignoreBoundingChecks) {
    if (!g_origCreateFxSystem) return nullptr;

    // The ground fire producer is independent from CWeapon::m_FxSystem.  A flat
    // retail frustum can therefore reject the muzzle blueprint while creeping
    // fire still appears at the correctly routed impact point.  Preserve the
    // stock decision first; only retry the exact local-player flamethrower
    // blueprint when that decision returned null.  This never admits unrelated
    // systems and adds no particles when retail creation already succeeded.
    const bool localFlamethrower =
        g_insideLocalFlameProducer && name &&
        std::strcmp(name, "flamethrower") == 0;
    if (!localFlamethrower) {
        return g_origCreateFxSystem(
            manager, name, position, parentMatrix, ignoreBoundingChecks);
    }

    const std::uint64_t attempt =
        g_flameCreateAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
    void* system = g_origCreateFxSystem(
        manager, name, position, parentMatrix, ignoreBoundingChecks);
    bool retried = false;
    if (system) {
        g_flameCreateStockSuccess.fetch_add(1, std::memory_order_relaxed);
    } else if (!ignoreBoundingChecks) {
        retried = true;
        g_flameCreateRetries.fetch_add(1, std::memory_order_relaxed);
        system = g_origCreateFxSystem(
            manager, name, position, parentMatrix, 1u);
        if (system) {
            g_flameCreateRetrySuccess.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!system) {
        g_flameCreateFailures.fetch_add(1, std::memory_order_relaxed);
    }
    if (attempt <= 4 || attempt % 600 == 0 || retried) {
        LOGI("[gfxfx.flame] create=%llu stock=%d retry=%d result=%p",
             static_cast<unsigned long long>(attempt),
             (!retried && system) ? 1 : 0, retried ? 1 : 0, system);
    }
    return system;
}

bool CallLocalFireAreaEffect(void* weapon, void* firingEntity,
                             GameSymbols::Vec3* origin, void* targetEntity,
                             GameSymbols::Vec3* target) {
    void* const before = *reinterpret_cast<void**>(
        static_cast<std::uint8_t*>(weapon) + 0x18);
    const bool previousProducer = g_insideLocalFlameProducer;
    g_insideLocalFlameProducer = true;
    const bool result = g_origFireAreaEffect(
        weapon, firingEntity, origin, targetEntity, target);
    g_insideLocalFlameProducer = previousProducer;
    void* const after = *reinterpret_cast<void**>(
        static_cast<std::uint8_t*>(weapon) + 0x18);

    if (after) {
        g_flameSystemPresentCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (!before && after) {
        const std::uint64_t created =
            g_flameSystemNewCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        LOGI("[gfxfx.flame] weapon_system_created=%llu ptr=%p",
             static_cast<unsigned long long>(created), after);
    } else if (!after) {
        const std::uint64_t attempts =
            g_flameCreateAttempts.load(std::memory_order_relaxed);
        if (attempts <= 4 || attempts % 600 == 0) {
            LOGW("[gfxfx.flame] weapon_system_missing before=%p after=%p",
                 before, after);
        }
    }
    return result;
}

void OnWeaponUpdate(void* weapon, void* ped) {
    if (!g_origWeaponUpdate) return;

    // The Quest controller path can drive FireAreaEffect again immediately
    // after the weapon's READY-state Update. Retail Update treats that short
    // READY gap as the end of a continuous area effect and kills m_FxSystem at
    // weapon+0x18. Runtime evidence showed the local flamethrower consequently
    // allocating a fresh system almost every shot, never retaining one long
    // enough to build its visible stream. Hide only that pointer from the
    // destructive READY update while recent local flame input is confirmed;
    // ammo/state/audio still run through the untouched retail function. Once
    // the trigger has been idle for the bounded grace period, normal teardown
    // is allowed again.
    auto* const bytes = static_cast<std::uint8_t*>(weapon);
    const bool localFlamethrower =
        Profile() == 1 && savr::vrcam::IsStereoActive() && bytes && ped &&
        g.FindPlayerPed &&
        *reinterpret_cast<const std::int32_t*>(bytes) == 37 &&
        ped == g.FindPlayerPed(-1);
    void* const before = localFlamethrower
        ? *reinterpret_cast<void**>(bytes + 0x18)
        : nullptr;
    const std::uint32_t state = localFlamethrower
        ? *reinterpret_cast<const std::uint32_t*>(bytes + 0x04)
        : 0u;
    const std::uint64_t nowMs = static_cast<std::uint64_t>(MonotonicMs());
    const std::uint64_t lastMs =
        g_flameLastRoutedMs.load(std::memory_order_relaxed);
    const bool recent = lastMs != 0 && nowMs >= lastMs &&
        nowMs - lastMs <= kFlameLifecycleGraceMs;

    if (before && state == 0u && recent) {
        auto& slot = *reinterpret_cast<void**>(bytes + 0x18);
        slot = nullptr;
        g_origWeaponUpdate(weapon, ped);
        if (!slot &&
            *reinterpret_cast<const std::int32_t*>(bytes) == 37 &&
            ped == g.FindPlayerPed(-1)) {
            slot = before;
            const std::uint64_t preserved =
                g_flameLifecyclePreserves.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            if (preserved <= 4 || preserved % 600 == 0) {
                LOGI("[gfxfx.flame] lifecycle_preserve=%llu state=%u age_ms=%llu ptr=%p",
                     static_cast<unsigned long long>(preserved), state,
                     static_cast<unsigned long long>(nowMs - lastMs), before);
            }
        }
        return;
    }

    g_origWeaponUpdate(weapon, ped);
    if (before && !*reinterpret_cast<void**>(bytes + 0x18)) {
        const std::uint64_t killed =
            g_flameLifecycleKills.fetch_add(1, std::memory_order_relaxed) + 1;
        if (killed <= 4 || killed % 120 == 0) {
            LOGI("[gfxfx.flame] lifecycle_stock_kill=%llu state=%u recent=%d",
                 static_cast<unsigned long long>(killed), state,
                 recent ? 1 : 0);
        }
    }
}

struct FlameV3 {
    float x{}, y{}, z{};
};

FlameV3 operator+(FlameV3 a, FlameV3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
FlameV3 operator-(FlameV3 a, FlameV3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
FlameV3 operator*(FlameV3 v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

float FlameDot(FlameV3 a, FlameV3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool FlameFinite(FlameV3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) &&
           std::isfinite(v.z);
}

FlameV3 FlameNormalized(FlameV3 v) {
    const float lengthSq = FlameDot(v, v);
    if (!std::isfinite(lengthSq) || lengthSq < 1.0e-8f) return {};
    return v * (1.0f / std::sqrt(lengthSq));
}

FlameV3 FlameRotateQuaternion(const float q[4], FlameV3 v) {
    const FlameV3 vectorPart{q[0], q[1], q[2]};
    const float scalarPart = q[3];
    const FlameV3 cross{
        vectorPart.y * v.z - vectorPart.z * v.y,
        vectorPart.z * v.x - vectorPart.x * v.z,
        vectorPart.x * v.y - vectorPart.y * v.x};
    return vectorPart * (2.0f * FlameDot(vectorPart, v)) +
        v * (scalarPart * scalarPart - FlameDot(vectorPart, vectorPart)) +
        cross * (2.0f * scalarPart);
}

FlameV3 FlameRotateAroundAxis(FlameV3 value, FlameV3 axis, float angle) {
    axis = FlameNormalized(axis);
    if (FlameDot(axis, axis) < 0.9f || !std::isfinite(angle)) return value;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const FlameV3 cross{
        axis.y * value.z - axis.z * value.y,
        axis.z * value.x - axis.x * value.z,
        axis.x * value.y - axis.y * value.x};
    return value * cosine + cross * sine +
        axis * (FlameDot(axis, value) * (1.0f - cosine));
}

// flame.dff is the important exception to the generic SA weapon convention.
// Its visible nozzle is neither at model origin + 0.18*X nor parallel to +X:
// the retail mesh ends around (1.10,-0.027,0.44), and its tube axis is
// normalize(0.40,-0.007,0.16), 21.8 degrees above +X. Evaluate the socket in
// the exact FINAL rendered model pose, but preserve the mature Vice City split:
// WPN fields position only the visible model/muzzle while raw controller AIM +
// AIM fields own the shot direction. This is why changing WPN no longer drags
// the firing zone and changing AIM finally has an independent visible result.
bool BuildFlamethrowerMuzzleRay(int hand,
                                float originOut[3],
                                float directionOut[3],
                                bool* usedVisualMuzzleOut) {
    if (!originOut || !directionOut || hand < 0 || hand > 1) return false;
    if (usedVisualMuzzleOut) *usedVisualMuzzleOut = false;

    const int slot = physicalweapon::HeldSlot(hand);
    physicalweapon::TrackingPose pose{};
    const bool haveVisualPose = slot > 0 &&
        physicalweapon::GetHeldVisualPoseTracking(hand, slot, &pose);

    xr::HandPose hands[physicalweapon::kHandCount]{};
    if (!physicalweapon::GetHandPosesSnapshot(hands))
        xr::GetHandPoses(hands);
    const xr::HandPose& tracked = hands[hand];
    if (!tracked.valid || !tracked.aimValid ||
        !std::isfinite(tracked.aimPos[0]) ||
        !std::isfinite(tracked.aimPos[1]) ||
        !std::isfinite(tracked.aimPos[2])) {
        return false;
    }

    FlameV3 right = FlameNormalized(FlameRotateQuaternion(
        tracked.aimOri, {1.0f, 0.0f, 0.0f}));
    FlameV3 up = FlameNormalized(FlameRotateQuaternion(
        tracked.aimOri, {0.0f, 1.0f, 0.0f}));
    FlameV3 forward = FlameNormalized(FlameRotateQuaternion(
        tracked.aimOri, {0.0f, 0.0f, -1.0f}));
    up = FlameNormalized(up - forward * FlameDot(up, forward));
    right = FlameNormalized(right - forward * FlameDot(right, forward) -
                            up * FlameDot(right, up));
    if (FlameDot(right, right) < 0.9f || FlameDot(up, up) < 0.9f ||
        FlameDot(forward, forward) < 0.9f) return false;

    const calib::WeaponCalib calibration = calib::Snapshot(1, 37);
    constexpr float kHalfDegreeRadians =
        0.5f * 3.14159265358979323846f / 180.0f;
    const float pitch = calibration.aimRotX * kHalfDegreeRadians;
    const float yaw = calibration.aimRotY * kHalfDegreeRadians;
    const float roll = calibration.aimRotZ * kHalfDegreeRadians;
    if (!std::isfinite(pitch) || !std::isfinite(yaw) ||
        !std::isfinite(roll)) return false;
    if (pitch != 0.0f) {
        forward = FlameRotateAroundAxis(forward, right, pitch);
        up = FlameRotateAroundAxis(up, right, pitch);
    }
    if (yaw != 0.0f) {
        forward = FlameRotateAroundAxis(forward, up, yaw);
        right = FlameRotateAroundAxis(right, up, yaw);
    }
    if (roll != 0.0f) {
        right = FlameRotateAroundAxis(right, forward, roll);
        up = FlameRotateAroundAxis(up, forward, roll);
    }

    constexpr float kCalibrationUnitMetres = 0.005f;
    // Startup can fire before the first render has published a model pose. Use
    // the VC flamethrower's proven long-muzzle source for that bounded window,
    // never the old model +X ray that created the visible downward burst.
    constexpr float kViceCityFlameMuzzleForward = 0.68f;
    FlameV3 fallbackSource{
        tracked.aimPos[0], tracked.aimPos[1], tracked.aimPos[2]};
    fallbackSource = fallbackSource +
        right * (calibration.aimOffX * kCalibrationUnitMetres) +
        up * (calibration.aimOffY * kCalibrationUnitMetres) +
        forward * (kViceCityFlameMuzzleForward +
                   calibration.aimOffZ * kCalibrationUnitMetres);

    // Vice City rotates its raw AIM ray by the same support-hand transform as
    // the weapon. The final muzzle pose above already contains this rotation;
    // apply it only to the independently built AIM basis, never to the socket.
    float twoHandPivot[3]{}, twoHandAxisValues[3]{};
    float twoHandAngle = 0.0f;
    if (physicalweapon::GetTwoHandTransformTracking(
            hand, twoHandPivot, twoHandAxisValues, &twoHandAngle)) {
        const FlameV3 twoHandAxis{
            twoHandAxisValues[0], twoHandAxisValues[1], twoHandAxisValues[2]};
        const FlameV3 twoHandPivotVector{
            twoHandPivot[0], twoHandPivot[1], twoHandPivot[2]};
        fallbackSource = twoHandPivotVector + FlameRotateAroundAxis(
            fallbackSource - twoHandPivotVector,
            twoHandAxis, twoHandAngle);
        right = FlameRotateAroundAxis(right, twoHandAxis, twoHandAngle);
        up = FlameRotateAroundAxis(up, twoHandAxis, twoHandAngle);
        forward = FlameRotateAroundAxis(forward, twoHandAxis, twoHandAngle);
    }
    right = FlameNormalized(right);
    up = FlameNormalized(up);
    forward = FlameNormalized(forward);

    FlameV3 source = fallbackSource;
    if (haveVisualPose) {
        const FlameV3 position{
            pose.position[0], pose.position[1], pose.position[2]};
        const FlameV3 modelX = FlameNormalized(
            {pose.right[0], pose.right[1], pose.right[2]});
        const FlameV3 modelY = FlameNormalized(
            {pose.forward[0], pose.forward[1], pose.forward[2]});
        const FlameV3 modelZ = FlameNormalized(
            {pose.up[0], pose.up[1], pose.up[2]});
        if (FlameFinite(position) && FlameDot(modelX, modelX) >= 0.9f &&
            FlameDot(modelY, modelY) >= 0.9f &&
            FlameDot(modelZ, modelZ) >= 0.9f) {
            constexpr FlameV3 kMuzzleSocket{1.10f, -0.027f, 0.44f};
            const FlameV3 muzzle = position +
                modelX * kMuzzleSocket.x +
                modelY * kMuzzleSocket.y +
                modelZ * kMuzzleSocket.z;
            source = muzzle +
                right * (calibration.aimOffX * kCalibrationUnitMetres) +
                up * (calibration.aimOffY * kCalibrationUnitMetres) +
                forward * (calibration.aimOffZ * kCalibrationUnitMetres);
            if (usedVisualMuzzleOut) *usedVisualMuzzleOut = true;
        }
    }
    if (!FlameFinite(source) || !FlameFinite(forward) ||
        FlameDot(forward, forward) < 0.9f) {
        return false;
    }

    const FlameV3 trackingEnd = source + forward;
    const float trackingSource[3]{source.x, source.y, source.z};
    const float trackingTarget[3]{
        trackingEnd.x, trackingEnd.y, trackingEnd.z};
    float worldSource[3]{}, worldTarget[3]{};
    if (!vrcam::TrackingPointToWorld(trackingSource, worldSource) ||
        !vrcam::TrackingPointToWorld(trackingTarget, worldTarget)) {
        return false;
    }
    const FlameV3 worldDirection = FlameNormalized(
        {worldTarget[0] - worldSource[0],
         worldTarget[1] - worldSource[1],
         worldTarget[2] - worldSource[2]});
    if (!FlameFinite(worldDirection) ||
        FlameDot(worldDirection, worldDirection) < 0.9f) {
        return false;
    }

    std::memcpy(originOut, worldSource, sizeof(worldSource));
    directionOut[0] = worldDirection.x;
    directionOut[1] = worldDirection.y;
    directionOut[2] = worldDirection.z;
    return true;
}

bool OnFireAreaEffect(void* weapon, void* firingEntity,
                      GameSymbols::Vec3* origin, void* targetEntity,
                      GameSymbols::Vec3* target) {
    if (!g_origFireAreaEffect) return false;

    // The Quest physical-fire route currently owns hitscan weapons only.  The
    // stock area-weapon producer therefore still derives flamethrower direction
    // from the legacy player camera/heading and a ped-relative barrel point.
    // Once generic Fx rendering became visible this exposed flame and creeping
    // ground fire behind the player.  Route only the local, physically held
    // flamethrower through the already-calibrated weapon ray; NPCs and every
    // other area weapon retain the retail producer unchanged.
    constexpr std::int32_t kFlamethrowerType = 37;
    const bool localFlamethrower =
        Profile() == 1 && savr::vrcam::IsStereoActive() && weapon &&
        firingEntity && origin && g.FindPlayerPed &&
        *static_cast<const std::int32_t*>(weapon) == kFlamethrowerType &&
        firingEntity == g.FindPlayerPed(-1);
    if (!localFlamethrower) {
        return g_origFireAreaEffect(weapon, firingEntity, origin,
                                    targetEntity, target);
    }

    g_flameLastRoutedMs.store(
        static_cast<std::uint64_t>(MonotonicMs()),
        std::memory_order_relaxed);

    const int hand = physicalweapon::FiringHand();
    float rayOrigin[3]{};
    float rayDirection[3]{};
    bool usedVisualMuzzle = false;
    const bool physicalFlameRay = hand >= 0 && hand <= 1 &&
        BuildFlamethrowerMuzzleRay(
            hand, rayOrigin, rayDirection, &usedVisualMuzzle);
    const bool genericFallbackRay = !physicalFlameRay &&
        hand >= 0 && hand <= 1 &&
        vrcam::GetWeaponFireRay(hand, kFlamethrowerType,
                               rayOrigin, rayDirection);
    if (!physicalFlameRay && !genericFallbackRay) {
        const std::uint64_t fallback =
            g_flameFallbackCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallback <= 4 || fallback % 600 == 0) {
            LOGW("[gfxfx.flame] fallback=%llu hand=%d reason=no_physical_ray",
                 static_cast<unsigned long long>(fallback), hand);
        }
        return CallLocalFireAreaEffect(weapon, firingEntity, origin,
                                       targetEntity, target);
    }

    const float directionLengthSq =
        rayDirection[0] * rayDirection[0] +
        rayDirection[1] * rayDirection[1] +
        rayDirection[2] * rayDirection[2];
    if (!std::isfinite(rayOrigin[0]) || !std::isfinite(rayOrigin[1]) ||
        !std::isfinite(rayOrigin[2]) || !std::isfinite(directionLengthSq) ||
        directionLengthSq < 0.5f || directionLengthSq > 1.5f) {
        const std::uint64_t fallback =
            g_flameFallbackCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallback <= 4 || fallback % 600 == 0) {
            LOGW("[gfxfx.flame] fallback=%llu hand=%d reason=invalid_ray len2=%.3f",
                 static_cast<unsigned long long>(fallback), hand,
                 directionLengthSq);
        }
        return CallLocalFireAreaEffect(weapon, firingEntity, origin,
                                       targetEntity, target);
    }

    // Retail's no-target FireAreaEffect uses shotPt = origin + direction.
    // Preserve that one-metre segment so CShotInfo keeps the stock type-37
    // spread; DoWeaponEffect and creeping fire normalize the same direction.
    constexpr float kAreaRayLengthM = 1.0f;
    const float invLength = 1.0f / std::sqrt(directionLengthSq);
    GameSymbols::Vec3 routedOrigin{
        rayOrigin[0], rayOrigin[1], rayOrigin[2]};
    GameSymbols::Vec3 routedTarget{
        routedOrigin.x + rayDirection[0] * invLength * kAreaRayLengthM,
        routedOrigin.y + rayDirection[1] * invLength * kAreaRayLengthM,
        routedOrigin.z + rayDirection[2] * invLength * kAreaRayLengthM};

    const std::uint64_t call =
        g_flameRoutedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 4 || call % 600 == 0) {
        const calib::WeaponCalib calibration =
            calib::Snapshot(1, kFlamethrowerType);
        LOGI("[gfxfx.flame] routed=%llu hand=%d muzzle=%d "
             "aim_off=(%d,%d,%d) aim_rot=(%d,%d,%d) "
             "origin=(%.2f,%.2f,%.2f) "
             "target=(%.2f,%.2f,%.2f)",
             static_cast<unsigned long long>(call), hand,
             usedVisualMuzzle ? 1 : 0,
             static_cast<int>(calibration.aimOffX),
             static_cast<int>(calibration.aimOffY),
             static_cast<int>(calibration.aimOffZ),
             static_cast<int>(calibration.aimRotX),
             static_cast<int>(calibration.aimRotY),
             static_cast<int>(calibration.aimRotZ),
             routedOrigin.x, routedOrigin.y, routedOrigin.z,
             routedTarget.x, routedTarget.y, routedTarget.z);
    }
    return CallLocalFireAreaEffect(
        weapon, firingEntity, &routedOrigin, nullptr, &routedTarget);
}

bool InstallAreaWeaponRouting(void* handle) {
    g_origFireAreaEffect = Resolve<FireAreaEffectFn>(
        handle, "_ZN7CWeapon14FireAreaEffectEP7CEntityP7CVectorS1_S3_");
    g_origCreateFxSystem = Resolve<CreateFxSystemFn>(
        handle,
        "_ZN11FxManager_c14CreateFxSystemEPcP5RwV3dP11RwMatrixTagh");
    g_origWeaponUpdate = Resolve<WeaponUpdateFn>(
        handle, "_ZN7CWeapon6UpdateEP4CPed");
    Dl_info info{};
    if (!g_origFireAreaEffect || !g_origCreateFxSystem ||
        !g_origWeaponUpdate ||
        dladdr(reinterpret_cast<void*>(g_origFireAreaEffect), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.flame] physical ray disabled: missing retail symbol");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    static constexpr std::uint32_t kCreateFxSystemPlt[4] = {
        0xb0000230u, 0xf944fa11u, 0x9127c210u, 0xd61f0220u,
    };
    static constexpr std::uint32_t kFireAreaEffectPlt[4] = {
        0xb00001f0u, 0xf940a211u, 0x91050210u, 0xd61f0220u,
    };
    static constexpr std::uint32_t kWeaponUpdatePlt[4] = {
        0x90000230u, 0xf9432a11u, 0x91194210u, 0xd61f0220u,
    };
    g_flameCreateRetryReady = InstallRetailGotHook(
        "local flamethrower create retry", base,
        0x4dfed4u, 0x7fff60u, 0x8449f0u, kCreateFxSystemPlt,
        reinterpret_cast<void*>(g_origCreateFxSystem),
        reinterpret_cast<void*>(&OnCreateFxSystem));
    const bool hooked = InstallRetailGotHook(
        "flamethrower physical ray", base,
        0x6fcc48u, 0x810e00u, 0x84d140u, kFireAreaEffectPlt,
        reinterpret_cast<void*>(g_origFireAreaEffect),
        reinterpret_cast<void*>(&OnFireAreaEffect));
    g_flameLifecycleReady = InstallRetailGotHook(
        "local flamethrower lifecycle", base,
        0x700d9cu, 0x801820u, 0x845650u, kWeaponUpdatePlt,
        reinterpret_cast<void*>(g_origWeaponUpdate),
        reinterpret_cast<void*>(&OnWeaponUpdate));
    LOGI("[gfxfx.flame] physical_ray_ready=%d create_retry_ready=%d lifecycle_ready=%d",
         hooked ? 1 : 0, g_flameCreateRetryReady ? 1 : 0,
         g_flameLifecycleReady ? 1 : 0);
    return hooked && g_flameLifecycleReady;
}

bool InstallShadowTailRouting(void* handle) {
    g_origResolveShadowTarget = Resolve<ResolveShadowTargetFn>(
        handle, "_Z23emu_ResolveShadowTargetf");
    g_scene = static_cast<std::uint8_t*>(dlsym(handle, "Scene"));

    Dl_info info{};
    if (!g_eyeTargetRoutingReady || !g_origResolveShadowTarget || !g_scene ||
        dladdr(reinterpret_cast<void*>(g_origResolveShadowTarget), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.blood] shadow tail disabled: missing retail symbols");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    if (reinterpret_cast<std::uintptr_t>(g_scene) - base != 0xcd0868u) {
        LOGW("[gfxfx.blood] shadow tail disabled: Scene offset mismatch");
        return false;
    }

    static constexpr std::uint32_t kResolveShadowTargetPlt[4] = {
        0xf0000210u, 0xf942ea11u, 0x91174210u, 0xd61f0220u,
    };
    const bool hooked = InstallRetailGotHook(
        "shadow tail target", base, 0x73a35cu, 0x803720u, 0x8465d0u,
        kResolveShadowTargetPlt,
        reinterpret_cast<void*>(g_origResolveShadowTarget),
        reinterpret_cast<void*>(&OnResolveShadowTarget));
    LOGI("[gfxfx.blood] shadow_tail_ready=%d scene=%p", hooked ? 1 : 0,
         g_scene);
    return hooked;
}

void* OnIm3DTransform(void* vertices, unsigned int count, void* matrix,
                      unsigned int flags) {
    if (!g_insideFxRender || !g_origIm3DTransform)
        return g_origIm3DTransform
            ? g_origIm3DTransform(vertices, count, matrix, flags)
            : nullptr;

    ++g_aggregate.im3dTransformCalls;
    if (g_fxEye == 0) ++g_aggregate.im3dTransformLeft;
    if (g_fxEye == 1) ++g_aggregate.im3dTransformRight;
    g_aggregate.im3dVertices += count;
    if (!g_aggregate.renderStateSampled) {
        g_aggregate.renderStateSampled =
            ReadRenderState(6, g_aggregate.zTest) &&
            ReadRenderState(8, g_aggregate.zWrite) &&
            ReadRenderState(12, g_aggregate.vertexAlpha) &&
            ReadRenderState(10, g_aggregate.srcBlend) &&
            ReadRenderState(11, g_aggregate.dstBlend) &&
            ReadRenderState(29, g_aggregate.alphaTest) &&
            ReadRenderState(30, g_aggregate.alphaRef);
    }
    void* textureRaster = nullptr;
    if (g_fn.renderStateGet && g_fn.renderStateGet(1, &textureRaster) &&
        textureRaster) {
        ++g_aggregate.im3dTextureRaster;
    } else {
        ++g_aggregate.im3dNullTextureRaster;
    }
    const int probeMode = FxProbeMode();
    if (probeMode > 0) {
        ApplyFxVisibilityProbe(probeMode);
        ++g_aggregate.probeApplications;
    }
    void* const result = g_origIm3DTransform(vertices, count, matrix, flags);
    if (result) ++g_aggregate.im3dTransformSuccess;
    return result;
}

int OnIm3DRenderPrimitive(int primitiveType) {
    const int result = g_origIm3DRenderPrimitive
        ? g_origIm3DRenderPrimitive(primitiveType)
        : 0;
    if (g_insideFxRender) {
        ++g_aggregate.im3dPrimitiveCalls;
        if (result) ++g_aggregate.im3dPrimitiveSuccess;
    }
    return result;
}

void OnFxManagerUpdate(void* manager, void* camera, float timeDelta) {
    g_fxCameraPositionValid = false;
    if (camera && g_rwFrameGetLtm) {
        void* const frame = *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(camera) + 0x08);
        auto* const matrix = frame
            ? static_cast<std::uint8_t*>(g_rwFrameGetLtm(frame))
            : nullptr;
        if (matrix) {
            const float* const position =
                reinterpret_cast<const float*>(matrix + 0x30);
            if (std::isfinite(position[0]) && std::isfinite(position[1]) &&
                std::isfinite(position[2])) {
                g_fxCameraPosition[0] = position[0];
                g_fxCameraPosition[1] = position[1];
                g_fxCameraPosition[2] = position[2];
                g_fxCameraPositionValid = true;
            }
        }
    }
    if (g_origFxManagerUpdate)
        g_origFxManagerUpdate(manager, camera, timeDelta);
}

int OnFxFrustumCollision(void* frustum, void* sphere) {
    if (!g_origFxFrustumCollision) return 0;
    if (Profile() != 1 || !g_fxCameraPositionValid || !sphere)
        return g_origFxFrustumCollision(frustum, sphere);

    const float* const values = static_cast<const float*>(sphere);
    const float radius = values[3];
    if (!std::isfinite(values[0]) || !std::isfinite(values[1]) ||
        !std::isfinite(values[2]) || !std::isfinite(radius)) {
        return g_origFxFrustumCollision(frustum, sphere);
    }
    // Car fire only needs a local safety sphere. The earlier 100 m diagnostic
    // range proved the culling fault but admitted thousands of unrelated FX;
    // 20 m keeps nearby chaos independent from the flat camera direction while
    // bounding both CPU emission and stereo fill-rate.
    constexpr float kVrNearbyFxRangeM = 20.0f;
    const float dx = values[0] - g_fxCameraPosition[0];
    const float dy = values[1] - g_fxCameraPosition[1];
    const float dz = values[2] - g_fxCameraPosition[2];
    const float paddedRange = kVrNearbyFxRangeM + std::max(0.0f, radius);
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSq) ||
        distanceSq > paddedRange * paddedRange) {
        return g_origFxFrustumCollision(frustum, sphere);
    }

    g_fxNearChecks.fetch_add(1, std::memory_order_relaxed);
    const int stockResult = g_origFxFrustumCollision(frustum, sphere);
    if (!stockResult)
        g_fxNearRescues.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

bool InstallIm3DDiagnostics(void* handle) {
    g_origIm3DTransform = Resolve<Im3DTransformFn>(
        handle, "_Z15RwIm3DTransformP18RxObjSpace3DVertexjP11RwMatrixTagj");
    g_origIm3DRenderPrimitive = Resolve<Im3DRenderPrimitiveFn>(
        handle, "_Z21RwIm3DRenderPrimitive15RwPrimitiveType");
    Dl_info info{};
    if (!g_fn.fxRender ||
        dladdr(reinterpret_cast<void*>(g_fn.fxRender), &info) == 0 ||
        !info.dli_fbase) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    static constexpr std::uint32_t kTransformPlt[4] = {
        0xb0000230u, 0xf9464211u, 0x91320210u, 0xd61f0220u,
    };
    static constexpr std::uint32_t kPrimitivePlt[4] = {
        0xd0000210u, 0xf9421611u, 0x9110a210u, 0xd61f0220u,
    };
    const bool transform = InstallRetailGotHook(
        "transform", base, 0x7562ecu, 0x7fe480u, 0x843c80u,
        kTransformPlt, reinterpret_cast<void*>(g_origIm3DTransform),
        reinterpret_cast<void*>(&OnIm3DTransform));
    const bool primitive = InstallRetailGotHook(
        "primitive", base, 0x7565e0u, 0x8053d0u, 0x847428u,
        kPrimitivePlt, reinterpret_cast<void*>(g_origIm3DRenderPrimitive),
        reinterpret_cast<void*>(&OnIm3DRenderPrimitive));
    LOGI("[gfxfx.im3d] diagnostics ready=%d transform=%d primitive=%d",
         (transform && primitive) ? 1 : 0, transform ? 1 : 0,
         primitive ? 1 : 0);
    return transform && primitive;
}

bool InstallFxFrustumFix(void* handle) {
    g_origFxManagerUpdate = Resolve<FxManagerUpdateFn>(
        handle, "_ZN11FxManager_c6UpdateEP8RwCameraf");
    g_origFxFrustumCollision = Resolve<FxFrustumCollisionFn>(
        handle, "_ZN15FxFrustumInfo_c11IsCollisionEP10FxSphere_c");
    g_rwFrameGetLtm = Resolve<RwFrameGetLtmFn>(
        handle, "_Z13RwFrameGetLTMP7RwFrame");
    Dl_info info{};
    if (!g_fn.fxRender || !g_rwFrameGetLtm ||
        dladdr(reinterpret_cast<void*>(g_fn.fxRender), &info) == 0 ||
        !info.dli_fbase) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    static constexpr std::uint32_t kUpdatePlt[4] = {
        0xd0000210u, 0xf9419611u, 0x910ca210u, 0xd61f0220u,
    };
    static constexpr std::uint32_t kCollisionPlt[4] = {
        0xd0000210u, 0xf943a611u, 0x911d2210u, 0xd61f0220u,
    };
    const bool update = InstallRetailGotHook(
        "fx update camera", base, 0x4df850u, 0x8051d0u, 0x847328u,
        kUpdatePlt, reinterpret_cast<void*>(g_origFxManagerUpdate),
        reinterpret_cast<void*>(&OnFxManagerUpdate));
    const bool collision = update && InstallRetailGotHook(
        "nearby fx frustum", base, 0x4e0754u, 0x805a10u, 0x847748u,
        kCollisionPlt, reinterpret_cast<void*>(g_origFxFrustumCollision),
        reinterpret_cast<void*>(&OnFxFrustumCollision));
    LOGI("[gfxfx.frustum] ready=%d update=%d collision=%d range_m=20.0",
         (update && collision) ? 1 : 0, update ? 1 : 0,
         collision ? 1 : 0);
    return update && collision;
}

bool ResolveCoronaViewSync(void* handle) {
    g_fn.matrixUpdate = Resolve<CMatrixUpdateFn>(
        handle, "_ZN7CMatrix6UpdateEv");
    auto* const theCamera = Resolve<std::uint8_t*>(handle, "TheCamera");

    Dl_info info{};
    if (!g_fn.matrixUpdate || !theCamera || theCamera != g.TheCamera ||
        dladdr(reinterpret_cast<void*>(g_fn.matrixUpdate), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.citylight] eye view sync disabled: missing symbols");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool offsetsMatch =
        reinterpret_cast<std::uintptr_t>(g_fn.matrixUpdate) - base ==
            0x52de40u &&
        reinterpret_cast<std::uintptr_t>(theCamera) - base == 0x9f86f8u;
    static constexpr std::uint32_t kMatrixUpdateEntry[4] = {
        0xf9402008u, 0xbd400100u, 0xbd000000u, 0xbd400500u,
    };
    std::uint32_t observedEntry[4]{};
    std::memcpy(observedEntry,
                reinterpret_cast<const void*>(g_fn.matrixUpdate),
                sizeof(observedEntry));
    // Retail CSprite::CalcScreenCoors @ 0x5f449c passes TheCamera+0xa10
    // to CMatrix*CVector at 0x5f44ec. Guard that exact dependency before
    // touching the cached matrix at the same offset.
    const bool projectionMatch =
        *reinterpret_cast<const std::uint32_t*>(base + 0x5f44ecu) ==
            0x91284100u;
    const bool entryMatch =
        std::memcmp(observedEntry, kMatrixUpdateEntry,
                    sizeof(observedEntry)) == 0;
    const bool ready = offsetsMatch && projectionMatch && entryMatch;
    LOGI("[gfxfx.citylight] view_sync_ready=%d offsets=%d projection=%d "
         "entry=%d camera=%p update=%p",
         ready ? 1 : 0, offsetsMatch ? 1 : 0,
         projectionMatch ? 1 : 0, entryMatch ? 1 : 0,
         theCamera, reinterpret_cast<void*>(g_fn.matrixUpdate));
    return ready;
}

bool InstallEyeBeginViewSync(void* handle) {
    auto* const resolved = Resolve<RwCameraBeginUpdateFn>(
        handle, "_Z19RwCameraBeginUpdateP8RwCamera");
    auto* const scene = static_cast<std::uint8_t*>(dlsym(handle, "Scene"));
    if (!g_scene) g_scene = scene;
    Dl_info info{};
    if (!g_coronaViewSyncReady || !scene || g_scene != scene || !resolved ||
        g.RwCameraBeginUpdate != resolved ||
        dladdr(reinterpret_cast<void*>(resolved), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.citylight] pre-scene sync disabled: owner mismatch");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool layoutMatch =
        reinterpret_cast<std::uintptr_t>(resolved) - base == 0x765678u &&
        reinterpret_cast<std::uintptr_t>(g_scene) - base == 0xcd0868u;
    if (!layoutMatch) {
        LOGW("[gfxfx.citylight] pre-scene sync disabled: retail layout mismatch");
        return false;
    }

    // The Quest eye loop deliberately calls this shared symbol pointer. Wrap
    // that one local dispatch point instead of editing the concurrently-owned
    // camera renderer or patching every libGame callsite.
    g_origRwCameraBeginUpdate = resolved;
    g.RwCameraBeginUpdate = &OnRwCameraBeginUpdate;
    LOGI("[gfxfx.citylight] pre_scene_sync_ready=1 begin=%p scene=%p",
         reinterpret_cast<void*>(resolved), g_scene);
    return true;
}

bool ResolveCoronaBillboardState(void* handle) {
    auto* const coronas = static_cast<std::uint8_t*>(
        dlsym(handle, "_ZN8CCoronas8aCoronasE"));
    auto** const textures = static_cast<void**>(
        dlsym(handle, "gpCoronaTexture"));
    auto* const foggyness = Resolve<float*>(
        handle, "_ZN8CWeather9FoggynessE");
    Dl_info info{};
    if (!g_fn.coronas || !coronas || !textures || !foggyness ||
        !g_fn.renderStateSet || !g.RwIm3DTransform ||
        !g.RwIm3DRenderIndexedPrimitive || !g.RwIm3DEnd ||
        dladdr(reinterpret_cast<void*>(g_fn.coronas), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.corona3d] disabled: missing symbols");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool layoutMatch =
        reinterpret_cast<std::uintptr_t>(g_fn.coronas) - base == 0x5ce048u &&
        reinterpret_cast<std::uintptr_t>(coronas) - base == 0xc6c7f0u &&
        reinterpret_cast<std::uintptr_t>(textures) - base == 0xc6c788u &&
        reinterpret_cast<std::uintptr_t>(foggyness) - base == 0xcc7468u &&
        reinterpret_cast<std::uintptr_t>(g.RwIm3DTransform) - base ==
            0x7562ecu &&
        reinterpret_cast<std::uintptr_t>(
            g.RwIm3DRenderIndexedPrimitive) - base == 0x756438u &&
        reinterpret_cast<std::uintptr_t>(g.RwIm3DEnd) - base == 0x7563ecu &&
        reinterpret_cast<std::uintptr_t>(g_fn.renderStateSet) - base ==
            0x75dabcu &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5ce148u) ==
            0xf101033fu &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5ce150u) ==
            0x52800a08u &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5ce464u) ==
            0x3940fa68u &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5ce484u) ==
            0x7100091fu &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5cf8c4u) ==
            0x3900f934u;
    if (!layoutMatch) {
        LOGW("[gfxfx.corona3d] disabled: retail layout mismatch");
        return false;
    }
    g_coronaArray = coronas;
    g_coronaTextures = textures;
    g_weatherFoggyness = foggyness;
    LOGI("[gfxfx.corona3d] ready=1 array=%p textures=%p fog=%p",
         coronas, textures, foggyness);
    return true;
}

bool ResolveEssentialLightStereoState(void* handle) {
    auto* const numBrightLights = Resolve<int*>(
        handle, "_ZN13CBrightLights15NumBrightLightsE");
    auto* const brightLights = Resolve<std::uint8_t*>(
        handle, "_ZN13CBrightLights13aBrightLightsE");
    auto* const numShinyTexts = Resolve<int*>(
        handle, "_ZN11CShinyTexts13NumShinyTextsE");
    auto* const shinyTexts = Resolve<std::uint8_t*>(
        handle, "_ZN11CShinyTexts11aShinyTextsE");

    Dl_info info{};
    if (!g_fn.coronas || !g_fn.brightLights || !g_fn.shinyTexts ||
        !numBrightLights || !brightLights || !numShinyTexts || !shinyTexts ||
        dladdr(reinterpret_cast<void*>(g_fn.coronas), &info) == 0 ||
        !info.dli_fbase) {
        LOGW("[gfxfx.citylight] stereo queues disabled: missing retail symbols");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const bool offsetsMatch =
        reinterpret_cast<std::uintptr_t>(g_fn.coronas) - base == 0x5ce048u &&
        reinterpret_cast<std::uintptr_t>(g_fn.brightLights) - base == 0x5f093cu &&
        reinterpret_cast<std::uintptr_t>(g_fn.shinyTexts) - base == 0x5ef42cu &&
        reinterpret_cast<std::uintptr_t>(numBrightLights) - base == 0xca0690u &&
        reinterpret_cast<std::uintptr_t>(brightLights) - base == 0xca0694u &&
        reinterpret_cast<std::uintptr_t>(numShinyTexts) - base == 0xca0d94u &&
        reinterpret_cast<std::uintptr_t>(shinyTexts) - base == 0xca0d98u;

    static constexpr std::uint32_t kCoronaEntry[4] = {
        0xd10583ffu, 0x6d0c3befu, 0x6d0d33edu, 0x6d0e2bebu,
    };
    static constexpr std::uint32_t kBrightEntry[4] = {
        0xd10303ffu, 0x6d023befu, 0x6d0333edu, 0x6d042bebu,
    };
    static constexpr std::uint32_t kShinyEntry[4] = {
        0xd101c3ffu, 0xa9017bfdu, 0xa9026ffcu, 0xa90367fau,
    };
    std::uint32_t observedCorona[4]{};
    std::uint32_t observedBright[4]{};
    std::uint32_t observedShiny[4]{};
    std::memcpy(observedCorona, reinterpret_cast<const void*>(g_fn.coronas),
                sizeof(observedCorona));
    std::memcpy(observedBright, reinterpret_cast<const void*>(g_fn.brightLights),
                sizeof(observedBright));
    std::memcpy(observedShiny, reinterpret_cast<const void*>(g_fn.shinyTexts),
                sizeof(observedShiny));
    const bool entriesMatch =
        std::memcmp(observedCorona, kCoronaEntry, sizeof(observedCorona)) == 0 &&
        std::memcmp(observedBright, kBrightEntry, sizeof(observedBright)) == 0 &&
        std::memcmp(observedShiny, kShinyEntry, sizeof(observedShiny)) == 0;
    const bool destructiveClearsMatch = offsetsMatch && entriesMatch &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5f0fccu) ==
            0xb90002bfu &&
        *reinterpret_cast<const std::uint32_t*>(base + 0x5ef734u) ==
            0xb90002dfu;
    const bool ready = offsetsMatch && entriesMatch && destructiveClearsMatch;
    if (ready) {
        g_numBrightLights = numBrightLights;
        g_brightLights = brightLights;
        g_numShinyTexts = numShinyTexts;
        g_shinyTexts = shinyTexts;
    }
    LOGI("[gfxfx.citylight] stereo_ready=%d offsets=%d entries=%d clears=%d "
         "bright=%p/%p shiny=%p/%p",
         ready ? 1 : 0, offsetsMatch ? 1 : 0, entriesMatch ? 1 : 0,
         destructiveClearsMatch ? 1 : 0, numBrightLights, brightLights,
         numShinyTexts, shinyTexts);
    return ready;
}

bool PrepareEssentialLightQueues(int eye) {
    if (!g_essentialLightStereoReady || !g_numBrightLights ||
        !g_brightLights || !g_numShinyTexts || !g_shinyTexts) {
        return false;
    }

    if (eye == 0) {
        g_essentialLightSnapshotValid = false;
        const int brightCount = *g_numBrightLights;
        const int shinyCount = *g_numShinyTexts;
        if (brightCount < 0 || brightCount > kEssentialLightQueueCapacity ||
            shinyCount < 0 || shinyCount > kEssentialLightQueueCapacity) {
            ++g_aggregate.essentialLightReplayMisses;
            return false;
        }
        g_brightLightSnapshotCount = brightCount;
        g_shinyTextSnapshotCount = shinyCount;
        std::memcpy(g_brightLightSnapshot, g_brightLights,
                    static_cast<std::size_t>(brightCount) *
                        kBrightLightStride);
        std::memcpy(g_shinyTextSnapshot, g_shinyTexts,
                    static_cast<std::size_t>(shinyCount) * kShinyTextStride);
        g_aggregate.brightLightSlotsMax = std::max(
            g_aggregate.brightLightSlotsMax,
            static_cast<std::uint64_t>(brightCount));
        g_aggregate.shinyTextSlotsMax = std::max(
            g_aggregate.shinyTextSlotsMax,
            static_cast<std::uint64_t>(shinyCount));
        ++g_aggregate.essentialLightCaptures;
        g_essentialLightSnapshotValid = true;
        return true;
    }

    if (eye != 1 || !g_essentialLightSnapshotValid) {
        ++g_aggregate.essentialLightReplayMisses;
        return false;
    }
    std::memcpy(g_brightLights, g_brightLightSnapshot,
                static_cast<std::size_t>(g_brightLightSnapshotCount) *
                    kBrightLightStride);
    std::memcpy(g_shinyTexts, g_shinyTextSnapshot,
                static_cast<std::size_t>(g_shinyTextSnapshotCount) *
                    kShinyTextStride);
    *g_numBrightLights = g_brightLightSnapshotCount;
    *g_numShinyTexts = g_shinyTextSnapshotCount;
    g_essentialLightSnapshotValid = false;
    ++g_aggregate.essentialLightRestores;
    return true;
}

bool ResolveFunctions() {
    if (g_resolveAttempted.exchange(true, std::memory_order_acq_rel))
        return g_ready;

    void* handle = dlopen("libGame.so", RTLD_NOLOAD | RTLD_NOW);
    if (!handle) {
        LOGE("[gfxfx] libGame.so unavailable: %s", dlerror());
        return false;
    }

    g_fn.renderStateSet = Resolve<RenderStateSetFn>(
        handle, "_Z16RwRenderStateSet13RwRenderStatePv");
    g_fn.renderStateGet = Resolve<RenderStateGetFn>(
        handle, "_Z16RwRenderStateGet13RwRenderStatePv");
    g_fn.definedState = Resolve<VoidFn>(handle, "_Z12DefinedStatev");
    g_fn.birds = Resolve<VoidFn>(handle, "_ZN6CBirds6RenderEv");
    g_fn.skidmarks = Resolve<VoidFn>(handle, "_ZN10CSkidmarks6RenderEv");
    g_fn.ropes = Resolve<VoidFn>(handle, "_ZN6CRopes6RenderEv");
    g_fn.glass = Resolve<VoidFn>(handle, "_ZN6CGlass6RenderEv");
    g_fn.movingThings = Resolve<VoidFn>(handle, "_ZN13CMovingThings6RenderEv");
    g_fn.coronas = Resolve<VoidFn>(handle, "_ZN8CCoronas6RenderEv");
    g_fn.fxRender = Resolve<FxRenderFn>(handle, "_ZN4Fx_c6RenderEP8RwCamerah");
    g_fn.fx = dlsym(handle, "g_fx");
    g_fn.waterCannons = Resolve<VoidFn>(handle, "_ZN13CWaterCannons6RenderEv");
    g_fn.heliPreSearchlight = Resolve<VoidFn>(handle, "_ZN5CHeli19Pre_SearchLightConeEv");
    g_fn.heliSearchlights = Resolve<VoidFn>(handle, "_ZN5CHeli25RenderAllHeliSearchLightsEv");
    g_fn.scriptSearchlights = Resolve<VoidFn>(handle, "_ZN11CTheScripts21RenderAllSearchLightsEv");
    g_fn.heliPostSearchlight = Resolve<VoidFn>(handle, "_ZN5CHeli20Post_SearchLightConeEv");
    // CSpecialFX::Render is intentionally split. Its first two children are
    // temporal motion-blur streaks and stock bullet traces (the XR compositor
    // already owns a stereo-safe tracer pass); the remaining four are useful,
    // low-cost world effects and gameplay markers.
    g_fn.brightLights = Resolve<VoidFn>(handle, "_ZN13CBrightLights6RenderEv");
    g_fn.shinyTexts = Resolve<VoidFn>(handle, "_ZN11CShinyTexts6RenderEv");
    g_fn.markers = Resolve<VoidFn>(handle, "_ZN10C3dMarkers6RenderEv");
    auto* const markerArray = static_cast<std::uint8_t*>(
        dlsym(handle, "_ZN10C3dMarkers14m_aMarkerArrayE"));
    g_fn.checkpoints = Resolve<VoidFn>(handle, "_ZN12CCheckpoints6RenderEv");
    g_fn.pointLightFog = Resolve<VoidFn>(handle, "_ZN12CPointLights15RenderFogEffectEv");
    ResolveWeatherState(handle);
    g_coronaViewSyncReady = ResolveCoronaViewSync(handle);
    g_essentialLightStereoReady = ResolveEssentialLightStereoState(handle);
    g_eyeTargetRoutingReady = ResolveEyeTargetRouting(handle);
    if (Profile() > 0) {
        g_shadowTailRoutingReady = InstallShadowTailRouting(handle);
        InstallHeadlightDiagnostics(handle);
    }
    if (Profile() == 1) {
        g_areaWeaponRoutingReady = InstallAreaWeaponRouting(handle);
        // Eye-target routing, not a global visibility override, is the proven
        // requirement for stock Fx particles in the XR rasters.  The former
        // nearby rescue hooked FxFrustumInfo::IsCollision globally; that also
        // changed creation and per-frame emission for every FX system within
        // 20 m (including area-weapon effects emitted behind the player).
        // Restore retail culling and keep the two target-routing fixes only.
        g_fxFrustumFixReady = false;
        LOGI("[gfxfx.frustum] stock culling restored; nearby rescue disabled");
        g_im3dHooksReady = InstallIm3DDiagnostics(handle);
    }
    if (Profile() > 0) {
        g_trafficLightQualityReady = InstallTrafficLightQualityGate(handle);
        g_coronaBillboardReady = ResolveCoronaBillboardState(handle);
        g_eyeBeginViewSyncReady = InstallEyeBeginViewSync(handle);
        g_embeddedCanopyHideReady = InstallEmbeddedCanopyHide(handle);
        g_lampGroundPoolsReady = InstallLampGroundPools(handle);
        g_wetReflectionsReady = InstallWetRoadReflections(handle);
    }

    const bool fireReady = g_eyeTargetRoutingReady && g_fn.definedState &&
        g_fn.fxRender && g_fn.fx;
    const bool bloodReady = fireReady && g_shadowTailRoutingReady;
    const bool surfaceReady = g_fn.renderStateSet && g_fn.skidmarks &&
        g_fn.glass;
    const bool lightweightReady = surfaceReady && g_fn.ropes &&
        g_fn.waterCannons;
    const bool balancedReady = surfaceReady && g_coronaBillboardReady &&
        g_fn.fxRender && g_fn.fx &&
        g_fn.waterCannons && g_fn.brightLights && g_fn.shinyTexts &&
        g_fn.markers && g_fn.checkpoints && g_fn.pointLightFog &&
        g_essentialLightStereoReady;
    const bool extendedReady = g_fn.birds && g_fn.ropes &&
        g_fn.movingThings && g_fn.heliPreSearchlight &&
        g_fn.heliSearchlights && g_fn.scriptSearchlights &&
        g_fn.heliPostSearchlight;
    const int profile = Profile();
    g_ready = fireReady && (profile < 1 || lightweightReady) &&
        (profile < 2 || balancedReady) &&
        (profile < 3 || extendedReady);
    Dl_info markerInfo{};
    const bool markerBaseReady = g_fn.markers && markerArray &&
        dladdr(reinterpret_cast<void*>(g_fn.markers), &markerInfo) != 0 &&
        markerInfo.dli_fbase;
    if (markerBaseReady) {
        const auto base = reinterpret_cast<std::uintptr_t>(markerInfo.dli_fbase);
        g_markerStereoStateReady =
            reinterpret_cast<std::uintptr_t>(g_fn.markers) - base == 0x5ef79cu &&
            reinterpret_cast<std::uintptr_t>(markerArray) - base == 0xca18d0u;
    }
    g_markerArray = g_markerStereoStateReady ? markerArray : nullptr;
    LOGI("[gfxfx] symbols ready=%d profile=%d fire=%d blood=%d balanced=%d "
          "extended=%d defined=%d rs=%d eye_target=%d shadow_tail=%d "
          "light_diag=%d citylight=%d corona_view=%d eye_begin=%d "
          "traffic_gate=%d corona3d=%d lamp_pool=%d wet_reflect=%d "
          "underwater=%d area_ray=%d "
          "flame_create_retry=%d flame_lifecycle=%d fx_frustum=%d "
         "im3d_diag=%d "
         "skid=%d glass=%d rope=%d corona=%d particles=%d water=%d fog=%d",
         g_ready ? 1 : 0, profile, fireReady ? 1 : 0,
         bloodReady ? 1 : 0, balancedReady ? 1 : 0,
         extendedReady ? 1 : 0,
         g_fn.definedState ? 1 : 0, g_fn.renderStateSet ? 1 : 0,
         g_eyeTargetRoutingReady ? 1 : 0,
         g_shadowTailRoutingReady ? 1 : 0,
         g_headlightDiagnosticsReady ? 1 : 0,
         g_essentialLightStereoReady ? 1 : 0,
         g_coronaViewSyncReady ? 1 : 0,
         g_eyeBeginViewSyncReady ? 1 : 0,
         g_trafficLightQualityReady ? 1 : 0,
         g_coronaBillboardReady ? 1 : 0,
         g_lampGroundPoolsReady ? 1 : 0,
         g_wetReflectionsReady ? 1 : 0,
         g_underwaterStateReady ? 1 : 0,
         g_areaWeaponRoutingReady ? 1 : 0,
         g_flameCreateRetryReady ? 1 : 0,
         g_flameLifecycleReady ? 1 : 0,
         g_fxFrustumFixReady ? 1 : 0,
         g_im3dHooksReady ? 1 : 0,
         g_fn.skidmarks ? 1 : 0, g_fn.glass ? 1 : 0,
         g_fn.ropes ? 1 : 0,
         g_fn.coronas ? 1 : 0, (g_fn.fxRender && g_fn.fx) ? 1 : 0,
         g_fn.waterCannons ? 1 : 0, g_fn.pointLightFog ? 1 : 0);
    LOGI("[gfxfx.marker] stereo_state=%d array=%p",
         g_markerStereoStateReady ? 1 : 0, g_markerArray);
    return g_ready;
}

void Call(VoidFn fn) {
    if (fn) fn();
}

void Report(double elapsedMs, int eye) {
    ++g_aggregate.calls;
    if (eye == 0) ++g_aggregate.leftCalls;
    if (eye == 1) ++g_aggregate.rightCalls;
    g_aggregate.totalWallMs += elapsedMs;
    g_aggregate.maxWallMs = std::max(g_aggregate.maxWallMs, elapsedMs);
    if (g_aggregate.calls < kReportIntervalCalls) return;

    LOGI("[gfxfx.perf] profile=%d calls=%llu left=%llu right=%llu "
         "wall_avg=%.3f wall_max=%.3f pair_avg=%.3f "
         "xform=%llu xL=%llu xR=%llu xok=%llu verts=%llu tex=%llu "
         "tex0=%llu prim=%llu primok=%llu probe=%d probe_apply=%llu "
         "state_ok=%d zt=%u zw=%u va=%u sb=%u db=%u at=%u ar=%u "
         "rt=%llu rt_back=%llu rt_eye=%llu rt_other=%llu rt_ok=%llu "
         "shadow=%llu shadow_stereo=%llu shadow_ok=%llu shadow_flat=%llu "
          "shadow_fail=%llu light_store=%llu light_tex=%llu "
          "light_static=%llu light_head=%llu point=%llu "
          "city_ready=%d corona=%llu bright=%llu shiny=%llu "
          "light_cap=%llu light_restore=%llu light_miss=%llu "
          "bright_q=%llu shiny_q=%llu "
          "flame_route=%llu flame_fallback=%llu flame_create=%llu "
          "flame_stock=%llu flame_retry=%llu flame_retry_ok=%llu "
          "flame_create_fail=%llu flame_system=%llu flame_system_new=%llu "
          "flame_keep=%llu flame_kill=%llu "
          "near=%llu rescued=%llu",
         Profile(), static_cast<unsigned long long>(g_aggregate.calls),
         static_cast<unsigned long long>(g_aggregate.leftCalls),
         static_cast<unsigned long long>(g_aggregate.rightCalls),
         g_aggregate.totalWallMs / static_cast<double>(g_aggregate.calls),
         g_aggregate.maxWallMs,
         2.0 * g_aggregate.totalWallMs /
             static_cast<double>(g_aggregate.calls),
         static_cast<unsigned long long>(g_aggregate.im3dTransformCalls),
         static_cast<unsigned long long>(g_aggregate.im3dTransformLeft),
         static_cast<unsigned long long>(g_aggregate.im3dTransformRight),
         static_cast<unsigned long long>(g_aggregate.im3dTransformSuccess),
         static_cast<unsigned long long>(g_aggregate.im3dVertices),
         static_cast<unsigned long long>(g_aggregate.im3dTextureRaster),
         static_cast<unsigned long long>(g_aggregate.im3dNullTextureRaster),
         static_cast<unsigned long long>(g_aggregate.im3dPrimitiveCalls),
         static_cast<unsigned long long>(g_aggregate.im3dPrimitiveSuccess),
         FxProbeMode(),
         static_cast<unsigned long long>(g_aggregate.probeApplications),
         g_aggregate.renderStateSampled ? 1 : 0,
         g_aggregate.zTest, g_aggregate.zWrite,
         g_aggregate.vertexAlpha, g_aggregate.srcBlend,
         g_aggregate.dstBlend, g_aggregate.alphaTest,
         g_aggregate.alphaRef,
         static_cast<unsigned long long>(g_aggregate.targetSelectCalls),
         static_cast<unsigned long long>(g_aggregate.targetBeforeBack),
         static_cast<unsigned long long>(g_aggregate.targetBeforeEye),
         static_cast<unsigned long long>(g_aggregate.targetBeforeOther),
         static_cast<unsigned long long>(g_aggregate.targetAfterEye),
         static_cast<unsigned long long>(g_aggregate.shadowResolveCalls),
         static_cast<unsigned long long>(g_aggregate.shadowResolveStereo),
         static_cast<unsigned long long>(g_aggregate.shadowTailRouted),
         static_cast<unsigned long long>(g_aggregate.shadowTailFlatSkips),
         static_cast<unsigned long long>(g_aggregate.shadowTailFailures),
         static_cast<unsigned long long>(
             g_headlightStoreCalls.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_headlightTextureCalls.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_aggregate.staticShadowSlotsMax),
         static_cast<unsigned long long>(g_aggregate.headlightShadowSlotsMax),
         static_cast<unsigned long long>(g_aggregate.pointLightsMax),
         g_essentialLightStereoReady ? 1 : 0,
         static_cast<unsigned long long>(g_aggregate.coronaCalls),
         static_cast<unsigned long long>(g_aggregate.brightLightCalls),
         static_cast<unsigned long long>(g_aggregate.shinyTextCalls),
         static_cast<unsigned long long>(g_aggregate.essentialLightCaptures),
         static_cast<unsigned long long>(g_aggregate.essentialLightRestores),
         static_cast<unsigned long long>(
             g_aggregate.essentialLightReplayMisses),
         static_cast<unsigned long long>(g_aggregate.brightLightSlotsMax),
         static_cast<unsigned long long>(g_aggregate.shinyTextSlotsMax),
         static_cast<unsigned long long>(
             g_flameRoutedCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameFallbackCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameCreateAttempts.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameCreateStockSuccess.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameCreateRetries.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameCreateRetrySuccess.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameCreateFailures.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_flameSystemPresentCalls.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(
              g_flameSystemNewCalls.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(
              g_flameLifecyclePreserves.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(
              g_flameLifecycleKills.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(
             g_fxNearChecks.exchange(0, std::memory_order_relaxed)),
          static_cast<unsigned long long>(
             g_fxNearRescues.exchange(0, std::memory_order_relaxed)));
    LOGI("[gfxfx.citylight] begin_sync=%llu traffic_quality_raised=%llu "
         "corona3d_supported=%llu corona3d_visible=%llu "
         "corona3d_records=%llu corona3d_draws=%llu attached_skips=%llu",
         static_cast<unsigned long long>(
             g_eyeBeginViewSyncCalls.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_trafficLightQualityRaises.exchange(
                  0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_coronaBillboardSupportedRecords.exchange(
                 0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_coronaBillboardVisibleRecords.exchange(
                 0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_coronaBillboardRecords.exchange(
                 0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_coronaBillboardDraws.exchange(
                 0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_coronaBillboardAttachedSkips.exchange(
                 0, std::memory_order_relaxed)));
    const float underWaterness = g_weatherUnderWaterness &&
            std::isfinite(*g_weatherUnderWaterness)
        ? *g_weatherUnderWaterness : 0.0f;
    const float waterDepth = g_weatherWaterDepth &&
            std::isfinite(*g_weatherWaterDepth)
        ? *g_weatherWaterDepth : 0.0f;
    LOGI("[gfxfx.surface] lamp_candidate=%llu lamp_match=%llu "
         "lamp_ok=%llu lamp_fail=%llu wet_call=%llu wet_candidate=%llu "
         "wet_record=%llu wet_draw=%llu underwater=%.3f depth=%.3f",
         static_cast<unsigned long long>(
             g_lampCandidates.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_lampCoronaMatches.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_lampStoreSuccess.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_lampStoreFailures.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_wetReflectionCalls.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_wetReflectionCandidates.exchange(
                 0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_wetReflectionRecords.exchange(0, std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             g_wetReflectionDraws.exchange(0, std::memory_order_relaxed)),
         static_cast<double>(underWaterness),
         static_cast<double>(waterDepth));
    g_aggregate = {};
}

} // namespace

int Profile() {
    static const int profile = [] {
        char text[PROP_VALUE_MAX]{};
        int value = kDefaultProfile;
        if (__system_property_get(kEffectsProperty, text) > 0) {
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(text, &end, 10);
            if (errno == 0 && end != text && end && *end == '\0' &&
                parsed >= 0 && parsed <= 3) {
                value = static_cast<int>(parsed);
            } else {
                LOGW("[gfxfx] ignoring invalid %s=%s (valid 0..3)",
                     kEffectsProperty, text);
            }
        }
        LOGI("[gfxfx] process profile=%d property=%s", value, kEffectsProperty);
        return value;
    }();
    return profile;
}

void RenderSkyEye(void* rwCamera, int eye) {
    if (!rwCamera || (eye != 0 && eye != 1)) return;
    RenderStereoSkyObjects(rwCamera, eye);
}

void RenderEye(void* rwCamera, int eye) {
    const int profile = Profile();
    // Invalidate the paired destructive-queue replay before any eye-0 early
    // return. A transient target/symbol failure must never let eye 1 consume a
    // snapshot retained from the previous frame.
    if (eye == 0) g_essentialLightSnapshotValid = false;
    // Published BEFORE the profile gate: the underwater grade runs in the XR
    // compositor, not in the recorded eye passes, and profile 0 (the default
    // whenever the debug.savr.effects property is unset) must not kill it.
    PublishUnderwaterState(eye);
    if (profile == 0 || !rwCamera || (eye != 0 && eye != 1)) return;

    const bool symbolsReady = ResolveFunctions();
    const bool fullFlatSuppressed =
        g_flatPassSuppressed.load(std::memory_order_acquire);
    const bool fireFlatSuppressed =
        g_fireFlatPassSuppressed.load(std::memory_order_acquire);
    const bool passReady = fullFlatSuppressed ||
        (profile == 1 && fireFlatSuppressed);
    const std::uint64_t attempt =
        g_renderAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt == 1 || attempt % kReportIntervalCalls == 0) {
        LOGI("[gfxfx.guard] profile=%d ready=%d full_flat=%d fire_flat=%d "
             "pass=%d attempts=%llu",
             profile, symbolsReady ? 1 : 0, fullFlatSuppressed ? 1 : 0,
             fireFlatSuppressed ? 1 : 0, passReady ? 1 : 0,
             static_cast<unsigned long long>(attempt));
    }
    if (!passReady || !symbolsReady) return;

    const double startMs = MonotonicMs();

    if (!SelectCurrentCameraRasterTarget(rwCamera, eye)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_acq_rel))
            LOGE("[gfxfx.target] runtime eye target selection failed");
        return;
    }

    // Only a fully suppressed stock RenderEffects pass transfers ownership of
    // these queues to the stereo path.  The narrower fire-only fallback leaves
    // stock city lights alive and must never consume or duplicate them here.
    const bool moveEssentialLights =
        fullFlatSuppressed && g_essentialLightStereoReady &&
        g_fn.renderStateSet;
    const bool essentialLightQueuesReady =
        moveEssentialLights && PrepareEssentialLightQueues(eye);
    // Preserve the stock RenderEffects order. RenderScene has already recorded
    // opaque world geometry and shadows into this eye. These calls only append
    // late world-space/alpha geometry; no simulation update is performed here.
    if (profile >= 3) Call(g_fn.birds);
    if (profile >= 1) {
        g_fn.renderStateSet(2, reinterpret_cast<void*>(1));
        Call(g_fn.skidmarks);
        Call(g_fn.ropes);
        Call(g_fn.glass);
        if (profile >= 3) Call(g_fn.movingThings);
    }
    // RenderScene leaves several backend states tailored to its final world
    // batch. The stock late-effects path starts from the game's canonical 3D
    // state, while this injected per-eye path used to enter Fx directly. Fx
    // sets its texture, blend and Z-write state, but deliberately inherits the
    // remaining raster/alpha/depth state. Re-establish the exact retail base
    // state before recording its immediate-mode fire triangles into each eye.
    g_fn.definedState();
    if (moveEssentialLights && g_coronaBillboardReady &&
        RenderStereoCoronaBillboards(rwCamera, eye)) {
        ++g_aggregate.coronaCalls;
        // The custom billboard pass establishes stock corona additive/depth
        // state and binds the last corona raster. DefinedState does not clear
        // texture state 1, so release it explicitly before Fx_c establishes
        // the canonical retail 3D baseline for its own emitters.
        g_fn.renderStateSet(1, nullptr);
        g_fn.definedState();
    }

    // Fx_c is the render owner for vehicle fire and its smoke/particle systems.
    // The other profile-1 calls above/below are situational and return quickly
    // when no skid, glass, rope or water-cannon geometry is active.
    const int probeMode = FxProbeMode();
    const RenderStateSnapshot preProbeState = probeMode > 0
        ? CaptureRenderState()
        : RenderStateSnapshot{};
    g_insideFxRender = true;
    g_fxEye = eye;
    g_fn.fxRender(g_fn.fx, rwCamera, 0);
    g_fxEye = -1;
    g_insideFxRender = false;
    if (probeMode > 0) RestoreRenderState(preProbeState);
    if (profile >= 1) Call(g_fn.waterCannons);
    if (profile >= 3) {
        Call(g_fn.heliPreSearchlight);
        Call(g_fn.heliSearchlights);
        Call(g_fn.scriptSearchlights);
        Call(g_fn.heliPostSearchlight);
    }
    if (essentialLightQueuesReady) {
        Call(g_fn.brightLights);
        ++g_aggregate.brightLightCalls;
        // CBrightLights disables vertex alpha at its tail; pedestrian traffic
        // signals in CShinyTexts need it explicitly restored.
        g_fn.renderStateSet(12, reinterpret_cast<void*>(1));
        Call(g_fn.shinyTexts);
        ++g_aggregate.shinyTextCalls;
    }
    if (profile >= 2) {
        Call(g_fn.checkpoints);
        Call(g_fn.pointLightFog);
    }

    // Deliberately excluded even from profile 2:
    // - CPostEffects::MobileRender and CCamera::RenderMotionBlur are full-screen,
    //   temporal, and use the flat mobile camera history (VR discomfort + fill).
    // - CWeaponEffects renders the stock screen-space aiming UI; VR owns aiming.
    // - CSpecialFX is split above to omit motion-blur streaks and duplicate
    //   bullet traces while retaining lights, text, markers, and checkpoints.
    // - RenderReallyDrawLastObjects consumes shared alpha-list state that is not
    //   safe to replay for the second eye without a dedicated snapshot.
    Report(MonotonicMs() - startMs, eye);
}

bool RenderStockMarkersEye(int eye) {
    // ResolveFunctions fills g_fn even when the selected aggregate effects
    // profile is zero. Marker availability is independent from whether the
    // other optional members make that aggregate profile "ready".
    ResolveFunctions();
    if (!g_fn.markers) return false;

    if (eye == 0) {
        g_markerSnapshotValid = false;
        if (g_markerStereoStateReady && g_markerArray) {
            for (int i = 0; i < kMarkerCount; ++i) {
                g_markerUsedSnapshot[i] =
                    g_markerArray[i * kMarkerStride + kMarkerUsedOffset] != 0;
            }
            g_markerSnapshotValid = true;
        }
    } else if (eye == 1 && g_markerSnapshotValid && g_markerArray) {
        for (int i = 0; i < kMarkerCount; ++i) {
            if (g_markerUsedSnapshot[i])
                g_markerArray[i * kMarkerStride + kMarkerUsedOffset] = 1;
        }
    }
    g_fn.markers();
    if (eye == 1) g_markerSnapshotValid = false;
    return true;
}

void SetFlatPassSuppressed(bool suppressed) {
    g_flatPassSuppressed.store(suppressed, std::memory_order_release);
    LOGI("[gfxfx] flat pass suppressed=%d", suppressed ? 1 : 0);
}

void SetFireFlatPassSuppressed(bool suppressed) {
    g_fireFlatPassSuppressed.store(suppressed, std::memory_order_release);
    LOGI("[gfxfx] fire flat pass suppressed=%d", suppressed ? 1 : 0);
}

} // namespace savr::graphicsfx
