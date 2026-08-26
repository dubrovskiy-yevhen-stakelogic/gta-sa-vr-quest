#include "Xr.h"

#include "Appearance.h"
#include "Calib.h"
#include "Cheats.h"
#include "Driving.h"
#include "FrameTarget.h"
#include "Holster.h"
#include "HudSettings.h"
#include "Locomotion.h"
#include "Log.h"
#include "PerfTelemetry.h"
#include "ScopeAim.h"
#include "TrafficCensus.h"
#include "VrCamera.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dirent.h>
#include <sys/system_properties.h>

// sRGB write control is an extension in GLES; the enum is not in gl3.h core.
#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace savr::xr {
namespace {

constexpr char kModVersion[] = "0.1.0.13";  // internal build id (4th digit)
#ifdef SAVR_DEV
constexpr const char* kModVersionShown = kModVersion;
#else
constexpr const char* kModVersionShown = "0.1.0";  // players see the release id
#endif

JavaVM*   g_hudTextVm{};
jobject   g_hudTextApplication{};
jmethodID g_renderVrTextMethod{};

struct HudTextState {
    std::u16string brief;
    std::u16string help;
    std::u16string big;
    std::u16string timers;
    int bigStyle{};
    std::uint64_t revision{};
};

std::mutex   g_hudTextMutex;
HudTextState g_hudTextState;

struct HudTextTexture {
    GLuint texture{};
    int width{};
    int height{};
    std::uint64_t revision{~std::uint64_t{0}};
    bool visible{};
};

HudTextTexture g_briefTextTexture;
HudTextTexture g_bigTextTexture;
HudTextTexture g_timerTextTexture;

// Head pose published by the render thread, read by the game thread. A plain
// mutex is enough: it is written once per compositor frame and read once per
// game frame, never in a hot loop.
std::mutex     g_headPoseMutex;
bool           g_headPoseValid = false;
XrPosef        g_headPose{};
XrPosef        g_eyePose[2]{};          // per-eye poses in local space (IPD apart)
float          g_eyeFovXDeg = 90.0f;   // sane default until views are located
XrPosef        g_renderHeadPose{};      // pose the game rendered the last frame at
bool           g_renderHeadPoseValid = false;
HandPose       g_handPose[2]{};         // per-hand tracked poses (guarded by g_headPoseMutex)

// --- stereo eye-texture bridge (see Xr.h) ------------------------------------
struct EyeBuffer {
    GLuint tex[2]{0, 0};        // colour texture per eye (shared across contexts)
    GLuint fbo[2]{0, 0};        // GameThread-side framebuffer wrapping each tex
    GLsync fence{nullptr};      // signalled when this buffer's eye renders finish
};
// Triple-buffered: at any instant one buffer may be on display (published), one
// may be latched by the consumer (consuming), so the producer always has a third
// free to render into without ever overwriting or freeing what the consumer
// holds. Two buffers are not enough — a free-running producer would reuse the
// buffer the consumer is still sampling and delete the fence it is waiting on.
constexpr int kEyeBuffers = 3;
EyeBuffer  g_eyeBuf[kEyeBuffers];
int        g_eyeW = 0, g_eyeH = 0;
bool       g_eyeReady = false;
std::mutex g_eyePublishMutex;
int        g_eyePublished = -1; // newest complete pair, -1 = none
int        g_eyeConsuming = -1; // buffer the consumer currently holds, -1 = none
bool       g_stereoShared = true;   // false if the XR context is NOT shared with the engine
std::atomic<float> g_stereoRenderFovXDeg{75.0f};   // FOV the game rendered eyes at

// Stereo: the game renders the world twice (once per eye) into a ring of
// kStereoSets texture pairs on the GameThread, and publishes a monotonically
// increasing sequence number after finishing each pair. The compositor samples a
// pair kStereoReadLag frames behind the writer, so (a) the writer is never
// overwriting the pair being sampled and (b) the async RenderQueue thread has
// always finished drawing it. Left and right always come from the SAME sequence,
// which is what removes the flicker and the head-motion double-vision — those
// were the two symptoms of sampling a torn / half-drawn pair.
constexpr int kStereoSets    = 3;
constexpr int kStereoReadLag = 1;   // frames the consumer trails the writer
std::atomic<unsigned int> g_stereoEyeTex[kStereoSets][2] = {};
std::atomic<unsigned int> g_stereoEyeDepth[kStereoSets][2] = {};
std::atomic<unsigned int> g_stereoHudTex[kStereoSets] = {};
std::atomic<float>        g_stereoEyeNear[kStereoSets][2] = {};
std::atomic<float>        g_stereoEyeFar[kStereoSets][2] = {};
std::atomic<float>        g_stereoUnderWaterness[kStereoSets] = {};
std::atomic<float>        g_stereoWaterDepth[kStereoSets] = {};
std::atomic<float>        g_pendingUnderWaterness{0.0f};
std::atomic<float>        g_pendingWaterDepth{0.0f};
std::atomic<int>          g_stereoEyeW{0}, g_stereoEyeH{0};
std::atomic<int>          g_stereoSeq{-1};   // newest published pair index (-1 = none)
std::atomic<int>          g_stereoGeneration[kStereoSets] = {};
std::atomic<std::uint64_t> g_stereoPublishedNs[kStereoSets] = {};
std::atomic<float>        g_stereoTanX{1.303f}, g_stereoTanY{1.365f};  // render frustum half-tangents
constexpr float           kStereoHalfIpd = 0.032f;   // must match VrCamera kVrHalfIpd
bool                      g_hasPerfExt = false;      // XR_EXT_performance_settings available
bool                      g_hasDisplayRefreshExt = false;
bool                      g_hasPerformanceMetricsExt = false;
PFN_xrEnumerateDisplayRefreshRatesFB g_enumerateDisplayRefreshRates = nullptr;
PFN_xrGetDisplayRefreshRateFB        g_getDisplayRefreshRate = nullptr;
PFN_xrRequestDisplayRefreshRateFB    g_requestDisplayRefreshRate = nullptr;
PFN_xrEnumeratePerformanceMetricsCounterPathsMETA g_enumerateMetricPaths = nullptr;
PFN_xrSetPerformanceMetricsStateMETA               g_setMetricsState = nullptr;
PFN_xrQueryPerformanceMetricsCounterMETA            g_queryMetric = nullptr;
XrPath                   g_appCpuFrameTimePath = XR_NULL_PATH;
XrPath                   g_appGpuFrameTimePath = XR_NULL_PATH;
bool                     g_metricsEnabled = false;
bool                     g_runtimeCpuValid = false;
bool                     g_runtimeGpuValid = false;
float                    g_runtimeCpuMs = 0.0f;
float                    g_runtimeGpuMs = 0.0f;
bool                     g_displayRefreshValid = false;
float                    g_displayRefreshHz = 0.0f;
bool                     g_displayRefreshRequestSupported = false;
float                    g_requestedDisplayRefreshHz =
    static_cast<float>(frame_target::kFps);
int                      g_displayRefreshRetryCount = 0;
bool                     g_resetPresentTiming = true;

// CPU/GPU performance hint levels, adjustable from the VR menu Graphics section.
const char* const         kPerfNames[4] = {"POWER SAVE", "SUSTAINED LOW", "SUSTAINED HIGH", "BOOST"};
std::atomic<int>          g_cpuPerfIdx{3};           // default BOOST
std::atomic<int>          g_gpuPerfIdx{3};
std::atomic<bool>         g_perfDirty{true};         // (re)apply on next frame

XrPerfSettingsLevelEXT PerfLevelEnum(int idx) {
    switch (idx) {
        case 0:  return XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
        case 1:  return XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
        case 3:  return XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
        default: return XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    }
}

// Apply the current CPU/GPU levels to the running session (a hint, so re-issued
// whenever the menu changes them). Called on the present thread.
void ApplyPerfLevels(XrInstance instance, XrSession session) {
    if (!g_hasPerfExt || session == XR_NULL_HANDLE) return;
    PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrPerfSettingsSetPerformanceLevelEXT",
                  reinterpret_cast<PFN_xrVoidFunction*>(&setLevel))) || setLevel == nullptr)
        return;
    const int ci = g_cpuPerfIdx.load(std::memory_order_relaxed);
    const int gi = g_gpuPerfIdx.load(std::memory_order_relaxed);
    const XrResult rc = setLevel(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, PerfLevelEnum(ci));
    const XrResult rg = setLevel(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, PerfLevelEnum(gi));
    LOGI("[perf] CPU=%s GPU=%s (rc=%d,%d)", kPerfNames[ci], kPerfNames[gi], static_cast<int>(rc), static_cast<int>(rg));
}

// XR_KHR_android_thread_settings — pin the heavy GameThread (RenderScene) to a big
// core by registering it as the application-main thread.
bool                  g_hasThreadExt = false;
std::atomic<uint32_t> g_gameTid{0};             // GameThread kernel TID (0 = unknown)
std::atomic<bool>     g_threadDirty{true};
std::atomic<bool>     g_sessionFocused{false};

uint32_t FindThreadTidByName(const char* wantedName) {
    if (!wantedName || !*wantedName) return 0;
    DIR* const tasks = opendir("/proc/self/task");
    if (!tasks) return 0;

    uint32_t found = 0;
    while (const dirent* const entry = readdir(tasks)) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(entry->d_name, &end, 10);
        if (end == entry->d_name || !end || *end != '\0' || parsed == 0 ||
            parsed > UINT32_MAX) {
            continue;
        }
        char path[96]{};
        std::snprintf(path, sizeof(path), "/proc/self/task/%lu/comm", parsed);
        FILE* const comm = std::fopen(path, "r");
        if (!comm) continue;
        char name[64]{};
        const bool read = std::fgets(name, sizeof(name), comm) != nullptr;
        std::fclose(comm);
        if (!read) continue;
        name[std::strcspn(name, "\r\n")] = '\0';
        if (std::strcmp(name, wantedName) == 0) {
            found = static_cast<uint32_t>(parsed);
            break;
        }
    }
    closedir(tasks);
    return found;
}

void ApplyThreadSettings(XrInstance instance, XrSession session) {
    const uint32_t tid = g_gameTid.load(std::memory_order_relaxed);
    if (!g_hasThreadExt || session == XR_NULL_HANDLE || tid == 0) return;
    PFN_xrSetAndroidApplicationThreadKHR setThread = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrSetAndroidApplicationThreadKHR",
                  reinterpret_cast<PFN_xrVoidFunction*>(&setThread))) || setThread == nullptr)
        return;
    const XrResult appResult = setThread(
        session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, tid);
    LOGI("[perf] app-main thread tid=%u -> big core (rc=%d)", tid,
         static_cast<int>(appResult));

    // GTA SA records RenderWare commands on GameThread but replays every command
    // and calls the GLES driver on a separate engine thread named RenderQueue.
    // Stereo doubles that stream. Registering only GameThread leaves the consumer
    // eligible for a little core; then the 1 MiB linear queue fills and blocks the
    // producer even while GPU time is below budget.
    const uint32_t rendererTid = FindThreadTidByName("RenderQueue");
    if (rendererTid != 0) {
        const XrResult rendererResult = setThread(
            session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, rendererTid);
        LOGI("[perf] renderer-main thread tid=%u name=RenderQueue -> big core "
             "(rc=%d)", rendererTid, static_cast<int>(rendererResult));
    } else {
        LOGW("[perf] renderer-main thread name=RenderQueue not found; retrying");
        g_threadDirty.store(true, std::memory_order_relaxed);
    }
}

// Colour: the game renders an already-sRGB-encoded frame. An RGBA8 (linear)
// swapchain makes the compositor gamma-encode it a SECOND time -> washed out. We
// present into an sRGB swapchain and blit RAW (no conversion) so the compositor's
// sRGB decode + display encode round-trips the game's pixels unchanged.
int64_t g_swapchainFormat  = GL_RGBA8;
bool    g_swapchainSrgb    = false;
bool    g_srgbWriteControl = false;   // GL_EXT_sRGB_write_control present

// On-screen FPS: the GameThread ticks g_gameFrames once per rendered frame; the
// present thread turns that into a rate and draws it as 7-segment digits.
std::atomic<uint32_t> g_gameFrames{0};
std::atomic<int>      g_fpsValue{0};
std::atomic<int>      g_profRecMs{0}, g_profScene{0}, g_profSky{0}, g_profEnd{0}, g_profEnt{0}, g_profDynamic{0};
std::atomic<int>      g_profFrameMs{0};          // whole game-frame work (ms)
std::atomic<bool>     g_menuVisible{false};      // cheat menu shown
std::atomic<int>      g_menuSelection{0}, g_menuCount{0};
std::atomic<int>      g_menuCategory{-1};
std::atomic<bool>     g_calibActive{false};      // calibration page shown
std::atomic<int>      g_calibSel{0};             // highlighted row 0..20
std::atomic<int>      g_calibHand{1};            // EDIT HAND: 0=LEFT, 1=RIGHT
std::atomic<int>      g_calibWeaponType{0};       // latched on page entry
std::atomic<bool>     g_mainMenuActive{false};   // main VR menu (root list) shown
std::atomic<int>      g_mainSel{0};
std::atomic<bool>     g_holsterMenuActive{false}; // Vice City-style holster loadout
std::atomic<int>      g_holsterMenuSel{0};
std::atomic<bool>     g_holsterCalibActive{false}; // per-weapon holstered model pose
std::atomic<int>      g_holsterCalibSel{0};
std::atomic<bool>     g_drivingActive{false};    // driving-seat calibration submenu
std::atomic<int>      g_drivingSel{0};
std::atomic<int>      g_drivingVehicleType{driving::VEHICLE_BIKE};
std::atomic<bool>     g_drivingCalibActive{false};
std::atomic<int>      g_drivingCalibSel{0};
std::atomic<int>      g_drivingCalibHand{0};
std::atomic<bool>     g_locomotionActive{false}; // movement/turning submenu
std::atomic<int>      g_locomotionSel{0};
std::atomic<bool>     g_hudActive{false};        // classic/immersive HUD submenu
std::atomic<int>      g_hudSel{0};
std::atomic<int>      g_hudPage{0};              // 0 root, 1 crop/screen, 2 wrist
std::atomic<std::uint64_t> g_hudScanStartNs{0};  // preview-only real source sweep
std::atomic<int>      g_hudScanElement{-1};
constexpr std::uint64_t kHudScanDurationNs=12000000000ULL;
std::atomic<bool>     g_gfxActive{false};        // graphics/perf submenu shown
std::atomic<int>      g_gfxSel{0};
std::atomic<bool>     g_controlsMenuActive{false};
std::atomic<int>      g_controlsSel{0};
std::atomic<bool>     g_controlsTipsActive{false};
std::atomic<bool>     g_aboutActive{false};
std::atomic<bool>     g_aboutFirstRun{false};
std::atomic<bool>     g_gfxDistanceActive{false}; // draw-distance submenu shown
std::atomic<int>      g_gfxDistanceSel{0};
float                 g_holsterPos[8][3]{};      // holster anchors (guarded by g_headPoseMutex)
float                 g_chuteTogglePos[2][3]{};  // parachute brake toggles (g_headPoseMutex)
bool                  g_chuteToggleGrabbed[2]{};
bool                  g_chuteTogglesVisible{false};
int                   g_holsterCount = 0;
// Active weapon object-space geometry (guarded by g_headPoseMutex). Published by
// the GameThread; the present thread transforms it to the hand pose and draws it.
std::vector<float>          g_wpnObjVerts;         // xyz per vertex, object space
std::vector<unsigned short> g_wpnObjIdx;           // triangle indices into the above
constexpr int         kPanelW = 512, kPanelH = 512;   // FPS/profiler + cheat-menu panel

// The exact head pose (LOCAL space) each ring set's PIXELS were rendered at,
// carried alongside the textures. The compositor world-anchors the presented quad
// at this pose so the runtime's TimeWarp reprojects the render->scan-out head
// rotation out of it — that reprojection is what a head-locked quad cannot do and
// is the actual cure for the rotation ghosting. Guarded by g_headPoseMutex.
XrPosef g_stereoRenderPose[kStereoSets]{};
bool    g_stereoRenderPoseValid[kStereoSets] = {};
MobileColorState g_pendingMobileColor{};
MobileColorState g_stereoMobileColor[kStereoSets]{};
// Hand poses baked into each set, captured when that set's eye textures were
// published (the same GameThread sample the weapon was baked from). The present
// thread builds the hand meshes from the DISPLAYED set's snapshot, so the hands and
// the baked weapon share one pose and reproject together — the gun stays welded to
// the hand instead of swimming a frame behind it. Guarded by g_headPoseMutex.
HandPose g_stereoHandPose[kStereoSets][2]{};
HandPose g_renderedHandPose[2]{};
bool     g_renderedHandPoseValid = false;
unsigned int g_stereoWeaponHandMask[kStereoSets]{};
TwoHandVisualState g_twoHandVisualState{};
TwoHandVisualState g_stereoTwoHandVisualState[kStereoSets]{};
// Scope state is captured beside the exact eye pixels it affected. The consumer
// runs one ring slot behind, so reading the live state here would flash hands or
// a reticle for one mismatched frame at optic entry/exit.
scopeaim::VisualState g_stereoScopeState[kStereoSets]{};
// The virtual wheel and its snapped hand sockets are late GL geometry, but the
// cockpit underneath comes from the delayed eye-texture ring. Capture the wheel
// state beside those pixels so vehicle motion cannot make it swim relative to
// the baked dashboard. Guarded by g_headPoseMutex.
driving::WheelVisualState g_stereoDrivingWheelState[kStereoSets]{};
// Model id of the vehicle currently anchoring the dashboard panels (-1 when
// not in a vehicle). The HUD menu shows/edits that vehicle's own dash slot.
std::atomic<int> g_lastDashModelId{-1};

// Calibrated weapon-laser ray captured in the same ring slot as the eye pixels.
// Keeping it beside the baked hand pose prevents the late GL beam from using a
// newer controller/calibration sample than the weapon already in the image.
struct LaserRay {
    bool valid{false};
    XrVector3f origin{};
    XrVector3f direction{0.0f, 0.0f, -1.0f};
};
LaserRay g_stereoLaserRay[kStereoSets]{};
LaserRay BuildCalibratedLaserRay(const HandPose& pose, int hand,
                                 const TwoHandVisualState& twoHand);

constexpr int kMaxThrowableTrajectoryPoints = 41;
struct ThrowableTrajectory {
    int count{};
    bool hit{};
    XrVector3f points[kMaxThrowableTrajectoryPoints]{};
};
ThrowableTrajectory g_throwableTrajectory[2]{};
ThrowableTrajectory g_stereoThrowableTrajectory[kStereoSets][2]{};

// Stock SA records CBulletTrace in RenderEffects, after the two stereo
// RenderScene captures have already been closed. Keep a tiny LOCAL-space pool
// for a late GL pass that is visible in both eyes and shares their copied depth.
// Keep the streak close to the muzzle: a stock-SA random 2..5 m slice can land
// tens of metres away and is effectively invisible at headset resolution.
constexpr int kMaxBulletTracers = 16;
constexpr std::uint64_t kBulletTracerLifetimeNs = 420000000ULL;
constexpr float kBulletTracerMaxLength = 45.0f;
struct BulletTracer {
    bool valid{false};
    XrVector3f start{};
    XrVector3f end{};
    int weaponType{};
    std::uint64_t bornNs{};
    std::uint64_t expiresNs{};
};
std::mutex g_bulletTracerMutex;
BulletTracer g_bulletTracers[kMaxBulletTracers]{};
unsigned int g_nextBulletTracer = 0;

// Optional target highlight fed from the exact stock radar marker calls. It is
// deliberately short lived: missions can remove/reassign a blip without an
// explicit destruction callback, so absence of a refresh removes our visual.
constexpr int kMaxObjectiveMarkers = 16;
constexpr std::uint64_t kObjectiveMarkerLifetimeNs = 500000000ULL;
struct ObjectiveMarkerVisual {
    bool valid{false};
    std::uint64_t id{};
    XrVector3f center{};
    float radius{1.0f};
    float height{2.0f};
    unsigned char red{255}, green{255}, blue{255};
    // Stock-colour diamond_3 arrow (mission points, entrances and captured
    // script cylinders); false = the optional objective HIGHLIGHT cage.
    bool stockCone{false};
    std::uint64_t expiresNs{};
};
std::mutex g_objectiveMarkerMutex;
ObjectiveMarkerVisual g_objectiveMarkers[kMaxObjectiveMarkers]{};
unsigned int g_nextObjectiveMarker = 0;

std::uint64_t MonotonicNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The ring set the compositor should sample this frame, or -1 while too few
// frames have been produced to trail safely. seqOut receives the newest sequence.
int StereoReadSet(int& seqOut) {
    const int seq = g_stereoSeq.load(std::memory_order_acquire);
    seqOut = seq;
    if (seq < kStereoReadLag) return -1;          // warm-up: nothing safe to show yet
    return (seq - kStereoReadLag) % kStereoSets;
}

struct StereoSyncWaitResult {
    bool attempted{};
    bool rescued{};
    bool timedOut{};
    double waitMs{};
};

StereoSyncWaitResult WaitForFreshStereoPair(int lastSubmittedSequence) {
    StereoSyncWaitResult result{};
    if (lastSubmittedSequence < 0) return result;

    const int initialNewest = g_stereoSeq.load(std::memory_order_acquire);
    if (initialNewest < kStereoReadLag ||
        initialNewest - kStereoReadLag != lastSubmittedSequence) {
        return result;
    }

    // xrWaitFrame return times jitter around the independently paced game
    // producer. If we land just before its publication fence, immediately
    // sampling the ring repeats the old pair and skips the new one next frame.
    // Give only that boundary case a small bounded chance to complete. The cap
    // is deliberately below the measured 72 Hz GPU headroom and cannot turn a
    // genuinely late producer into an unbounded compositor stall.
    constexpr std::uint64_t kMaxWaitNs = 1250000ULL;
    constexpr long kSleepStepNs = 100000L;
    const std::uint64_t startNs = MonotonicNowNs();
    const std::uint64_t deadlineNs = startNs + kMaxWaitNs;
    result.attempted = true;

    do {
        timespec pause{0, kSleepStepNs};
        nanosleep(&pause, nullptr);
        const int newest = g_stereoSeq.load(std::memory_order_acquire);
        if (newest >= kStereoReadLag &&
            newest - kStereoReadLag > lastSubmittedSequence) {
            result.rescued = true;
            break;
        }
    } while (MonotonicNowNs() < deadlineNs);

    const std::uint64_t endNs = MonotonicNowNs();
    result.waitMs = static_cast<double>(endNs - startNs) / 1e6;
    result.timedOut = !result.rescued;
    return result;
}

// Rotate vector v by unit quaternion q (v' = q * v * q^-1). Used to place the
// world-anchored eye quad the render pose's forward direction in front of it.
inline XrVector3f QuatRotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f u{q.x, q.y, q.z};
    const float s = q.w;
    const float d  = u.x * v.x + u.y * v.y + u.z * v.z;   // dot(u, v)
    const float uu = u.x * u.x + u.y * u.y + u.z * u.z;   // dot(u, u)
    const XrVector3f c{u.y * v.z - u.z * v.y,             // cross(u, v)
                       u.z * v.x - u.x * v.z,
                       u.x * v.y - u.y * v.x};
    return {2.0f * d * u.x + (s * s - uu) * v.x + 2.0f * s * c.x,
            2.0f * d * u.y + (s * s - uu) * v.y + 2.0f * s * c.y,
            2.0f * d * u.z + (s * s - uu) * v.z + 2.0f * s * c.z};
}

struct Swapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    int32_t     width{};
    int32_t     height{};
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct State {
    XrInstance     instance{XR_NULL_HANDLE};
    XrSystemId     systemId{XR_NULL_SYSTEM_ID};
    XrSession      session{XR_NULL_HANDLE};
    XrSpace        space{XR_NULL_HANDLE};        // LOCAL (world-locked)
    XrSpace        viewSpace{XR_NULL_HANDLE};    // VIEW  (head-locked)
    XrSessionState sessionState{XR_SESSION_STATE_UNKNOWN};
    bool           running{false};

    std::vector<XrViewConfigurationView> configViews;
    std::vector<Swapchain>               swapchains;
    std::vector<XrView>                  views;

    // Theater screen: one flat swapchain shown as a world-locked quad. Menus,
    // loading and cutscenes are 2D, so projecting them per-eye looks wrong (the
    // "different in each eye" effect); a single quad both eyes converge on reads
    // as a comfortable cinema screen instead.
    Swapchain theater;
    bool      theaterMode{true};

    // Head-locked FPS/debug panel (toggled by both grips + A), in the spirit of
    // the Vice City VR port's overlay.
    Swapchain debug;
    bool      debugVisible{false};

    // Original mobile HUD rendered into a transparent shared texture and
    // submitted as a head-locked quad, independently of the world projection.
    Swapchain hud;

    GLuint framebuffer{0};
    GLuint eyeReadFramebuffer{0};   // consumer-side FBO for sampling shared eye textures

    // The engine's last finished frame, as an external texture, plus the layout
    // matrix SurfaceTexture reports for it.
    GLuint gameTexture{0};
    float  gameTransform[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    GLuint blitProgram{0};
    GLuint blitVertexArray{0};
    GLint  blitTextureUniform{-1};
    GLint  blitTransformUniform{-1};

    // Spatial AA for the already-rendered RenderWare eye texture. This replaces
    // the final linear copy and therefore needs no additional full-size image.
    GLuint fxaaProgram{0};
    GLuint fxaaVertexArray{0};
    GLuint fxaaSampler{0};
    GLint  fxaaTextureUniform{-1};
    GLint  fxaaInverseSizeUniform{-1};
    GLint  fxaaUnderwaterUniform{-1};
    GLint  fxaaWaterDepthUniform{-1};
    GLint  fxaaMobileColorModeUniform{-1};
    GLint  fxaaContrastMultUniform{-1};
    GLint  fxaaContrastAddUniform{-1};
    GLint  fxaaRedGradeUniform{-1};
    GLint  fxaaGreenGradeUniform{-1};
    GLint  fxaaBlueGradeUniform{-1};

    // When FXAA is disabled, underwater grading replaces the normal linear
    // blit with this one-sample copy. It is created lazily and used only while
    // the game camera is submerged.
    GLuint underwaterProgram{0};
    GLuint underwaterVertexArray{0};
    GLuint underwaterSampler{0};
    GLint  underwaterTextureUniform{-1};
    GLint  underwaterAmountUniform{-1};
    GLint  underwaterDepthUniform{-1};
    GLint  underwaterMobileColorModeUniform{-1};
    GLint  underwaterContrastMultUniform{-1};
    GLint  underwaterContrastAddUniform{-1};
    GLint  underwaterRedGradeUniform{-1};
    GLint  underwaterGreenGradeUniform{-1};
    GLint  underwaterBlueGradeUniform{-1};

    // Full classic HUD is alpha-keyed into each finished eye. Unlike the old
    // three OpenXR crop cards this has no opaque quad background or guessed
    // radar/message bounds.
    GLuint hudCompositeProgram{0};
    GLuint hudCompositeVertexArray{0};
    GLuint hudCompositeSampler{0};
    GLint  hudCompositeTextureUniform{-1};
    GLint  hudCompositeSourceRectUniform{-1};
    GLint  hudCompositeDestinationRectUniform{-1};
    GLint  hudCompositeHighlightUniform{-1};

    // Controller input.
    XrActionSet actionSet{XR_NULL_HANDLE};
    XrAction    stickAction{XR_NULL_HANDLE};
    XrAction    triggerAction{XR_NULL_HANDLE};
    XrAction    gripAction{XR_NULL_HANDLE};
    XrAction    aAction{XR_NULL_HANDLE};
    XrAction    bAction{XR_NULL_HANDLE};
    XrAction    xAction{XR_NULL_HANDLE};
    XrAction    yAction{XR_NULL_HANDLE};
    XrAction    menuAction{XR_NULL_HANDLE};
    XrAction    stickClickAction{XR_NULL_HANDLE}; // L3/R3, per-hand subactions
    XrAction    aimAction{XR_NULL_HANDLE};
    XrAction    gripPoseAction{XR_NULL_HANDLE};
    XrSpace     aimSpace{XR_NULL_HANDLE};        // right-hand aim, for the laser pointer
    XrSpace     handGripSpace[2]{};              // per-hand grip pose (VR hands)
    XrSpace     handAimSpace[2]{};               // per-hand aim pose
    XrPath      handPaths[2]{};
    InputState  input{};
};

State s;

const char* XrName(XrResult r) {
    static char buffer[XR_MAX_RESULT_STRING_SIZE];
    if (s.instance != XR_NULL_HANDLE && xrResultToString(s.instance, r, buffer) == XR_SUCCESS) {
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "XrResult %d", static_cast<int>(r));
    return buffer;
}

// Every OpenXR call goes through this. A silent failure in setup shows up much
// later as a black headset, which is the single most expensive way to debug VR.
//
// The rate limit is not politeness. A failing per-frame call in a loop that is
// not being throttled by the compositor produces tens of thousands of lines a
// second, which rolls the whole logcat buffer and destroys the evidence needed
// to find out what failed in the first place.
bool Check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }

    static unsigned long long failures = 0;
    if (++failures <= 8 || failures % 5000 == 0) {
        LOGE("%s failed: %s (failure %llu)", what, XrName(result), failures);
    }
    return false;
}

template <typename Fn>
bool GetProc(const char* name, Fn& out) {
    auto result = xrGetInstanceProcAddr(s.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out));
    return Check(result, name);
}

void ResolveDiagnosticExtensions() {
    if (g_hasDisplayRefreshExt) {
        const bool ok = GetProc("xrEnumerateDisplayRefreshRatesFB",
                                g_enumerateDisplayRefreshRates) &&
                        GetProc("xrGetDisplayRefreshRateFB", g_getDisplayRefreshRate) &&
                        GetProc("xrRequestDisplayRefreshRateFB",
                                g_requestDisplayRefreshRate) &&
                        g_enumerateDisplayRefreshRates && g_getDisplayRefreshRate &&
                        g_requestDisplayRefreshRate;
        if (!ok) g_hasDisplayRefreshExt = false;
    }

    if (g_hasPerformanceMetricsExt) {
        const bool ok = GetProc("xrEnumeratePerformanceMetricsCounterPathsMETA",
                                g_enumerateMetricPaths) &&
                        GetProc("xrSetPerformanceMetricsStateMETA", g_setMetricsState) &&
                        GetProc("xrQueryPerformanceMetricsCounterMETA", g_queryMetric) &&
                        g_enumerateMetricPaths && g_setMetricsState && g_queryMetric;
        if (!ok) {
            g_hasPerformanceMetricsExt = false;
        } else {
            uint32_t count = 0;
            if (XR_SUCCEEDED(g_enumerateMetricPaths(s.instance, 0, &count, nullptr)) &&
                count > 0) {
                std::vector<XrPath> paths(count);
                if (XR_SUCCEEDED(g_enumerateMetricPaths(
                        s.instance, count, &count, paths.data()))) {
                    XrPath cpuCandidate = XR_NULL_PATH;
                    XrPath gpuCandidate = XR_NULL_PATH;
                    xrStringToPath(s.instance, "/perfmetrics_meta/app/cpu_frametime",
                                   &cpuCandidate);
                    xrStringToPath(s.instance, "/perfmetrics_meta/app/gpu_frametime",
                                   &gpuCandidate);
                    for (XrPath path : paths) {
                        if (path == cpuCandidate) g_appCpuFrameTimePath = path;
                        if (path == gpuCandidate) g_appGpuFrameTimePath = path;
                    }
                }
            }
        }
    }

    LOGI("[perf.init] xr diagnostics refresh=%d meta_metrics=%d cpu_path=%d gpu_path=%d",
         g_hasDisplayRefreshExt ? 1 : 0,
         g_hasPerformanceMetricsExt ? 1 : 0,
         g_appCpuFrameTimePath != XR_NULL_PATH ? 1 : 0,
         g_appGpuFrameTimePath != XR_NULL_PATH ? 1 : 0);
}

void InitializeSessionDiagnostics() {
    if (g_hasDisplayRefreshExt && g_enumerateDisplayRefreshRates &&
        g_getDisplayRefreshRate && g_requestDisplayRefreshRate) {
        uint32_t count = 0;
        g_displayRefreshRequestSupported = false;
        g_requestedDisplayRefreshHz = static_cast<float>(frame_target::kFps);
        const XrResult countResult = g_enumerateDisplayRefreshRates(
            s.session, 0, &count, nullptr);
        if (XR_SUCCEEDED(countResult) && count > 0) {
            std::vector<float> rates(count);
            if (XR_SUCCEEDED(g_enumerateDisplayRefreshRates(
                    s.session, count, &count, rates.data()))) {
                char offered[160]{};
                std::size_t used = 0;
                float closestDistance = 10000.0f;
                for (uint32_t i = 0; i < count && used + 12 < sizeof(offered); ++i) {
                    const float distance = std::fabs(
                        rates[i] - static_cast<float>(frame_target::kFps));
                    if (std::isfinite(rates[i]) && rates[i] > 0.0f &&
                        distance < closestDistance) {
                        closestDistance = distance;
                        g_requestedDisplayRefreshHz = rates[i];
                    }
                    const int wrote = std::snprintf(
                        offered + used, sizeof(offered) - used,
                        "%s%.1f", i ? "/" : "", rates[i]);
                    if (wrote <= 0) break;
                    used += static_cast<std::size_t>(wrote);
                }
                g_displayRefreshRequestSupported = closestDistance < 10000.0f;
                LOGI("[perf.init] display rates offered=%s", offered);
            }
        } else {
            // This fallback mirrors the mature Vice City path. Some Horizon
            // builds expose the extension but briefly return an empty list;
            // a direct request remains safe and reports UNSUPPORTED cleanly.
            g_displayRefreshRequestSupported = true;
            LOGW("[perf.init] display rate list unavailable count=%u rc=%d; "
                 "using direct %.1fHz request",
                 count, static_cast<int>(countResult),
                 g_requestedDisplayRefreshHz);
        }
        if (g_displayRefreshRequestSupported &&
            std::fabs(g_requestedDisplayRefreshHz -
                      static_cast<float>(frame_target::kFps)) >= 0.5f) {
            LOGW("[perf.init] exact target %dHz unavailable; closest is %.1fHz",
                 frame_target::kFps, g_requestedDisplayRefreshHz);
        }
        float candidate = 0.0f;
        const XrResult result = g_getDisplayRefreshRate(s.session, &candidate);
        if (XR_SUCCEEDED(result) && std::isfinite(candidate) && candidate > 0.0f) {
            g_displayRefreshHz = candidate;
            g_displayRefreshValid = true;
        }
        LOGI("[perf.init] display target=%dHz selected=%.1fHz requestable=%d "
             "current=%.1fHz get_rc=%d",
             frame_target::kFps, g_requestedDisplayRefreshHz,
             g_displayRefreshRequestSupported ? 1 : 0,
             g_displayRefreshValid ? g_displayRefreshHz : -1.0f,
             static_cast<int>(result));
    }
}

XrResult RequestTargetDisplayRefresh(const char* phase) {
    if (!g_hasDisplayRefreshExt || !g_requestDisplayRefreshRate ||
        !g_displayRefreshRequestSupported || s.session == XR_NULL_HANDLE) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    const XrResult requestResult = g_requestDisplayRefreshRate(
        s.session, g_requestedDisplayRefreshHz);
    float current = 0.0f;
    const XrResult getResult = g_getDisplayRefreshRate
        ? g_getDisplayRefreshRate(s.session, &current)
        : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_SUCCEEDED(getResult) && std::isfinite(current) && current > 0.0f) {
        g_displayRefreshHz = current;
        g_displayRefreshValid = true;
    }
    LOGI("[perf.refresh] phase=%s target=%dHz selected=%.1fHz request_rc=%d "
         "current=%.1fHz get_rc=%d retry=%d/8",
         phase ? phase : "?", frame_target::kFps,
         g_requestedDisplayRefreshHz,
         static_cast<int>(requestResult),
         g_displayRefreshValid ? g_displayRefreshHz : -1.0f,
         static_cast<int>(getResult), g_displayRefreshRetryCount);
    return requestResult;
}

void EnablePerformanceMetrics() {
    if (!g_hasPerformanceMetricsExt || !g_setMetricsState || g_metricsEnabled ||
        s.session == XR_NULL_HANDLE) return;
    XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
    state.enabled = XR_TRUE;
    const XrResult result = g_setMetricsState(s.session, &state);
    g_metricsEnabled = XR_SUCCEEDED(result);
    LOGI("[perf.init] XR_META performance metrics enabled=%d rc=%d",
         g_metricsEnabled ? 1 : 0, static_cast<int>(result));
}

void QueryRuntimeDiagnostics() {
    if (g_hasDisplayRefreshExt && g_getDisplayRefreshRate) {
        float candidate = 0.0f;
        const XrResult result = g_getDisplayRefreshRate(s.session, &candidate);
        if (XR_SUCCEEDED(result) && std::isfinite(candidate) && candidate > 0.0f) {
            g_displayRefreshHz = candidate;
            g_displayRefreshValid = true;
        } else {
            g_displayRefreshValid = false;
        }
    }

    // Quest can acknowledge a request before the panel actually switches.
    // Re-check once per telemetry tick and retry for a bounded eight seconds,
    // matching the headset-validated Vice City implementation.
    if (s.sessionState == XR_SESSION_STATE_FOCUSED &&
        g_displayRefreshRequestSupported &&
        g_displayRefreshRetryCount < 8) {
        const bool confirmed = g_displayRefreshValid &&
            std::fabs(g_displayRefreshHz - g_requestedDisplayRefreshHz) < 0.5f;
        if (confirmed) {
            LOGI("[perf.refresh] confirmed %.1fHz after %d request(s)",
                 g_displayRefreshHz, g_displayRefreshRetryCount);
            g_displayRefreshRetryCount = 8;
        } else {
            RequestTargetDisplayRefresh("retry");
            ++g_displayRefreshRetryCount;
        }
    }

    g_runtimeCpuValid = false;
    g_runtimeGpuValid = false;
    if (!g_metricsEnabled || !g_queryMetric) return;

    auto queryFrameTime = [](XrPath path, float& value, bool& valid) {
        if (path == XR_NULL_PATH) return;
        XrPerformanceMetricsCounterMETA counter{
            XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
        if (XR_FAILED(g_queryMetric(s.session, path, &counter)) ||
            counter.counterUnit !=
                XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META) return;
        if ((counter.counterFlags &
             XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) != 0) {
            if (std::isfinite(counter.floatValue) && counter.floatValue >= 0.0f) {
                value = counter.floatValue;
                valid = true;
            }
        } else if ((counter.counterFlags &
                    XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) != 0) {
            value = static_cast<float>(counter.uintValue);
            valid = true;
        }
    };
    queryFrameTime(g_appCpuFrameTimePath, g_runtimeCpuMs, g_runtimeCpuValid);
    queryFrameTime(g_appGpuFrameTimePath, g_runtimeGpuMs, g_runtimeGpuValid);
}

// The runtime binds to one specific EGL context, and the config has to be the
// one that context was created with. Rather than guessing a config we ask the
// context which id it holds and look that exact one back up.
EGLConfig ConfigOfContext(EGLDisplay display, EGLContext context) {
    EGLint configId = 0;
    if (eglQueryContext(display, context, EGL_CONFIG_ID, &configId) != EGL_TRUE) {
        LOGE("eglQueryContext(EGL_CONFIG_ID) failed: 0x%x", eglGetError());
        return nullptr;
    }

    const EGLint attribs[] = {EGL_CONFIG_ID, configId, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint    count  = 0;
    if (eglChooseConfig(display, attribs, &config, 1, &count) != EGL_TRUE || count != 1) {
        LOGE("no EGLConfig with id %d", configId);
        return nullptr;
    }
    return config;
}

void PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            s.sessionState = changed.state;
            g_sessionFocused.store(s.sessionState == XR_SESSION_STATE_FOCUSED,
                                   std::memory_order_release);
            LOGI("session state -> %d", static_cast<int>(s.sessionState));

            if (s.sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(s.session, &begin), "xrBeginSession")) {
                    s.running = true;
                    LOGI("session running");
                    g_displayRefreshRetryCount = 0;
                    RequestTargetDisplayRefresh("begin");
                    g_displayRefreshRetryCount = 1;
                    g_resetPresentTiming = true;
                    perf::ResetPresentTelemetry();
                    EnablePerformanceMetrics();
                    g_perfDirty.store(true, std::memory_order_relaxed);    // apply perf + thread
                    g_threadDirty.store(true, std::memory_order_relaxed);  // hints next frame
                }
            } else if (s.sessionState == XR_SESSION_STATE_FOCUSED) {
                // Some runtimes restore the headset's global refresh rate while
                // transitioning into the running/focused state. Reassert once
                // at focus so the session target cannot silently fall back.
                RequestTargetDisplayRefresh("focused");
                if (g_displayRefreshRetryCount < 2)
                    g_displayRefreshRetryCount = 2;
            } else if (s.sessionState == XR_SESSION_STATE_STOPPING) {
                s.running = false;
                g_runtimeCpuValid = false;
                g_runtimeGpuValid = false;
                g_displayRefreshValid = false;
                g_resetPresentTiming = true;
                Check(xrEndSession(s.session), "xrEndSession");
            } else if (s.sessionState == XR_SESSION_STATE_EXITING ||
                       s.sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                s.running = false;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
            const auto& changed =
                *reinterpret_cast<XrEventDataPerfSettingsEXT*>(&event);
            const bool warning =
                changed.toLevel != XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT;
            if (warning) {
                LOGW("[perf.event] domain=%d subdomain=%d level=%d->%d",
                     static_cast<int>(changed.domain),
                     static_cast<int>(changed.subDomain),
                     static_cast<int>(changed.fromLevel),
                     static_cast<int>(changed.toLevel));
            } else {
                LOGI("[perf.event] domain=%d subdomain=%d level=%d->%d",
                     static_cast<int>(changed.domain),
                     static_cast<int>(changed.subDomain),
                     static_cast<int>(changed.fromLevel),
                     static_cast<int>(changed.toLevel));
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& changed =
                *reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&event);
            g_displayRefreshHz = changed.toDisplayRefreshRate;
            g_displayRefreshValid = std::isfinite(changed.toDisplayRefreshRate) &&
                                    changed.toDisplayRefreshRate > 0.0f;
            LOGI("[perf.event] display %.1fHz->%.1fHz",
                 changed.fromDisplayRefreshRate, changed.toDisplayRefreshRate);
            if (g_displayRefreshValid &&
                std::fabs(g_displayRefreshHz - g_requestedDisplayRefreshHz) < 0.5f) {
                g_displayRefreshRetryCount = 8;
            } else {
                g_displayRefreshRetryCount = 0;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOGW("instance loss pending");
            s.running = false;
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Split out of swapchain creation because the eye size is needed earlier than
// the session: the game is handed a surface of that size the moment it asks for
// one, which is before any session can exist.
bool EnumerateViews() {
    uint32_t viewCount = 0;
    if (!Check(xrEnumerateViewConfigurationViews(s.instance, s.systemId,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 0, &viewCount, nullptr),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }

    s.configViews.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!Check(xrEnumerateViewConfigurationViews(s.instance, s.systemId,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 viewCount, &viewCount, s.configViews.data()),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    s.views.assign(viewCount, {XR_TYPE_VIEW});
    return true;
}

bool CreateSwapchains() {
    // Pick an sRGB swapchain format if the runtime offers one AND the driver lets
    // us disable sRGB write conversion (so our blit copies the game's already-
    // encoded pixels raw). Without write control an sRGB target would re-encode on
    // blit, so fall back to RGBA8 rather than make it worse.
    {
        const GLubyte* exts = glGetString(GL_EXTENSIONS);
        const char* const glExts = reinterpret_cast<const char*>(exts);
        const auto hasGlExt = [&](const char* name) {
            return glExts && std::strstr(glExts, name) != nullptr;
        };
        g_srgbWriteControl = hasGlExt("GL_EXT_sRGB_write_control");
        LOGI("[gpu.caps] gl qcom_foveated=%d qcom_subsampled=%d "
             "qcom_shading_rate=%d ext_density_map=%d ovr_multiview2=%d",
             hasGlExt("GL_QCOM_texture_foveated") ? 1 : 0,
             hasGlExt("GL_QCOM_texture_foveated_subsampled_layout") ? 1 : 0,
             hasGlExt("GL_QCOM_shading_rate") ? 1 : 0,
             hasGlExt("GL_EXT_fragment_density_map") ? 1 : 0,
             hasGlExt("GL_OVR_multiview2") ? 1 : 0);
        uint32_t fc = 0;
        xrEnumerateSwapchainFormats(s.session, 0, &fc, nullptr);
        std::vector<int64_t> formats(fc ? fc : 1);
        if (fc) xrEnumerateSwapchainFormats(s.session, fc, &fc, formats.data());
        bool offersSrgb = false;
        for (uint32_t k = 0; k < fc; ++k) if (formats[k] == GL_SRGB8_ALPHA8) { offersSrgb = true; break; }
        g_swapchainSrgb   = offersSrgb && g_srgbWriteControl;
        g_swapchainFormat = g_swapchainSrgb ? static_cast<int64_t>(GL_SRGB8_ALPHA8)
                                            : static_cast<int64_t>(GL_RGBA8);
        LOGI("[stereo] swapchain format=0x%llx sRGB=%d (runtime offers sRGB=%d, writeControl=%d)",
             static_cast<unsigned long long>(g_swapchainFormat), g_swapchainSrgb,
             offersSrgb, g_srgbWriteControl);
    }

    const auto viewCount = static_cast<uint32_t>(s.configViews.size());
    s.swapchains.resize(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        const XrViewConfigurationView& view = s.configViews[i];

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format      = g_swapchainFormat;
        info.sampleCount = 1;
        info.width       = static_cast<int32_t>(view.recommendedImageRectWidth);
        info.height      = static_cast<int32_t>(view.recommendedImageRectHeight);
        info.faceCount   = 1;
        info.arraySize   = 1;
        info.mipCount    = 1;

        Swapchain& chain = s.swapchains[i];
        chain.width  = info.width;
        chain.height = info.height;
        if (!Check(xrCreateSwapchain(s.session, &info, &chain.handle), "xrCreateSwapchain")) {
            return false;
        }

        uint32_t imageCount = 0;
        if (!Check(xrEnumerateSwapchainImages(chain.handle, 0, &imageCount, nullptr),
                   "xrEnumerateSwapchainImages")) {
            return false;
        }
        chain.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                       chain.handle, imageCount, &imageCount,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data())),
                   "xrEnumerateSwapchainImages")) {
            return false;
        }

        LOGI("eye %u swapchain %dx%d, %u images", i, chain.width, chain.height, imageCount);
    }

    // The theater screen matches the size the engine actually renders into (the
    // per-eye recommended size = the game surface), so the game frame blits 1:1
    // with no stretching. A mismatched landscape size distorted the picture.
    {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format      = g_swapchainFormat;
        info.sampleCount = 1;
        info.width       = static_cast<int32_t>(s.configViews[0].recommendedImageRectWidth);
        info.height      = static_cast<int32_t>(s.configViews[0].recommendedImageRectHeight);
        info.faceCount   = 1;
        info.arraySize   = 1;
        info.mipCount    = 1;

        s.theater.width  = info.width;
        s.theater.height = info.height;
        if (!Check(xrCreateSwapchain(s.session, &info, &s.theater.handle), "xrCreateSwapchain(theater)")) {
            return false;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(s.theater.handle, 0, &imageCount, nullptr);
        s.theater.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                       s.theater.handle, imageCount, &imageCount,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(s.theater.images.data())),
                   "xrEnumerateSwapchainImages(theater)")) {
            return false;
        }
        LOGI("theater swapchain %dx%d, %u images", s.theater.width, s.theater.height, imageCount);
    }

    // Debug/FPS panel swapchain (fixed 512x128, uploaded from CPU pixels).
    {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format      = g_swapchainFormat;
        info.sampleCount = 1;
        info.width       = kPanelW;
        info.height      = kPanelH;
        info.faceCount   = 1;
        info.arraySize   = 1;
        info.mipCount    = 1;
        s.debug.width  = info.width;
        s.debug.height = info.height;
        if (!Check(xrCreateSwapchain(s.session, &info, &s.debug.handle), "xrCreateSwapchain(debug)")) {
            return false;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(s.debug.handle, 0, &imageCount, nullptr);
        s.debug.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(s.debug.handle, imageCount, &imageCount,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(s.debug.images.data()));
        LOGI("debug swapchain %dx%d, %u images", s.debug.width, s.debug.height, imageCount);
    }

    // Dedicated gameplay HUD quad. Unlike the settings/debug pixels, its source
    // is a shared RenderWare camera-texture produced by the game thread.
    {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                           XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format      = g_swapchainFormat;
        info.sampleCount = 1;
        // Legacy quad-layer path (PresentGameplayHud) is dead; keep its
        // swapchain at the logical size so the 2x physical raster does not
        // grow an unused allocation.
        info.width       = kGameplayHudLogicalWidth;
        info.height      = kGameplayHudLogicalHeight;
        info.faceCount   = 1;
        info.arraySize   = 1;
        info.mipCount    = 1;
        s.hud.width=info.width; s.hud.height=info.height;
        if (!Check(xrCreateSwapchain(s.session,&info,&s.hud.handle),
                   "xrCreateSwapchain(hud)")) return false;
        uint32_t imageCount=0;
        xrEnumerateSwapchainImages(s.hud.handle,0,&imageCount,nullptr);
        s.hud.images.assign(imageCount,{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                       s.hud.handle,imageCount,&imageCount,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(
                           s.hud.images.data())),
                   "xrEnumerateSwapchainImages(hud)")) return false;
        LOGI("hud swapchain %dx%d, %u images",s.hud.width,s.hud.height,
             imageCount);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Blitting the game's frame into the eye swapchains
//
// The engine renders into a SurfaceTexture, so its finished frame is already an
// external GL texture in the same context. Getting it in front of the player is
// therefore one textured triangle per eye and no copy at all.
// ---------------------------------------------------------------------------

constexpr GLenum kTextureExternalOES = 0x8D65;

// A single oversized triangle rather than a quad: no vertex buffer, no index
// buffer, and one less edge for the rasteriser to walk.
const char* kVertexShader = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vUv;
uniform samplerExternalOES uTexture;
uniform mat4 uTransform;
out vec4 outColor;
void main() {
    outColor = texture(uTexture, (uTransform * vec4(vUv, 0.0, 1.0)).xy);
}
)";

constexpr char kFxaaProperty[] = "debug.savr.fxaa";

// The Quest Vice City port's stable single-frame FXAA, translated to GLES.
// Low-contrast pixels stop after five samples; edge pixels use nine. Sampling
// is clamped to half a source texel so the filter cannot bleed across an eye.
const char* kFxaaFragmentShader = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTexture;
uniform vec2 uInverseSourceSize;
uniform float uUnderwater;
uniform float uWaterDepth;
uniform int uMobileColorMode;
uniform vec3 uContrastMult;
uniform vec3 uContrastAdd;
uniform vec4 uRedGrade;
uniform vec4 uGreenGrade;
uniform vec4 uBlueGrade;
out vec4 outColor;

float Luma(vec3 colour) {
    return dot(colour, vec3(0.299, 0.587, 0.114));
}

vec3 SampleColour(vec2 uv) {
    const vec2 one = vec2(1.0);
    const vec2 halfPixelScale = vec2(0.5);
    vec2 halfPixel = uInverseSourceSize * halfPixelScale;
    return texture(uTexture, clamp(uv, halfPixel, one - halfPixel)).rgb;
}

vec3 GradeUnderwater(vec3 colour) {
    float amount = smoothstep(0.12, 0.50, clamp(uUnderwater, 0.0, 1.0));
    float depth = clamp(uWaterDepth * 0.08, 0.0, 0.65);
    vec3 tinted = colour * vec3(0.40, 0.66, 0.74) +
                  vec3(0.008, 0.042, 0.070);
    vec3 deep = vec3(0.012, 0.10, 0.16);
    vec3 water = mix(tinted, deep, depth);
    return mix(colour, clamp(water, 0.0, 1.0), amount);
}

vec3 GradeMobileOriginal(vec3 colour) {
    if (uMobileColorMode == 1)
        return colour * uContrastMult + uContrastAdd;
    if (uMobileColorMode == 2) {
        vec4 sampleColour = vec4(colour, 1.0);
        return vec3(dot(sampleColour, uRedGrade),
                    dot(sampleColour, uGreenGrade),
                    dot(sampleColour, uBlueGrade));
    }
    return colour;
}

vec3 GradeOutput(vec3 colour) {
    // Retail MobileRender first lands in a normalized render target. Preserve
    // that saturation point before our compositor-only underwater blend;
    // otherwise highlights above 1.0 would be over-weighted under water.
    return GradeUnderwater(clamp(GradeMobileOriginal(colour), 0.0, 1.0));
}

void main() {
    vec3 middle = SampleColour(vUv);
    vec3 nw = SampleColour(vUv + vec2(-1.0, -1.0) * uInverseSourceSize);
    vec3 ne = SampleColour(vUv + vec2( 1.0, -1.0) * uInverseSourceSize);
    vec3 sw = SampleColour(vUv + vec2(-1.0,  1.0) * uInverseSourceSize);
    vec3 se = SampleColour(vUv + vec2( 1.0,  1.0) * uInverseSourceSize);
    float lm = Luma(middle);
    float lnw = Luma(nw);
    float lne = Luma(ne);
    float lsw = Luma(sw);
    float lse = Luma(se);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));

    if (lmax - lmin < max(0.0312, lmax * 0.125)) {
        outColor = vec4(GradeOutput(middle), 1.0);
        return;
    }

    vec2 direction = vec2(-((lnw + lne) - (lsw + lse)),
                           ((lnw + lsw) - (lne + lse)));
    float reduce = max((lnw + lne + lsw + lse) * 0.03125, 0.0078125);
    direction = clamp(direction /
                      (min(abs(direction.x), abs(direction.y)) + reduce),
                      vec2(-8.0), vec2(8.0)) * uInverseSourceSize;
    vec3 a = 0.5 * (
        SampleColour(vUv + direction * (1.0 / 3.0 - 0.5)) +
        SampleColour(vUv + direction * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (
        SampleColour(vUv + direction * -0.5) +
        SampleColour(vUv + direction *  0.5));
    float lb = Luma(b);
    outColor = vec4(GradeOutput((lb < lmin || lb > lmax) ? a : b), 1.0);
}
)";

const char* kUnderwaterCopyFragmentShader = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTexture;
uniform float uUnderwater;
uniform float uWaterDepth;
uniform int uMobileColorMode;
uniform vec3 uContrastMult;
uniform vec3 uContrastAdd;
uniform vec4 uRedGrade;
uniform vec4 uGreenGrade;
uniform vec4 uBlueGrade;
out vec4 outColor;

vec3 GradeMobileOriginal(vec3 colour) {
    if (uMobileColorMode == 1)
        return colour * uContrastMult + uContrastAdd;
    if (uMobileColorMode == 2) {
        vec4 sampleColour = vec4(colour, 1.0);
        return vec3(dot(sampleColour, uRedGrade),
                    dot(sampleColour, uGreenGrade),
                    dot(sampleColour, uBlueGrade));
    }
    return colour;
}

void main() {
    // Match the normalized intermediate target used by retail before applying
    // the compositor-only underwater tint.
    vec3 colour = clamp(GradeMobileOriginal(texture(uTexture, vUv).rgb),
                        0.0, 1.0);
    float amount = smoothstep(0.12, 0.50, clamp(uUnderwater, 0.0, 1.0));
    float depth = clamp(uWaterDepth * 0.08, 0.0, 0.65);
    vec3 tinted = colour * vec3(0.40, 0.66, 0.74) +
                  vec3(0.008, 0.042, 0.070);
    vec3 deep = vec3(0.012, 0.10, 0.16);
    vec3 water = mix(tinted, deep, depth);
    outColor = vec4(mix(colour, clamp(water, 0.0, 1.0), amount), 1.0);
}
)";

const char* kHudCompositeFragmentShader = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTexture;
uniform vec4 uSourceRect;
uniform vec4 uDestinationRect;
uniform float uHighlight;
out vec4 outColor;

void main() {
    // Destination and calibration coordinates use a top-left origin, matching
    // the values shown in the headset menu.
    vec2 screenUv = vec2(vUv.x, 1.0 - vUv.y);
    vec2 local = (screenUv - uDestinationRect.xy) / uDestinationRect.zw;
    if (local.x < 0.0 || local.y < 0.0 || local.x > 1.0 || local.y > 1.0)
        discard;
    // The captured HUD texture has already passed through the RenderWare-to-GL
    // framebuffer path and arrives top-down here. Flipping it a second time was
    // why tutorial and money text appeared upside-down in the headset.
    vec2 sourceUv = uSourceRect.xy + local * uSourceRect.zw;
    vec4 colour = texture(uTexture, sourceUv);
    float coverage = max(colour.r, max(colour.g, colour.b));
    float keyedAlpha = smoothstep(0.008, 0.055, coverage);
    float edge = min(min(local.x, 1.0 - local.x),
                     min(local.y, 1.0 - local.y));
    if (uHighlight > 2.5) {
        if (edge < 0.018) {
            outColor = vec4(0.0,0.85,1.0,0.95);
            return;
        }
        discard;
    }
    if (uHighlight > 0.5 && edge < 0.018) {
        outColor = vec4(0.0, 0.85, 1.0, 0.95);
        return;
    }
    // Value 2 is an outline-only pass. It is emitted after every HUD element,
    // so no later crop can cover the calibration rectangle.
    if (uHighlight > 1.5)
        discard;
    outColor = vec4(colour.rgb, colour.a * keyedAlpha);
}
)";

bool FxaaRequested() {
    static const bool requested = [] {
        char text[PROP_VALUE_MAX]{};
        bool enabled = true;
        if (__system_property_get(kFxaaProperty, text) > 0) {
            if (std::strcmp(text, "0") == 0) enabled = false;
            else if (std::strcmp(text, "1") != 0)
                LOGW("[fxaa] ignoring invalid %s=%s (valid 0 or 1)",
                     kFxaaProperty, text);
        }
        LOGI("[fxaa] requested=%d property=%s", enabled ? 1 : 0,
             kFxaaProperty);
        return enabled;
    }();
    return requested;
}

bool g_fxaaBuildAttempted = false;
bool g_fxaaRuntimeFailed = false;
bool g_fxaaFirstDrawValidated = false;
int  g_fxaaErrorCount = 0;
bool g_underwaterBuildAttempted = false;
bool g_underwaterRuntimeFailed = false;
bool g_underwaterFirstDrawValidated = false;

struct FxaaFrameStats {
    bool requested{};
    bool active{};
    int draws{};
    int fallbacks{};
    int errors{};
    double submitWallMs{};
};

struct FxaaGlState {
    GLint program{};
    GLint vertexArray{};
    GLint activeTexture{};
    GLint texture0{};
    GLint sampler0{};
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
};

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool BuildBlitProgram() {
    if (s.blitProgram != 0) {
        return true;
    }

    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex == 0 || fragment == 0) {
        return false;
    }

    s.blitProgram = glCreateProgram();
    glAttachShader(s.blitProgram, vertex);
    glAttachShader(s.blitProgram, fragment);
    glLinkProgram(s.blitProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = 0;
    glGetProgramiv(s.blitProgram, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        char log[1024]{};
        glGetProgramInfoLog(s.blitProgram, sizeof(log), nullptr, log);
        LOGE("blit program link failed: %s", log);
        glDeleteProgram(s.blitProgram);
        s.blitProgram = 0;
        return false;
    }

    s.blitTextureUniform   = glGetUniformLocation(s.blitProgram, "uTexture");
    s.blitTransformUniform = glGetUniformLocation(s.blitProgram, "uTransform");
    glGenVertexArrays(1, &s.blitVertexArray);
    LOGI("blit program ready");
    return true;
}

bool BuildFxaaProgram() {
    if (!FxaaRequested()) return false;
    if (s.fxaaProgram != 0 && !g_fxaaRuntimeFailed) return true;
    if (g_fxaaBuildAttempted) return false;
    g_fxaaBuildAttempted = true;

    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFxaaFragmentShader);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        ++g_fxaaErrorCount;
        LOGE("[fxaa] shader build failed; exact linear blit fallback active");
        return false;
    }

    s.fxaaProgram = glCreateProgram();
    glAttachShader(s.fxaaProgram, vertex);
    glAttachShader(s.fxaaProgram, fragment);
    glLinkProgram(s.fxaaProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(s.fxaaProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024]{};
        glGetProgramInfoLog(s.fxaaProgram, sizeof(log), nullptr, log);
        LOGE("[fxaa] program link failed: %s", log);
        glDeleteProgram(s.fxaaProgram);
        s.fxaaProgram = 0;
        ++g_fxaaErrorCount;
        LOGE("[fxaa] exact linear blit fallback active");
        return false;
    }

    s.fxaaTextureUniform = glGetUniformLocation(s.fxaaProgram, "uTexture");
    s.fxaaInverseSizeUniform =
        glGetUniformLocation(s.fxaaProgram, "uInverseSourceSize");
    s.fxaaUnderwaterUniform =
        glGetUniformLocation(s.fxaaProgram, "uUnderwater");
    s.fxaaWaterDepthUniform =
        glGetUniformLocation(s.fxaaProgram, "uWaterDepth");
    s.fxaaMobileColorModeUniform =
        glGetUniformLocation(s.fxaaProgram, "uMobileColorMode");
    s.fxaaContrastMultUniform =
        glGetUniformLocation(s.fxaaProgram, "uContrastMult");
    s.fxaaContrastAddUniform =
        glGetUniformLocation(s.fxaaProgram, "uContrastAdd");
    s.fxaaRedGradeUniform =
        glGetUniformLocation(s.fxaaProgram, "uRedGrade");
    s.fxaaGreenGradeUniform =
        glGetUniformLocation(s.fxaaProgram, "uGreenGrade");
    s.fxaaBlueGradeUniform =
        glGetUniformLocation(s.fxaaProgram, "uBlueGrade");
    glGenVertexArrays(1, &s.fxaaVertexArray);
    glGenSamplers(1, &s.fxaaSampler);
    if (s.fxaaTextureUniform < 0 || s.fxaaInverseSizeUniform < 0 ||
        s.fxaaUnderwaterUniform < 0 || s.fxaaWaterDepthUniform < 0 ||
        s.fxaaMobileColorModeUniform < 0 ||
        s.fxaaContrastMultUniform < 0 || s.fxaaContrastAddUniform < 0 ||
        s.fxaaRedGradeUniform < 0 || s.fxaaGreenGradeUniform < 0 ||
        s.fxaaBlueGradeUniform < 0 ||
        s.fxaaVertexArray == 0 || s.fxaaSampler == 0) {
        LOGE("[fxaa] program resources invalid tex=%d inv=%d water=%d depth=%d "
             "mobile=%d mult=%d add=%d grade=%d/%d/%d vao=%u sampler=%u",
             s.fxaaTextureUniform, s.fxaaInverseSizeUniform,
             s.fxaaUnderwaterUniform, s.fxaaWaterDepthUniform,
             s.fxaaMobileColorModeUniform, s.fxaaContrastMultUniform,
             s.fxaaContrastAddUniform, s.fxaaRedGradeUniform,
             s.fxaaGreenGradeUniform, s.fxaaBlueGradeUniform,
             s.fxaaVertexArray, s.fxaaSampler);
        if (s.fxaaSampler != 0) glDeleteSamplers(1, &s.fxaaSampler);
        if (s.fxaaVertexArray != 0) glDeleteVertexArrays(1, &s.fxaaVertexArray);
        glDeleteProgram(s.fxaaProgram);
        s.fxaaSampler = 0;
        s.fxaaVertexArray = 0;
        s.fxaaProgram = 0;
        s.fxaaTextureUniform = -1;
        s.fxaaInverseSizeUniform = -1;
        s.fxaaUnderwaterUniform = -1;
        s.fxaaWaterDepthUniform = -1;
        s.fxaaMobileColorModeUniform = -1;
        s.fxaaContrastMultUniform = -1;
        s.fxaaContrastAddUniform = -1;
        s.fxaaRedGradeUniform = -1;
        s.fxaaGreenGradeUniform = -1;
        s.fxaaBlueGradeUniform = -1;
        ++g_fxaaErrorCount;
        LOGE("[fxaa] exact linear blit fallback active");
        return false;
    }
    glSamplerParameteri(s.fxaaSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s.fxaaSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(s.fxaaSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s.fxaaSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    LOGI("[fxaa] GLES program ready (Vice City spatial filter)");
    return true;
}

bool BuildUnderwaterCopyProgram() {
    if (s.underwaterProgram != 0 && !g_underwaterRuntimeFailed) return true;
    if (g_underwaterBuildAttempted) return false;
    g_underwaterBuildAttempted = true;

    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = CompileShader(
        GL_FRAGMENT_SHADER, kUnderwaterCopyFragmentShader);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        LOGE("[underwater] copy shader build failed; linear blit retained");
        return false;
    }

    s.underwaterProgram = glCreateProgram();
    glAttachShader(s.underwaterProgram, vertex);
    glAttachShader(s.underwaterProgram, fragment);
    glLinkProgram(s.underwaterProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(s.underwaterProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024]{};
        glGetProgramInfoLog(s.underwaterProgram, sizeof(log), nullptr, log);
        LOGE("[underwater] copy program link failed: %s", log);
        glDeleteProgram(s.underwaterProgram);
        s.underwaterProgram = 0;
        return false;
    }

    s.underwaterTextureUniform =
        glGetUniformLocation(s.underwaterProgram, "uTexture");
    s.underwaterAmountUniform =
        glGetUniformLocation(s.underwaterProgram, "uUnderwater");
    s.underwaterDepthUniform =
        glGetUniformLocation(s.underwaterProgram, "uWaterDepth");
    s.underwaterMobileColorModeUniform =
        glGetUniformLocation(s.underwaterProgram, "uMobileColorMode");
    s.underwaterContrastMultUniform =
        glGetUniformLocation(s.underwaterProgram, "uContrastMult");
    s.underwaterContrastAddUniform =
        glGetUniformLocation(s.underwaterProgram, "uContrastAdd");
    s.underwaterRedGradeUniform =
        glGetUniformLocation(s.underwaterProgram, "uRedGrade");
    s.underwaterGreenGradeUniform =
        glGetUniformLocation(s.underwaterProgram, "uGreenGrade");
    s.underwaterBlueGradeUniform =
        glGetUniformLocation(s.underwaterProgram, "uBlueGrade");
    glGenVertexArrays(1, &s.underwaterVertexArray);
    glGenSamplers(1, &s.underwaterSampler);
    if (s.underwaterTextureUniform < 0 ||
        s.underwaterAmountUniform < 0 || s.underwaterDepthUniform < 0 ||
        s.underwaterMobileColorModeUniform < 0 ||
        s.underwaterContrastMultUniform < 0 ||
        s.underwaterContrastAddUniform < 0 ||
        s.underwaterRedGradeUniform < 0 ||
        s.underwaterGreenGradeUniform < 0 ||
        s.underwaterBlueGradeUniform < 0 ||
        s.underwaterVertexArray == 0 || s.underwaterSampler == 0) {
        LOGE("[mobile.color] copy resources invalid tex=%d water=%d depth=%d "
             "mobile=%d mult=%d add=%d grade=%d/%d/%d vao=%u sampler=%u",
             s.underwaterTextureUniform, s.underwaterAmountUniform,
             s.underwaterDepthUniform,
             s.underwaterMobileColorModeUniform,
             s.underwaterContrastMultUniform,
             s.underwaterContrastAddUniform,
             s.underwaterRedGradeUniform,
             s.underwaterGreenGradeUniform,
             s.underwaterBlueGradeUniform,
             s.underwaterVertexArray, s.underwaterSampler);
        if (s.underwaterSampler != 0)
            glDeleteSamplers(1, &s.underwaterSampler);
        if (s.underwaterVertexArray != 0)
            glDeleteVertexArrays(1, &s.underwaterVertexArray);
        glDeleteProgram(s.underwaterProgram);
        s.underwaterSampler = 0;
        s.underwaterVertexArray = 0;
        s.underwaterProgram = 0;
        s.underwaterTextureUniform = -1;
        s.underwaterAmountUniform = -1;
        s.underwaterDepthUniform = -1;
        s.underwaterMobileColorModeUniform = -1;
        s.underwaterContrastMultUniform = -1;
        s.underwaterContrastAddUniform = -1;
        s.underwaterRedGradeUniform = -1;
        s.underwaterGreenGradeUniform = -1;
        s.underwaterBlueGradeUniform = -1;
        return false;
    }
    glSamplerParameteri(s.underwaterSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s.underwaterSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(s.underwaterSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s.underwaterSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    LOGI("[mobile.color] one-sample colour/underwater resolve ready");
    return true;
}

bool BuildHudCompositeProgram() {
    if (s.hudCompositeProgram!=0) return true;
    GLuint vertex=CompileShader(GL_VERTEX_SHADER,kVertexShader);
    GLuint fragment=CompileShader(GL_FRAGMENT_SHADER,
                                  kHudCompositeFragmentShader);
    if (!vertex||!fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        LOGE("[hud] in-eye composite shader build failed");
        return false;
    }
    s.hudCompositeProgram=glCreateProgram();
    glAttachShader(s.hudCompositeProgram,vertex);
    glAttachShader(s.hudCompositeProgram,fragment);
    glLinkProgram(s.hudCompositeProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked=GL_FALSE;
    glGetProgramiv(s.hudCompositeProgram,GL_LINK_STATUS,&linked);
    if (linked!=GL_TRUE) {
        char log[1024]{};
        glGetProgramInfoLog(s.hudCompositeProgram,sizeof(log),nullptr,log);
        LOGE("[hud] in-eye composite link failed: %s",log);
        glDeleteProgram(s.hudCompositeProgram);
        s.hudCompositeProgram=0;
        return false;
    }
    s.hudCompositeTextureUniform=glGetUniformLocation(
        s.hudCompositeProgram,"uTexture");
    s.hudCompositeSourceRectUniform=glGetUniformLocation(
        s.hudCompositeProgram,"uSourceRect");
    s.hudCompositeDestinationRectUniform=glGetUniformLocation(
        s.hudCompositeProgram,"uDestinationRect");
    s.hudCompositeHighlightUniform=glGetUniformLocation(
        s.hudCompositeProgram,"uHighlight");
    glGenVertexArrays(1,&s.hudCompositeVertexArray);
    glGenSamplers(1,&s.hudCompositeSampler);
    if (s.hudCompositeTextureUniform<0||
        s.hudCompositeSourceRectUniform<0||
        s.hudCompositeDestinationRectUniform<0||
        s.hudCompositeHighlightUniform<0||
        !s.hudCompositeVertexArray||!s.hudCompositeSampler) {
        LOGE("[hud] in-eye composite resources invalid");
        return false;
    }
    glSamplerParameteri(s.hudCompositeSampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glSamplerParameteri(s.hudCompositeSampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glSamplerParameteri(s.hudCompositeSampler,GL_TEXTURE_WRAP_S,
                        GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s.hudCompositeSampler,GL_TEXTURE_WRAP_T,
                        GL_CLAMP_TO_EDGE);
    LOGI("[hud] in-eye alpha composite ready");
    return true;
}

bool SetupHudTextRenderer(JavaVM* vm,jobject activity) {
    if (g_renderVrTextMethod&&g_hudTextApplication) return true;
    if (!vm||!activity) return false;
    JNIEnv* env=nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK||
        !env) return false;

    jclass activityClass=env->GetObjectClass(activity);
    jmethodID getApplication=activityClass?env->GetMethodID(
        activityClass,"getApplication","()Landroid/app/Application;"):nullptr;
    jobject application=getApplication?env->CallObjectMethod(
        activity,getApplication):nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        application=nullptr;
    }
    jclass applicationClass=application?env->GetObjectClass(application):nullptr;
    jmethodID renderMethod=applicationClass?env->GetMethodID(
        applicationClass,"renderVrText","(Ljava/lang/String;III)[I"):nullptr;
    if (renderMethod&&application) {
        g_hudTextVm=vm;
        g_hudTextApplication=env->NewGlobalRef(application);
        g_renderVrTextMethod=renderMethod;
    }
    if (applicationClass) env->DeleteLocalRef(applicationClass);
    if (application) env->DeleteLocalRef(application);
    if (activityClass) env->DeleteLocalRef(activityClass);
    LOGI("[hud.text] Java font renderer %s",
         g_renderVrTextMethod&&g_hudTextApplication?"ready":"unavailable");
    return g_renderVrTextMethod&&g_hudTextApplication;
}

HudTextState SnapshotHudText() {
    std::lock_guard<std::mutex> lock(g_hudTextMutex);
    return g_hudTextState;
}

bool UpdateHudTextTexture(HudTextTexture& target,
                          const std::u16string& text,
                          int style,std::uint64_t revision) {
    if (target.revision==revision) return target.visible;
    target.revision=revision;
    target.visible=false;
    if (text.empty()||!g_hudTextVm||!g_hudTextApplication||
        !g_renderVrTextMethod) return false;

    JNIEnv* env=nullptr;
    bool detach=false;
    const jint envResult=g_hudTextVm->GetEnv(
        reinterpret_cast<void**>(&env),JNI_VERSION_1_6);
    if (envResult==JNI_EDETACHED) {
        if (g_hudTextVm->AttachCurrentThread(&env,nullptr)!=JNI_OK) return false;
        detach=true;
    } else if (envResult!=JNI_OK||!env) {
        return false;
    }

    // 2x the original raster so the Java-side font stays sharp when the layer
    // covers a third of the eye. renderVrText scales its typeface with width.
    constexpr int kTextWidth=2048;
    constexpr int kTextHeight=512;
    jstring javaText=env->NewString(
        reinterpret_cast<const jchar*>(text.data()),
        static_cast<jsize>(text.size()));
    auto pixels=static_cast<jintArray>(env->CallObjectMethod(
        g_hudTextApplication,g_renderVrTextMethod,javaText,
        kTextWidth,kTextHeight,style));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        pixels=nullptr;
    }
    if (javaText) env->DeleteLocalRef(javaText);

    const jsize count=pixels?env->GetArrayLength(pixels):0;
    if (count!=kTextWidth*kTextHeight) {
        if (pixels) env->DeleteLocalRef(pixels);
        if (detach) g_hudTextVm->DetachCurrentThread();
        LOGW("[hud.text] Java raster size invalid: %d",static_cast<int>(count));
        return false;
    }

    std::vector<jint> argb(static_cast<std::size_t>(count));
    env->GetIntArrayRegion(pixels,0,count,argb.data());
    env->DeleteLocalRef(pixels);
    if (detach) g_hudTextVm->DetachCurrentThread();

    // Android returns ARGB words in top-down row order. The HUD shader also
    // addresses source rectangles with a top-left origin, so preserve the row
    // order while converting to RGBA (a second flip makes text upside-down).
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(count)*4u);
    for (int y=0;y<kTextHeight;++y) {
        for (int x=0;x<kTextWidth;++x) {
            const std::uint32_t colour=static_cast<std::uint32_t>(
                argb[static_cast<std::size_t>(y)*kTextWidth+x]);
            const std::size_t out=(static_cast<std::size_t>(y)*
                                   kTextWidth+x)*4u;
            rgba[out+0]=static_cast<std::uint8_t>((colour>>16)&0xffu);
            rgba[out+1]=static_cast<std::uint8_t>((colour>>8)&0xffu);
            rgba[out+2]=static_cast<std::uint8_t>(colour&0xffu);
            rgba[out+3]=static_cast<std::uint8_t>((colour>>24)&0xffu);
        }
    }

    // This runs after several independent renderer passes in one shared GL
    // context. An old error must not mark a successful text upload invisible.
    GLenum staleError=GL_NO_ERROR;
    for (int attempt=0;attempt<8;++attempt) {
        const GLenum error=glGetError();
        if (error==GL_NO_ERROR) break;
        staleError=error;
    }
    if (staleError!=GL_NO_ERROR)
        LOGW("[hud.text] cleared stale GL error 0x%x before upload",staleError);

    if (!target.texture) glGenTextures(1,&target.texture);
    glBindTexture(GL_TEXTURE_2D,target.texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,kTextWidth,kTextHeight,0,
                 GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    target.width=kTextWidth;
    target.height=kTextHeight;
    const GLenum uploadError=glGetError();
    target.visible=target.texture!=0&&uploadError==GL_NO_ERROR;
    LOGI("[hud.text] direct %s raster chars=%zu tex=%u visible=%d gl=0x%x",
         style==1?"big":"help",text.size(),target.texture,
         target.visible?1:0,uploadError);
    return target.visible;
}

// The distance the flat HUD layers pretend to sit at. Drawing the same rect
// into both eyes put them at stereo infinity, so every nearer world surface
// rivalled them and the text read as "behind the 3D world". A small opposite
// horizontal shift per eye gives the overlay a real, comfortable depth.
constexpr float kHudPlaneDistanceMetres = 1.8f;

bool DrawHudComposite(GLuint sourceTexture,int targetWidth,int targetHeight,
                      bool drawClassicElements,int eye) {
    if (!sourceTexture||!BuildHudCompositeProgram()) return false;
    const HudTextState textState=SnapshotHudText();
    const int selected=hud::CalibrationElement();
    const bool calibrating=g_hudActive.load(std::memory_order_relaxed);
    std::u16string smallText=textState.help;
    if (!textState.brief.empty()) {
        if (!smallText.empty()) smallText.append(u"\n\n");
        smallText.append(textState.brief);
    }
    std::u16string bigText=textState.big;
    std::u16string timerText=textState.timers;
    // While the HUD menu is open, live mission text is usually absent, which
    // made the text layers impossible to place. Substitute sample strings
    // so their boxes are always visible during calibration; the revision
    // below already includes the calibration state, so entering/leaving the
    // menu re-rasterises and the samples never leak into gameplay.
    if (calibrating) {
        if (smallText.empty())
            smallText=u"HELP TEXT SAMPLE\nSECOND LINE OF THE TUTORIAL BOX\n"
                      u"POSITION AND SCALE ME IN THIS MENU";
        if (bigText.empty())
            bigText=u"BIG MESSAGE SAMPLE";
        if (timerText.empty())
            timerText=u"TIME 04:32\nGOAL 12/30";
    }
    const std::uint64_t textureRevision=(textState.revision<<4)|
        (calibrating?static_cast<std::uint64_t>(selected+1):0u);
    UpdateHudTextTexture(g_briefTextTexture,smallText,0,textureRevision);
    UpdateHudTextTexture(g_bigTextTexture,bigText,1,textureRevision);
    UpdateHudTextTexture(g_timerTextTexture,timerText,0,textureRevision);
    // Per-eye convergence: shift every destination rect toward the nose by
    // the parallax of a plane kHudPlaneDistanceMetres away, so crops and
    // text fuse at that depth instead of at infinity behind the world.
    const float tanXForDepth=std::max(
        0.2f,g_stereoTanX.load(std::memory_order_relaxed));
    const float hudDepthShiftUv=((eye==0)?1.0f:-1.0f)*kStereoHalfIpd/
        (2.0f*kHudPlaneDistanceMetres*tanXForDepth);
    glViewport(0,0,targetWidth,targetHeight);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s.hudCompositeProgram);
    glBindVertexArray(s.hudCompositeVertexArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,sourceTexture);
    glBindSampler(0,s.hudCompositeSampler);
    glUniform1i(s.hudCompositeTextureUniform,0);
    for (int element=0;element<hud::ELEMENT_COUNT;++element) {
        // Text layers render published strings; their source crop is not
        // sampled at all; only their destination calibration is retained.
        if (hud::IsDirectTextElement(element)) continue;
        const bool selectedCalibrationCrop=calibrating&&element==selected;
        // While calibrating, always show the selected real crop in its final
        // rectangle even if the master classic HUD switch/preset is off. This
        // makes source X/Y/size editable instead of presenting an empty box.
        if (!drawClassicElements&&!selectedCalibrationCrop) continue;
        hud::ElementSettings settings=hud::GetElementSettings(element);
        const std::uint64_t scanStart=g_hudScanStartNs.load(
            std::memory_order_acquire);
        if (scanStart&&g_hudScanElement.load(std::memory_order_acquire)==element) {
            const std::uint64_t now=MonotonicNowNs();
            if (now>=scanStart&&now-scanStart<kHudScanDurationNs) {
                const float progress=static_cast<float>(now-scanStart)/
                    static_cast<float>(kHudScanDurationNs);
                const int maxX=std::max(
                    0,kGameplayHudLogicalWidth-settings.sourceWidth);
                const int maxY=std::max(
                    0,kGameplayHudLogicalHeight-settings.sourceHeight);
                const int laneStep=std::max(1,settings.sourceHeight*3/4);
                const int lanes=std::clamp((maxY+laneStep-1)/laneStep+1,1,24);
                const float lanePosition=progress*static_cast<float>(lanes);
                const int lane=std::min(lanes-1,static_cast<int>(lanePosition));
                float across=lanePosition-static_cast<float>(lane);
                if ((lane&1)!=0) across=1.0f-across;
                settings.sourceX=static_cast<int>(std::lround(maxX*across));
                settings.sourceY=lanes>1
                    ? static_cast<int>(std::lround(
                        maxY*static_cast<float>(lane)/static_cast<float>(lanes-1)))
                    : 0;
            } else {
                g_hudScanStartNs.store(0,std::memory_order_release);
                g_hudScanElement.store(-1,std::memory_order_release);
            }
        }
        if (!settings.enabled&&!selectedCalibrationCrop) continue;
        // Crop calibration is stored in logical 1024x576 units; the physical
        // texture is a uniformly scaled copy, so the normalized UVs match.
        glUniform4f(s.hudCompositeSourceRectUniform,
            static_cast<float>(settings.sourceX)/kGameplayHudLogicalWidth,
            static_cast<float>(settings.sourceY)/kGameplayHudLogicalHeight,
            static_cast<float>(settings.sourceWidth)/kGameplayHudLogicalWidth,
            static_cast<float>(settings.sourceHeight)/kGameplayHudLogicalHeight);
        const float baseX=static_cast<float>(settings.screenX)/100.0f;
        const float baseY=static_cast<float>(settings.screenY)/100.0f;
        const float baseW=static_cast<float>(settings.screenWidth)/100.0f;
        const float baseH=static_cast<float>(settings.screenHeight)/100.0f;
        const float scale=static_cast<float>(settings.scaleTenths)/10.0f;
        const float scaledW=baseW*scale;
        const float scaledH=baseH*scale;
        glUniform4f(s.hudCompositeDestinationRectUniform,
            baseX+(baseW-scaledW)*0.5f+hudDepthShiftUv,
            baseY+(baseH-scaledH)*0.5f,
            scaledW,scaledH);
        glUniform1f(s.hudCompositeHighlightUniform,0.0f);
        glDrawArrays(GL_TRIANGLES,0,3);
    }

    const auto drawDirectText=[&](int element,const HudTextTexture& text) {
        if (!text.visible||!text.texture) return;
        const hud::ElementSettings settings=hud::GetElementSettings(element);
        // Visibility remains a gameplay option, but the selected element must
        // always be visible while calibrating or it would be impossible to
        // position again after disabling it.
        if (!settings.enabled&&!(calibrating&&element==selected)) return;
        const float baseX=static_cast<float>(settings.screenX)/100.0f;
        const float baseY=static_cast<float>(settings.screenY)/100.0f;
        const float baseW=static_cast<float>(settings.screenWidth)/100.0f;
        const float baseH=static_cast<float>(settings.screenHeight)/100.0f;
        const float scale=static_cast<float>(settings.scaleTenths)/10.0f;
        const float scaledW=baseW*scale;
        const float scaledH=baseH*scale;
        glBindTexture(GL_TEXTURE_2D,text.texture);
        glUniform4f(s.hudCompositeSourceRectUniform,0.0f,0.0f,1.0f,1.0f);
        glUniform4f(s.hudCompositeDestinationRectUniform,
                    baseX+(baseW-scaledW)*0.5f+hudDepthShiftUv,
                    baseY+(baseH-scaledH)*0.5f,
                    scaledW,scaledH);
        glUniform1f(s.hudCompositeHighlightUniform,0.0f);
        glDrawArrays(GL_TRIANGLES,0,3);
    };
    drawDirectText(hud::MESSAGES,g_bigTextTexture);
    drawDirectText(hud::HELP_TEXT,g_briefTextTexture);
    drawDirectText(hud::TIMERS,g_timerTextTexture);

    if (calibrating) {
        // The calibration sample is always drawn at the element's real final
        // destination. It never opens a raw or enlarged copy elsewhere.
        const hud::ElementSettings settings=hud::GetElementSettings(selected);
        const float baseX=static_cast<float>(settings.screenX)/100.0f;
        const float baseY=static_cast<float>(settings.screenY)/100.0f;
        const float baseW=static_cast<float>(settings.screenWidth)/100.0f;
        const float baseH=static_cast<float>(settings.screenHeight)/100.0f;
        const float scale=static_cast<float>(settings.scaleTenths)/10.0f;
        const float scaledW=baseW*scale;
        const float scaledH=baseH*scale;
        glUniform4f(s.hudCompositeSourceRectUniform,0.0f,0.0f,1.0f,1.0f);
        glUniform4f(s.hudCompositeDestinationRectUniform,
                    baseX+(baseW-scaledW)*0.5f+hudDepthShiftUv,
                    baseY+(baseH-scaledH)*0.5f,
                    scaledW,scaledH);
        // Draw only a calibration outline over the element's real destination.
        glUniform1f(s.hudCompositeHighlightUniform,3.0f);
        glDrawArrays(GL_TRIANGLES,0,3);
    }
    glBindSampler(0,0);
    glBindTexture(GL_TEXTURE_2D,0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    static bool logged=false;
    if (!logged) {
        logged=true;
        LOGI("[hud] full stock surface composited into both stereo eyes");
    }
    return true;
}

void UploadMobileColorUniforms(const MobileColorState& colour,
                               GLint modeUniform,
                               GLint contrastMultUniform,
                               GLint contrastAddUniform,
                               GLint redGradeUniform,
                               GLint greenGradeUniform,
                               GLint blueGradeUniform) {
    glUniform1i(modeUniform, colour.mode);
    if (colour.mode == 1) {
        glUniform3fv(contrastMultUniform, 1, colour.contrastMult);
        glUniform3fv(contrastAddUniform, 1, colour.contrastAdd);
    } else if (colour.mode == 2) {
        glUniform4fv(redGradeUniform, 1, colour.redGrade);
        glUniform4fv(greenGradeUniform, 1, colour.greenGrade);
        glUniform4fv(blueGradeUniform, 1, colour.blueGrade);
    }
}

bool DrawEyeFxaa(GLuint sourceTexture, int sourceWidth, int sourceHeight,
                 int targetWidth, int targetHeight,
                 float underWaterness, float waterDepth,
                 const MobileColorState& mobileColor,
                 const FxaaGlState& restoreState, FxaaFrameStats& stats) {
    if (sourceTexture == 0 || sourceWidth <= 0 || sourceHeight <= 0 ||
        !BuildFxaaProgram() || g_fxaaRuntimeFailed) {
        stats.active = false;
        stats.errors = g_fxaaErrorCount;
        return false;
    }

    const double startMs = perf::MonotonicMs();
    bool drawOk = true;
    if (!g_fxaaFirstDrawValidated) {
        // Drain before touching FXAA state, so setup/draw errors below cannot be
        // mistaken for a stale error from an earlier renderer operation.
        GLenum stale = GL_NO_ERROR;
        for (int i = 0; i < 8; ++i) {
            const GLenum error = glGetError();
            if (error == GL_NO_ERROR) break;
            stale = error;
        }
        if (stale != GL_NO_ERROR)
            LOGW("[fxaa] cleared pre-existing GL error 0x%x", stale);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("[fxaa] swapchain draw FBO incomplete: 0x%x", status);
            drawOk = false;
        }
    }

    if (drawOk) {
        glViewport(0, 0, targetWidth, targetHeight);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(s.fxaaProgram);
        glBindVertexArray(s.fxaaVertexArray);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glBindSampler(0, s.fxaaSampler);
        glUniform1i(s.fxaaTextureUniform, 0);
        glUniform2f(s.fxaaInverseSizeUniform,
                    1.0f / static_cast<float>(sourceWidth),
                    1.0f / static_cast<float>(sourceHeight));
        glUniform1f(s.fxaaUnderwaterUniform,
                    std::clamp(underWaterness, 0.0f, 1.0f));
        glUniform1f(s.fxaaWaterDepthUniform,
                    std::clamp(waterDepth, 0.0f, 100.0f));
        UploadMobileColorUniforms(
            mobileColor, s.fxaaMobileColorModeUniform,
            s.fxaaContrastMultUniform, s.fxaaContrastAddUniform,
            s.fxaaRedGradeUniform, s.fxaaGreenGradeUniform,
            s.fxaaBlueGradeUniform);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!g_fxaaFirstDrawValidated) {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                LOGE("[fxaa] first draw failed: GL error 0x%x", error);
                drawOk = false;
            } else {
                g_fxaaFirstDrawValidated = true;
                LOGI("[fxaa] first eye draw validated source=%dx%d target=%dx%d",
                     sourceWidth, sourceHeight, targetWidth, targetHeight);
            }
        }
    }

    // Late hands use their own shaders and vertex attributes immediately after
    // this pass. Restore the caller's VAO/program now so FXAA's empty VAO cannot
    // accidentally become their mutable geometry VAO.
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, static_cast<GLuint>(restoreState.sampler0));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(restoreState.texture0));
    glActiveTexture(static_cast<GLenum>(restoreState.activeTexture));
    glBindVertexArray(static_cast<GLuint>(restoreState.vertexArray));
    glUseProgram(static_cast<GLuint>(restoreState.program));
    glColorMask(restoreState.colorMask[0], restoreState.colorMask[1],
                restoreState.colorMask[2], restoreState.colorMask[3]);

    stats.submitWallMs += std::max(0.0, perf::MonotonicMs() - startMs);
    if (!drawOk) {
        g_fxaaRuntimeFailed = true;
        ++g_fxaaErrorCount;
        stats.active = false;
        stats.errors = g_fxaaErrorCount;
        LOGE("[fxaa] disabled after validation failure; exact linear blit fallback active");
        return false;
    }

    ++stats.draws;
    stats.active = true;
    stats.errors = g_fxaaErrorCount;
    return true;
}

bool DrawEyeUnderwaterCopy(GLuint sourceTexture,
                           int targetWidth, int targetHeight,
                           float underWaterness, float waterDepth,
                           const MobileColorState& mobileColor,
                           const FxaaGlState& restoreState) {
    if (sourceTexture == 0 || targetWidth <= 0 || targetHeight <= 0 ||
        (underWaterness <= 0.001f && mobileColor.mode == 0) ||
        !BuildUnderwaterCopyProgram() ||
        g_underwaterRuntimeFailed) {
        return false;
    }

    bool drawOk = true;
    if (!g_underwaterFirstDrawValidated) {
        GLenum stale = GL_NO_ERROR;
        for (int i = 0; i < 8; ++i) {
            const GLenum error = glGetError();
            if (error == GL_NO_ERROR) break;
            stale = error;
        }
        if (stale != GL_NO_ERROR)
            LOGW("[mobile.color] cleared pre-existing GL error 0x%x", stale);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("[mobile.color] swapchain draw FBO incomplete: 0x%x", status);
            drawOk = false;
        }
    }

    if (drawOk) {
        glViewport(0, 0, targetWidth, targetHeight);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(s.underwaterProgram);
        glBindVertexArray(s.underwaterVertexArray);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glBindSampler(0, s.underwaterSampler);
        glUniform1i(s.underwaterTextureUniform, 0);
        glUniform1f(s.underwaterAmountUniform,
                    std::clamp(underWaterness, 0.0f, 1.0f));
        glUniform1f(s.underwaterDepthUniform,
                    std::clamp(waterDepth, 0.0f, 100.0f));
        UploadMobileColorUniforms(
            mobileColor, s.underwaterMobileColorModeUniform,
            s.underwaterContrastMultUniform,
            s.underwaterContrastAddUniform,
            s.underwaterRedGradeUniform, s.underwaterGreenGradeUniform,
            s.underwaterBlueGradeUniform);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!g_underwaterFirstDrawValidated) {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                LOGE("[mobile.color] first copy draw failed: GL error 0x%x",
                     error);
                drawOk = false;
            } else {
                g_underwaterFirstDrawValidated = true;
                LOGI("[mobile.color] first copy draw validated target=%dx%d",
                     targetWidth, targetHeight);
            }
        }
    }

    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, static_cast<GLuint>(restoreState.sampler0));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(restoreState.texture0));
    glActiveTexture(static_cast<GLenum>(restoreState.activeTexture));
    glBindVertexArray(static_cast<GLuint>(restoreState.vertexArray));
    glUseProgram(static_cast<GLuint>(restoreState.program));
    glColorMask(restoreState.colorMask[0], restoreState.colorMask[1],
                restoreState.colorMask[2], restoreState.colorMask[3]);

    if (!drawOk) {
        g_underwaterRuntimeFailed = true;
        LOGE("[mobile.color] graded copy disabled; linear blit retained");
        return false;
    }
    return true;
}

// ---- Menu laser cursor -----------------------------------------------------
// A real desktop-style arrow sprite: the classic pointer outline is rasterised
// once on the CPU into an RGBA texture with a signed-distance pass (white
// fill, dark border, soft antialiased edges and a subtle drop shadow), then
// drawn every frame as one alpha-blended quad with its tip on the laser hit
// point. Looks like an OS cursor instead of a painted rectangle.
constexpr int kCursorTexW = 96;
constexpr int kCursorTexH = 144;
GLuint g_cursorTexture = 0;
GLuint g_cursorProgram = 0;
GLuint g_cursorVao = 0;
GLint  g_cursorRectUniform = -1;
GLint  g_cursorTexUniform = -1;

bool BuildCursorResources() {
    if (g_cursorProgram != 0 && g_cursorTexture != 0) return true;

    if (g_cursorTexture == 0) {
        // Classic arrow polygon, y measured DOWN from the tip, coordinates as
        // a fraction of the arrow height (left edge, tail notch, tail, wing).
        static const float pts[][2] = {
            {0.000f, 0.000f}, {0.000f, 0.800f}, {0.192f, 0.642f},
            {0.300f, 0.900f}, {0.417f, 0.850f}, {0.308f, 0.600f},
            {0.542f, 0.600f}};
        constexpr int n = static_cast<int>(std::size(pts));
        const float scale = 124.0f;   // arrow height in texels
        const float ox = 10.0f, oy = 8.0f;
        const float outline = 4.5f;   // border thickness, texels
        const float shadowOff = 5.0f; // baked drop-shadow offset
        std::vector<unsigned char> rgba(
            static_cast<size_t>(kCursorTexW) * kCursorTexH * 4, 0);
        auto signedDistance = [&](float fx, float fy) {
            const float px = fx / scale, py = fy / scale;
            float best = 1e9f;
            bool inside = false;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                const float x1 = pts[j][0], y1 = pts[j][1];
                const float x2 = pts[i][0], y2 = pts[i][1];
                const float dx = x2 - x1, dy = y2 - y1;
                const float t = std::clamp(
                    ((px - x1) * dx + (py - y1) * dy) /
                        (dx * dx + dy * dy + 1e-12f), 0.0f, 1.0f);
                const float ex = px - (x1 + t * dx), ey = py - (y1 + t * dy);
                best = std::min(best, std::sqrt(ex * ex + ey * ey));
                if (((y1 > py) != (y2 > py)) &&
                    (px < (x2 - x1) * (py - y1) / (y2 - y1 + 1e-12f) + x1))
                    inside = !inside;
            }
            return (inside ? -best : best) * scale;  // texels
        };
        auto coverage = [](float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); };
        for (int y = 0; y < kCursorTexH; ++y) {
            for (int x = 0; x < kCursorTexW; ++x) {
                const float fx = static_cast<float>(x) + 0.5f - ox;
                const float fy = static_cast<float>(y) + 0.5f - oy;
                const float sd = signedDistance(fx, fy);
                const float body = coverage(sd - outline);   // fill + border
                const float white = coverage(sd + 1.0f);     // fill only
                const float shadow = 0.35f * coverage(
                    signedDistance(fx - shadowOff, fy - shadowOff) - outline);
                // Composite the arrow over its shadow (straight alpha).
                const float alpha = body + shadow * (1.0f - body);
                float color = 0.0f;
                if (alpha > 0.0f)
                    color = (white * 0.96f * body) / alpha;
                unsigned char* px8 =
                    &rgba[(static_cast<size_t>(y) * kCursorTexW + x) * 4];
                px8[0] = px8[1] = px8[2] =
                    static_cast<unsigned char>(std::clamp(color, 0.0f, 1.0f) * 255.0f + 0.5f);
                px8[3] = static_cast<unsigned char>(
                    std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
        glGenTextures(1, &g_cursorTexture);
        glBindTexture(GL_TEXTURE_2D, g_cursorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kCursorTexW, kCursorTexH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (g_cursorProgram == 0) {
        static const char* kCursorVertex = R"(#version 300 es
uniform vec4 uRect;
out vec2 vUv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = vec2(c.x, 1.0 - c.y);
    gl_Position = vec4(uRect.xy + c * uRect.zw, 0.0, 1.0);
})";
        static const char* kCursorFragment = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec2 vUv;
out vec4 outColor;
void main() { outColor = texture(uTexture, vUv); })";
        const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kCursorVertex);
        const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kCursorFragment);
        if (vertex == 0 || fragment == 0) return false;
        g_cursorProgram = glCreateProgram();
        glAttachShader(g_cursorProgram, vertex);
        glAttachShader(g_cursorProgram, fragment);
        glLinkProgram(g_cursorProgram);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(g_cursorProgram, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            char log[512]{};
            glGetProgramInfoLog(g_cursorProgram, sizeof(log), nullptr, log);
            LOGE("cursor program link failed: %s", log);
            glDeleteProgram(g_cursorProgram);
            g_cursorProgram = 0;
            return false;
        }
        g_cursorRectUniform = glGetUniformLocation(g_cursorProgram, "uRect");
        g_cursorTexUniform = glGetUniformLocation(g_cursorProgram, "uTexture");
        glGenVertexArrays(1, &g_cursorVao);  // empty VAO; gl_VertexID quad
    }
    return true;
}

void BlitGameFrame() {
    glUseProgram(s.blitProgram);
    glBindVertexArray(s.blitVertexArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(kTextureExternalOES, s.gameTexture);
    glUniform1i(s.blitTextureUniform, 0);
    glUniformMatrix4fv(s.blitTransformUniform, 1, GL_FALSE, s.gameTransform);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// Draw the game frame into the theater swapchain and describe it as a quad. All
// GL state the blit touches is saved and restored, because this runs on the same
// context the compositor and (indirectly) the engine share.
bool RenderTheaterQuad(XrSpace space, XrCompositionLayerQuad& quad, bool fillView = false) {
    if (!BuildBlitProgram()) {
        return false;
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(s.theater.handle, &acquire, &imageIndex),
               "xrAcquireSwapchainImage(theater)")) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(s.theater.handle, &wait), "xrWaitSwapchainImage(theater)")) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Check(xrReleaseSwapchainImage(s.theater.handle, &release),
              "xrReleaseSwapchainImage(theater wait-fail)");
        return false;
    }

    GLint prevFbo = 0, prevVp[4]{}, prevProg = 0, prevVao = 0, prevTex = 0, prevActive = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND),
                    cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST),
                    framebufferSrgb = g_swapchainSrgb
                                          ? glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT)
                                          : GL_FALSE;
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    if (g_swapchainSrgb) glDisable(GL_FRAMEBUFFER_SRGB_EXT);  // copy the game frame raw; no re-encode

    glBindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s.theater.images[imageIndex].image, 0);
    glViewport(0, 0, s.theater.width, s.theater.height);

    int fvSeq = -1;
    const int fvSet = fillView ? StereoReadSet(fvSeq) : -1;
    const GLuint eyeTex = fvSet >= 0 ? g_stereoEyeTex[fvSet][0].load(std::memory_order_relaxed) : 0;
    if (eyeTex != 0) {
        // Stereo Step 1: the game rendered the world into this offscreen texture
        // (on the shared context). Copy it onto the screen. Flip Y — the engine's
        // FBO is GL bottom-up while the swapchain wants top-down.
        const int ew = g_stereoEyeW.load(std::memory_order_relaxed);
        const int eh = g_stereoEyeH.load(std::memory_order_relaxed);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.eyeReadFramebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eyeTex, 0);
        static bool eyeDiag = false;
        if (!eyeDiag) {
            const GLenum st = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            const GLboolean isTex = glIsTexture(eyeTex);
            LOGI("[stereo] consumer eye blit: tex=%u isTexture=%d readFBstatus=0x%x glErr=0x%x %dx%d",
                 eyeTex, isTex, st, glGetError(), ew, eh);
            eyeDiag = true;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.framebuffer);
        glBlitFramebuffer(0, 0, ew, eh, 0, 0, s.theater.width, s.theater.height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
    } else if (s.gameTexture != 0) {
        BlitGameFrame();
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Late GL colours use the same legacy/display-referred byte space as the
    // already-encoded game frame (including the RGBA8 hand albedo), so keep sRGB
    // write conversion disabled until the caller's saved state is restored below.

    // A visible aiming cursor on the screen, so the menu can be pointed at and
    // tapped instead of blind-swiped. One alpha-blended quad carrying the
    // pre-rasterised desktop-arrow sprite, tip exactly on the laser hit point.
    // pointerV is measured from the top, the framebuffer from the bottom,
    // hence the Y flip.
    if (s.input.pointerValid && BuildCursorResources()) {
        const float cw = static_cast<float>(s.theater.width);
        const float ch = static_cast<float>(s.theater.height);
        const float px = s.input.pointerU * cw;
        const float py = (1.0f - s.input.pointerV) * ch;
        const float hpx = std::max(64.0f, cw / 20.0f);  // arrow height, px
        const float wpx = hpx * (static_cast<float>(kCursorTexW) / kCursorTexH);
        glUseProgram(g_cursorProgram);
        glBindVertexArray(g_cursorVao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_cursorTexture);
        glUniform1i(g_cursorTexUniform, 0);
        // Quad rect in NDC: tip at the top-left corner, body down-right.
        glUniform4f(g_cursorRectUniform,
                    px / cw * 2.0f - 1.0f,
                    (py - hpx) / ch * 2.0f - 1.0f,
                    wpx / cw * 2.0f,
                    hpx / ch * 2.0f);
        GLint prevSrcRgb = 0, prevDstRgb = 0, prevSrcA = 0, prevDstA = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
        glBlendFuncSeparate(static_cast<GLenum>(prevSrcRgb),
                            static_cast<GLenum>(prevDstRgb),
                            static_cast<GLenum>(prevSrcA),
                            static_cast<GLenum>(prevDstA));
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glActiveTexture(static_cast<GLenum>(prevActive));
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glUseProgram(static_cast<GLuint>(prevProg));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (g_swapchainSrgb) {
        if (framebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
        else                 glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Check(xrReleaseSwapchainImage(s.theater.handle, &release), "xrReleaseSwapchainImage(theater)");

    // Menu: a world-locked cinema screen (comfortable, distant). In-world: a
    // head-locked screen brought close and wide so it fills the view like a proper
    // VR image while staying rigid to the head (no rotation tremble).
    const float kDistanceMeters = fillView ? 1.4f : 2.2f;
    const float kWidthMeters    = fillView ? 3.2f : 3.2f;
    const float aspect = static_cast<float>(s.theater.width) / static_cast<float>(s.theater.height);

    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags   = 0;
    quad.space        = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain        = s.theater.handle;
    quad.subImage.imageArrayIndex  = 0;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {s.theater.width, s.theater.height};
    quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    quad.pose.position    = {0.0f, 0.0f, -kDistanceMeters};
    quad.size             = {kWidthMeters, kWidthMeters / aspect};
    return true;
}

// ---------------------------------------------------------------------------
// Controller input (Meta Touch)
// ---------------------------------------------------------------------------

XrPath StringToPath(const char* text) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(s.instance, text, &path);
    return path;
}

bool CreateAction(XrAction& action, XrActionType type, const char* name, bool perHand) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (perHand) {
        info.countSubactionPaths = 2;
        info.subactionPaths      = s.handPaths;
    }
    return Check(xrCreateAction(s.actionSet, &info, &action), name);
}

bool CreateInput() {
    s.handPaths[0] = StringToPath("/user/hand/left");
    s.handPaths[1] = StringToPath("/user/hand/right");

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gameplay");
    std::strcpy(setInfo.localizedActionSetName, "Gameplay");
    if (!Check(xrCreateActionSet(s.instance, &setInfo, &s.actionSet), "xrCreateActionSet")) {
        return false;
    }

    if (!CreateAction(s.stickAction,   XR_ACTION_TYPE_VECTOR2F_INPUT, "stick",   true)  ||
        !CreateAction(s.triggerAction, XR_ACTION_TYPE_FLOAT_INPUT,    "trigger", true)  ||
        !CreateAction(s.gripAction,    XR_ACTION_TYPE_FLOAT_INPUT,    "grip",    true)  ||
        !CreateAction(s.aAction,       XR_ACTION_TYPE_BOOLEAN_INPUT,  "a",       false) ||
        !CreateAction(s.bAction,       XR_ACTION_TYPE_BOOLEAN_INPUT,  "b",       false) ||
        !CreateAction(s.xAction,       XR_ACTION_TYPE_BOOLEAN_INPUT,  "x",       false) ||
        !CreateAction(s.yAction,       XR_ACTION_TYPE_BOOLEAN_INPUT,  "y",       false) ||
        !CreateAction(s.menuAction,    XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu",    false) ||
        !CreateAction(s.stickClickAction, XR_ACTION_TYPE_BOOLEAN_INPUT, "stickclick", true) ||
        !CreateAction(s.aimAction,     XR_ACTION_TYPE_POSE_INPUT,     "aim",     true)  ||
        !CreateAction(s.gripPoseAction,XR_ACTION_TYPE_POSE_INPUT,     "grippose", true)) {
        return false;
    }

    struct Bind { XrAction action; const char* path; };
    const Bind binds[] = {
        {s.stickAction,   "/user/hand/left/input/thumbstick"},
        {s.stickAction,   "/user/hand/right/input/thumbstick"},
        {s.triggerAction, "/user/hand/left/input/trigger/value"},
        {s.triggerAction, "/user/hand/right/input/trigger/value"},
        {s.gripAction,    "/user/hand/left/input/squeeze/value"},
        {s.gripAction,    "/user/hand/right/input/squeeze/value"},
        {s.aAction,       "/user/hand/right/input/a/click"},
        {s.bAction,       "/user/hand/right/input/b/click"},
        {s.xAction,       "/user/hand/left/input/x/click"},
        {s.yAction,       "/user/hand/left/input/y/click"},
        {s.menuAction,    "/user/hand/left/input/menu/click"},
        {s.stickClickAction, "/user/hand/left/input/thumbstick/click"},
        {s.stickClickAction, "/user/hand/right/input/thumbstick/click"},
        {s.aimAction,     "/user/hand/right/input/aim/pose"},
        {s.aimAction,     "/user/hand/left/input/aim/pose"},
        {s.gripPoseAction,"/user/hand/left/input/grip/pose"},
        {s.gripPoseAction,"/user/hand/right/input/grip/pose"},
    };
    std::vector<XrActionSuggestedBinding> suggested;
    for (const Bind& b : binds) {
        suggested.push_back({b.action, StringToPath(b.path)});
    }
    XrInteractionProfileSuggestedBinding profile{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    profile.interactionProfile     = StringToPath("/interaction_profiles/oculus/touch_controller");
    profile.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
    profile.suggestedBindings      = suggested.data();
    if (!Check(xrSuggestInteractionProfileBindings(s.instance, &profile),
               "xrSuggestInteractionProfileBindings")) {
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets      = &s.actionSet;
    if (!Check(xrAttachSessionActionSets(s.session, &attach), "xrAttachSessionActionSets")) {
        return false;
    }

    // A space that tracks the right controller's aim pose, for the laser pointer.
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action            = s.aimAction;
    spaceInfo.subactionPath     = s.handPaths[1];   // right hand
    spaceInfo.poseInActionSpace.orientation = {0, 0, 0, 1};
    Check(xrCreateActionSpace(s.session, &spaceInfo, &s.aimSpace), "xrCreateActionSpace(aim)");

    // Per-hand grip + aim spaces for the VR hands.
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo gi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        gi.action = s.gripPoseAction; gi.subactionPath = s.handPaths[h];
        gi.poseInActionSpace.orientation = {0, 0, 0, 1};
        Check(xrCreateActionSpace(s.session, &gi, &s.handGripSpace[h]), "xrCreateActionSpace(grip)");
        XrActionSpaceCreateInfo ai{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ai.action = s.aimAction; ai.subactionPath = s.handPaths[h];
        ai.poseInActionSpace.orientation = {0, 0, 0, 1};
        Check(xrCreateActionSpace(s.session, &ai, &s.handAimSpace[h]), "xrCreateActionSpace(handaim)");
    }

    LOGI("controller input ready");
    return true;
}

// Cast the right controller's aim ray at the theater screen and store the hit as
// [0,1] u,v. The screen matches RenderTheaterQuad: centred kTheaterDistance in
// front, kTheaterWidth wide, facing the user.
void UpdatePointer(InputState& in, XrTime displayTime) {
    in.pointerValid = false;
    if (s.aimSpace == XR_NULL_HANDLE) {
        return;
    }
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(s.aimSpace, s.space, displayTime, &loc)) ||
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
        return;
    }

    const XrQuaternionf& q = loc.pose.orientation;
    const XrVector3f&    o = loc.pose.position;
    // Forward = q * (0,0,-1): the aim ray points down the controller's -Z.
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fy = -2.0f * (q.y * q.z - q.w * q.x);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));

    constexpr float kTheaterWidth    = 3.2f;
    constexpr float kTheaterDistance = 2.2f;
    const float aspect  = static_cast<float>(s.theater.width) / static_cast<float>(s.theater.height);
    const float height  = kTheaterWidth / aspect;
    const float planeZ  = -kTheaterDistance;

    if (fz >= -1e-4f) {   // ray points away from the screen
        return;
    }
    const float t = (planeZ - o.z) / fz;
    if (t <= 0.0f) {
        return;
    }
    const float hx = o.x + t * fx;
    const float hy = o.y + t * fy;
    if (std::abs(hx) > kTheaterWidth * 0.5f || std::abs(hy) > height * 0.5f) {
        return;   // ray misses the screen
    }

    in.pointerValid   = true;
    in.pointerU       = hx / kTheaterWidth + 0.5f;   // 0 left, 1 right
    in.pointerV       = 0.5f - hy / height;          // 0 top,  1 bottom
    in.pointerPressed = in.triggers[1] > 0.5f;       // right trigger taps
}

float ReadFloat(XrAction action, XrPath hand) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action         = action;
    get.subactionPath  = hand;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(xrGetActionStateFloat(s.session, &get, &state)) && state.isActive) {
        return state.currentState;
    }
    return 0.0f;
}

void ReadStick(XrPath hand, float out[2]) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action        = s.stickAction;
    get.subactionPath = hand;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &get, &state)) && state.isActive) {
        out[0] = state.currentState.x;
        out[1] = state.currentState.y;
    } else {
        out[0] = out[1] = 0.0f;
    }
}

bool ReadBool(XrAction action, XrPath subactionPath = XR_NULL_PATH) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = subactionPath;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &state)) &&
           state.isActive && state.currentState == XR_TRUE;
}

void SyncInput(XrTime displayTime) {
    if (s.actionSet == XR_NULL_HANDLE) {
        return;
    }
    XrActiveActionSet active{s.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets      = &active;
    if (!XR_SUCCEEDED(xrSyncActions(s.session, &sync))) {
        return;
    }

    InputState in{};
    ReadStick(s.handPaths[0], in.leftStick);
    ReadStick(s.handPaths[1], in.rightStick);
    in.triggers[0] = ReadFloat(s.triggerAction, s.handPaths[0]);
    in.triggers[1] = ReadFloat(s.triggerAction, s.handPaths[1]);
    in.grip[0] = ReadFloat(s.gripAction, s.handPaths[0]);
    in.grip[1] = ReadFloat(s.gripAction, s.handPaths[1]);
    in.a    = ReadBool(s.aAction);
    in.b    = ReadBool(s.bAction);
    in.x    = ReadBool(s.xAction);
    in.y    = ReadBool(s.yAction);
    in.menu = ReadBool(s.menuAction);
    in.l3   = ReadBool(s.stickClickAction, s.handPaths[0]);
    in.r3   = ReadBool(s.stickClickAction, s.handPaths[1]);
    UpdatePointer(in, displayTime);
    s.input = in;
}

} // namespace

void GetInput(InputState& out) {
    out = s.input;
}

void PublishHudText(const std::int16_t* brief, int briefCapacity,
                    const std::int16_t* bigMessages, int bigStyleCount,
                    int bigStyleCapacity,
                    const std::int16_t* help, int helpCapacity) {
    const auto copyGxt=[](const std::int16_t* source, int capacity) {
        std::u16string result;
        if (!source || capacity <= 0) return result;
        result.reserve(static_cast<std::size_t>(std::min(capacity,400)));
        for (int i=0;i<capacity&&source[i]!=0;++i)
            result.push_back(static_cast<char16_t>(
                static_cast<std::uint16_t>(source[i])));
        return result;
    };

    std::u16string nextBig;
    int nextBigStyle=0;
    if (bigMessages && bigStyleCapacity > 0) {
        for (int style=0;style<bigStyleCount;++style) {
            const std::int16_t* candidate=
                bigMessages+style*bigStyleCapacity;
            if (candidate[0]==0) continue;
            nextBig=copyGxt(candidate,bigStyleCapacity);
            nextBigStyle=style;
            break;
        }
    }
    std::u16string nextBrief=copyGxt(brief,briefCapacity);
    std::u16string nextHelp=copyGxt(help,helpCapacity);

    std::lock_guard<std::mutex> lock(g_hudTextMutex);
    if (g_hudTextState.brief==nextBrief&&g_hudTextState.help==nextHelp&&
        g_hudTextState.big==nextBig&&
        g_hudTextState.bigStyle==nextBigStyle) return;
    g_hudTextState.brief=std::move(nextBrief);
    g_hudTextState.help=std::move(nextHelp);
    g_hudTextState.big=std::move(nextBig);
    g_hudTextState.bigStyle=nextBigStyle;
    ++g_hudTextState.revision;
    LOGI("[hud.text] publish rev=%llu brief=%zu help=%zu big=%zu style=%d",
         static_cast<unsigned long long>(g_hudTextState.revision),
         g_hudTextState.brief.size(),g_hudTextState.help.size(),
         g_hudTextState.big.size(),g_hudTextState.bigStyle);
}

void PublishMissionTimersText(const char* text) {
    std::u16string next;
    if (text) {
        for (const char* c = text; *c != '\0' && next.size() < 400; ++c)
            next.push_back(static_cast<char16_t>(
                static_cast<unsigned char>(*c)));
    }
    std::lock_guard<std::mutex> lock(g_hudTextMutex);
    if (g_hudTextState.timers == next) return;
    g_hudTextState.timers = std::move(next);
    ++g_hudTextState.revision;
    LOGI("[hud.text] timers publish rev=%llu chars=%zu",
         static_cast<unsigned long long>(g_hudTextState.revision),
         g_hudTextState.timers.size());
}

void GetHandPoses(HandPose out[2]) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    out[0] = g_handPose[0];
    out[1] = g_handPose[1];
}

bool GetHeadPose(float positionOut[3], float orientationOut[4]) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    if (!g_headPoseValid) {
        return false;
    }
    positionOut[0] = g_headPose.position.x;
    positionOut[1] = g_headPose.position.y;
    positionOut[2] = g_headPose.position.z;
    orientationOut[0] = g_headPose.orientation.x;
    orientationOut[1] = g_headPose.orientation.y;
    orientationOut[2] = g_headPose.orientation.z;
    orientationOut[3] = g_headPose.orientation.w;
    return true;
}

float RecommendedGameFovXDegrees() {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    return g_eyeFovXDeg;
}

bool GetEyePoses(float positionOut[2][3], float orientationOut[2][4]) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    if (!g_headPoseValid) {
        return false;
    }
    for (int e = 0; e < 2; ++e) {
        positionOut[e][0] = g_eyePose[e].position.x;
        positionOut[e][1] = g_eyePose[e].position.y;
        positionOut[e][2] = g_eyePose[e].position.z;
        orientationOut[e][0] = g_eyePose[e].orientation.x;
        orientationOut[e][1] = g_eyePose[e].orientation.y;
        orientationOut[e][2] = g_eyePose[e].orientation.z;
        orientationOut[e][3] = g_eyePose[e].orientation.w;
    }
    return true;
}

// --- stereo eye-texture bridge: producer half (GameThread) -------------------

void SetStereoContextShared(bool shared) { g_stereoShared = shared; }

void SetStereoRenderFov(float tanX, float tanY) {
    g_stereoTanX.store(tanX, std::memory_order_relaxed);
    g_stereoTanY.store(tanY, std::memory_order_relaxed);
}

void NotifyGameFrame() { g_gameFrames.fetch_add(1, std::memory_order_relaxed); }

void SetProfileStats(int recMs, int sceneMs, int skyMs, int endMs, int entCount, int dynCount) {
    g_profRecMs.store(recMs, std::memory_order_relaxed);
    g_profScene.store(sceneMs, std::memory_order_relaxed);
    g_profSky.store(skyMs, std::memory_order_relaxed);
    g_profEnd.store(endMs, std::memory_order_relaxed);
    g_profEnt.store(entCount, std::memory_order_relaxed);
    g_profDynamic.store(dynCount, std::memory_order_relaxed);
}

void SetFrameMs(double ms) { g_profFrameMs.store(static_cast<int>(ms + 0.5), std::memory_order_relaxed); }

void SetMenuState(bool visible, int selection, int count, int category) {
    g_menuVisible.store(visible, std::memory_order_relaxed);
    g_menuSelection.store(selection, std::memory_order_relaxed);
    g_menuCount.store(count, std::memory_order_relaxed);
    g_menuCategory.store(category, std::memory_order_relaxed);
}

void SetPerfLevels(int cpuIdx, int gpuIdx) {
    g_cpuPerfIdx.store(cpuIdx < 0 ? 0 : (cpuIdx > 3 ? 3 : cpuIdx), std::memory_order_relaxed);
    g_gpuPerfIdx.store(gpuIdx < 0 ? 0 : (gpuIdx > 3 ? 3 : gpuIdx), std::memory_order_relaxed);
    g_perfDirty.store(true, std::memory_order_relaxed);
}
int  GetCpuPerfIdx() { return g_cpuPerfIdx.load(std::memory_order_relaxed); }
int  GetGpuPerfIdx() { return g_gpuPerfIdx.load(std::memory_order_relaxed); }
const char* PerfLevelName(int idx) { return kPerfNames[idx < 0 ? 0 : (idx > 3 ? 3 : idx)]; }

void SetGameThreadTid(unsigned int tid) {
    g_gameTid.store(tid, std::memory_order_relaxed);
    g_threadDirty.store(true, std::memory_order_relaxed);
}

void SetCalibPage(bool active, int selection, int hand, int weaponType) {
    g_calibActive.store(active, std::memory_order_relaxed);
    g_calibSel.store(selection, std::memory_order_relaxed);
    g_calibHand.store(hand, std::memory_order_relaxed);
    g_calibWeaponType.store(weaponType, std::memory_order_relaxed);
}

void SetMainMenu(bool active, int selection) {
    g_mainMenuActive.store(active, std::memory_order_relaxed);
    g_mainSel.store(selection, std::memory_order_relaxed);
}

void SetHolsterMenu(bool active, int selection) {
    g_holsterMenuActive.store(active, std::memory_order_relaxed);
    g_holsterMenuSel.store(selection, std::memory_order_relaxed);
}

void SetHolsterCalibMenu(bool active, int selection) {
    g_holsterCalibActive.store(active, std::memory_order_relaxed);
    g_holsterCalibSel.store(selection, std::memory_order_relaxed);
}

void SetDrivingMenu(bool active, int selection, int vehicleType) {
    g_drivingActive.store(active, std::memory_order_relaxed);
    g_drivingSel.store(selection, std::memory_order_relaxed);
    g_drivingVehicleType.store(vehicleType, std::memory_order_relaxed);
}

void SetDrivingCalibrationMenu(bool active, int selection, int hand) {
    g_drivingCalibActive.store(active, std::memory_order_relaxed);
    g_drivingCalibSel.store(selection, std::memory_order_relaxed);
    g_drivingCalibHand.store(hand, std::memory_order_relaxed);
}

void SetLocomotionMenu(bool active, int selection) {
    g_locomotionActive.store(active,std::memory_order_relaxed);
    g_locomotionSel.store(selection,std::memory_order_relaxed);
}

void SetHudMenu(bool active, int selection, int page) {
    g_hudActive.store(active, std::memory_order_relaxed);
    g_hudSel.store(selection, std::memory_order_relaxed);
    g_hudPage.store(page, std::memory_order_relaxed);
    if (!active) {
        g_hudScanStartNs.store(0,std::memory_order_release);
        g_hudScanElement.store(-1,std::memory_order_release);
    }
}

void StartHudSourceScan() {
    const int element=hud::CalibrationElement();
    if (hud::IsDirectTextElement(element)) {
        LOGW("[hud.scan] %s is direct text; no source image to scan",
             hud::ElementName(element));
        return;
    }
    g_hudScanElement.store(element,std::memory_order_release);
    g_hudScanStartNs.store(MonotonicNowNs(),std::memory_order_release);
    LOGI("[hud.scan] started real 1024x576 source sweep for %s (saved crop unchanged)",
         hud::ElementName(element));
}

bool HudSourceScanActive() {
    const std::uint64_t start=g_hudScanStartNs.load(std::memory_order_acquire);
    if (!start) return false;
    const std::uint64_t now=MonotonicNowNs();
    if (now<start||now-start>=kHudScanDurationNs) {
        g_hudScanStartNs.store(0,std::memory_order_release);
        g_hudScanElement.store(-1,std::memory_order_release);
        return false;
    }
    return true;
}

bool HudCalibrationActive() {
    return g_hudActive.load(std::memory_order_acquire);
}

void SetControlsMenu(bool active, int selection) {
    g_controlsMenuActive.store(active, std::memory_order_relaxed);
    g_controlsSel.store(selection, std::memory_order_relaxed);
}

void SetControlsTipsMenu(bool active) {
    g_controlsTipsActive.store(active, std::memory_order_relaxed);
}

void SetAboutMenu(bool active, bool firstRun) {
    g_aboutActive.store(active, std::memory_order_relaxed);
    g_aboutFirstRun.store(firstRun, std::memory_order_relaxed);
}

void SetGraphicsMenu(bool active, int selection) {
    g_gfxActive.store(active, std::memory_order_relaxed);
    g_gfxSel.store(selection, std::memory_order_relaxed);
}

void SetGraphicsDistanceMenu(bool active, int selection) {
    g_gfxDistanceActive.store(active, std::memory_order_relaxed);
    g_gfxDistanceSel.store(selection, std::memory_order_relaxed);
}

void ResetStereoEyeTextures() {
    // Sequence is the publication fence. Clear dimensions as well so a consumer
    // already entering RenderStereoEyeProjection takes its normal theater
    // fallback while the new RenderWare ring receives its first complete pair.
    g_stereoEyeW.store(0, std::memory_order_relaxed);
    g_stereoEyeH.store(0, std::memory_order_relaxed);
    for (auto& generation : g_stereoGeneration)
        generation.store(-1, std::memory_order_relaxed);
    for (auto& publishedNs : g_stereoPublishedNs)
        publishedNs.store(0, std::memory_order_relaxed);
    for (auto& texture : g_stereoHudTex)
        texture.store(0, std::memory_order_relaxed);
    for (auto& value : g_stereoUnderWaterness)
        value.store(0.0f, std::memory_order_relaxed);
    for (auto& value : g_stereoWaterDepth)
        value.store(0.0f, std::memory_order_relaxed);
    g_pendingUnderWaterness.store(0.0f, std::memory_order_relaxed);
    g_pendingWaterDepth.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        g_pendingMobileColor = {};
        for (auto& colour : g_stereoMobileColor) colour = {};
    }
    g_stereoSeq.store(-1, std::memory_order_release);
}

void SetUnderwaterState(float underWaterness, float waterDepth) {
    underWaterness = std::isfinite(underWaterness)
        ? std::clamp(underWaterness, 0.0f, 1.0f)
        : 0.0f;
    waterDepth = std::isfinite(waterDepth)
        ? std::clamp(waterDepth, 0.0f, 100.0f)
        : 0.0f;
    g_pendingUnderWaterness.store(underWaterness,
                                  std::memory_order_relaxed);
    g_pendingWaterDepth.store(waterDepth, std::memory_order_relaxed);

    static std::atomic<bool> wasUnderwater{false};
    const bool underwater = underWaterness >= 0.535f;
    const bool prior = wasUnderwater.exchange(
        underwater, std::memory_order_acq_rel);
    if (prior != underwater) {
        LOGI("[underwater] active=%d amount=%.3f depth=%.3f",
             underwater ? 1 : 0,
             static_cast<double>(underWaterness),
             static_cast<double>(waterDepth));
    }
}

void SetMobileColorState(const MobileColorState& state) {
    MobileColorState next = state;
    if (next.mode != 1 && next.mode != 2) next = {};

    const auto finiteBounded = [](float value) {
        return std::isfinite(value) && std::abs(value) <= 8.0f;
    };
    bool valid = true;
    float signal = 0.0f;
    if (next.mode == 1) {
        for (int i = 0; i < 3; ++i) {
            valid = valid && finiteBounded(next.contrastMult[i]) &&
                finiteBounded(next.contrastAdd[i]);
            signal += std::abs(next.contrastMult[i]);
        }
    } else if (next.mode == 2) {
        for (int i = 0; i < 4; ++i) {
            valid = valid && finiteBounded(next.redGrade[i]) &&
                finiteBounded(next.greenGrade[i]) &&
                finiteBounded(next.blueGrade[i]);
            signal += std::abs(next.redGrade[i]) +
                std::abs(next.greenGrade[i]) +
                std::abs(next.blueGrade[i]);
        }
    }
    if (!valid || (next.mode != 0 && signal < 0.05f)) {
        LOGW("[mobile.color] rejected invalid retail parameters mode=%d",
             next.mode);
        next = {};
    }

    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        g_pendingMobileColor = next;
    }
    static std::atomic<int> lastMode{-1};
    const int prior = lastMode.exchange(next.mode, std::memory_order_acq_rel);
    if (prior != next.mode) {
        LOGI("[mobile.color] compositor mode=%d (0=off 1=contrast 2=grading)",
             next.mode);
    }
}

void SetHolsterMarkers(const float positions[][3], int count) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    g_holsterCount = count > 8 ? 8 : (count < 0 ? 0 : count);
    for (int i = 0; i < g_holsterCount; ++i) {
        g_holsterPos[i][0] = positions[i][0];
        g_holsterPos[i][1] = positions[i][1];
        g_holsterPos[i][2] = positions[i][2];
    }
}

void SetParachuteToggles(const float positions[2][3], const bool grabbed[2],
                         bool visible) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    g_chuteTogglesVisible = visible;
    for (int i = 0; i < 2; ++i) {
        g_chuteToggleGrabbed[i] = grabbed != nullptr && grabbed[i];
        if (positions != nullptr) {
            g_chuteTogglePos[i][0] = positions[i][0];
            g_chuteTogglePos[i][1] = positions[i][1];
            g_chuteTogglePos[i][2] = positions[i][2];
        }
    }
}

void SetRenderedHandPoses(const HandPose poses[2]) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    if (poses == nullptr) {
        g_renderedHandPoseValid = false;
        return;
    }
    g_renderedHandPose[0] = poses[0];
    g_renderedHandPose[1] = poses[1];
    g_renderedHandPoseValid = true;
}

void SetTwoHandVisualState(const TwoHandVisualState& state) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    g_twoHandVisualState = state;

    // Treat malformed publications as an ordinary one-hand frame.  Consumers
    // still receive a complete value in their ring slot, but never index a bad
    // hand or normalise a zero/NaN rotation axis on the present thread.
    const float axisLenSq = state.axis[0] * state.axis[0] +
                            state.axis[1] * state.axis[1] +
                            state.axis[2] * state.axis[2];
    if (!state.active || state.primaryHand < 0 || state.primaryHand > 1 ||
        state.supportHand < 0 || state.supportHand > 1 ||
        state.primaryHand == state.supportHand ||
        !std::isfinite(state.angleRadians) || !std::isfinite(axisLenSq) ||
        axisLenSq < 0.000001f) {
        g_twoHandVisualState.active = false;
    }
}

void SetWeaponGeometry(const float* verts, int numVerts,
                       const unsigned short* idx, int numIdx) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    if (verts == nullptr || numVerts <= 0 || idx == nullptr || numIdx <= 0) {
        g_wpnObjVerts.clear();
        g_wpnObjIdx.clear();
        return;
    }
    g_wpnObjVerts.assign(verts, verts + static_cast<size_t>(numVerts) * 3);
    g_wpnObjIdx.assign(idx, idx + numIdx);
}

// Which hand currently holds a rendered weapon (-1 = none). The GL hand mesh for
// that hand is skipped only when the matching game depth cannot be attached.
std::atomic<int> g_weaponHand{-1};
std::atomic<unsigned int> g_weaponHandMask{0};
std::atomic<bool> g_gameDepthUsable{false};
void SetWeaponHeldHand(int hand) {
    SetWeaponHeldHands(hand >= 0 && hand < 2 ? (1u << hand) : 0u, hand);
}
void SetWeaponHeldHands(unsigned int mask, int preferredHand) {
    g_weaponHandMask.store(mask & 0x3u, std::memory_order_relaxed);
    g_weaponHand.store(preferredHand >= 0 && preferredHand < 2 ? preferredHand : -1,
                       std::memory_order_relaxed);
}

void AddBulletTracer(const float start[3], const float end[3], int weaponType) {
    if (!start || !end) return;
    const XrVector3f a{start[0], start[1], start[2]};
    const XrVector3f b{end[0], end[1], end[2]};
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
        !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z)) {
        return;
    }
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float lengthSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(lengthSq) || lengthSq < 0.0001f || lengthSq > 90000.0f)
        return;

    std::lock_guard<std::mutex> lock(g_bulletTracerMutex);
    const std::uint64_t now = MonotonicNowNs();
    const float rayLength = std::sqrt(lengthSq);
    const XrVector3f direction{dx / rayLength, dy / rayLength, dz / rayLength};
    // Begin just beyond the weapon shell and preserve the exact physical ray.
    // 45 m is long enough to read the impact direction outdoors without turning
    // a 300 m miss into a permanent laser across the whole map.
    const float along = std::min(0.12f, rayLength * 0.05f);
    const float streakLength = std::min(kBulletTracerMaxLength, rayLength) - along;
    if (streakLength < 0.01f) return;
    const XrVector3f streakStart{
        a.x + direction.x * along,
        a.y + direction.y * along,
        a.z + direction.z * along};
    const XrVector3f streakEnd{
        streakStart.x + direction.x * streakLength,
        streakStart.y + direction.y * streakLength,
        streakStart.z + direction.z * streakLength};

    BulletTracer& tracer = g_bulletTracers[g_nextBulletTracer++ % kMaxBulletTracers];
    tracer.valid = true;
    tracer.start = streakStart;
    tracer.end = streakEnd;
    tracer.weaponType = weaponType;
    tracer.bornNs = now;
    tracer.expiresNs = now + kBulletTracerLifetimeNs;
}

void UpdateObjectiveMarker(std::uint64_t id, const float center[3],
                           float radius, float height,
                           unsigned char red, unsigned char green,
                           unsigned char blue, bool stockCone) {
    if (!center) return;
    if (stockCone ? !hud::ObjectiveMarkersIncludeOriginal()
                  : !hud::ObjectiveMarkersIncludeHighlight()) {
        return;
    }
    const XrVector3f point{center[0], center[1], center[2]};
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z) || !std::isfinite(radius) ||
        !std::isfinite(height)) {
        return;
    }
    radius = std::clamp(radius, 0.35f, 2.25f);
    height = std::clamp(height, 0.75f, 5.0f);
    const std::uint64_t now = MonotonicNowNs();

    std::lock_guard<std::mutex> lock(g_objectiveMarkerMutex);
    ObjectiveMarkerVisual* slot = nullptr;
    for (ObjectiveMarkerVisual& marker : g_objectiveMarkers) {
        if (marker.valid && marker.id == id &&
            marker.stockCone == stockCone) {
            slot = &marker;
            break;
        }
        if (!slot && (!marker.valid || marker.expiresNs <= now)) slot = &marker;
    }
    if (!slot) {
        slot = &g_objectiveMarkers[
            g_nextObjectiveMarker++ % kMaxObjectiveMarkers];
    }
    slot->valid = true;
    slot->id = id;
    slot->center = point;
    slot->radius = radius;
    slot->height = height;
    slot->red = red;
    slot->green = green;
    slot->blue = blue;
    slot->stockCone = stockCone;
    slot->expiresNs = now + kObjectiveMarkerLifetimeNs;
}

void SetThrowableTrajectory(int hand, const float points[][3], int count,
                            bool hit) {
    if (hand < 0 || hand > 1) return;
    ThrowableTrajectory next{};
    if (points && count > 1) {
        next.count = std::min(count, kMaxThrowableTrajectoryPoints);
        next.hit = hit;
        for (int i = 0; i < next.count; ++i) {
            const XrVector3f point{points[i][0], points[i][1], points[i][2]};
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                next = {};
                break;
            }
            next.points[i] = point;
        }
    }
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    g_throwableTrajectory[hand] = next;
}

void SetStereoEyeTextures(const unsigned int* tex, const unsigned int* depth,
                          const unsigned int* hudTex,
                          const float* nearZ, const float* farZ,
                          int seq, int width, int height) {
    // `tex` points at a [kStereoSets][2] row-major table of GL texture ids. The
    // ids are stable once the RenderQueue thread has created the rasters, so the
    // per-frame cost here is trivial; only `seq` actually advances. Publish the
    // sequence LAST with release ordering so a consumer that has acquired it sees
    // the matching texture ids.
    const int ws = ((seq % kStereoSets) + kStereoSets) % kStereoSets;
    // Mark this slot incomplete before changing any of its CPU-side metadata.
    // This detects ring wrap while the consumer is copying; it does not claim
    // asynchronous RenderQueue/GPU completion (the lag still serves that role).
    g_stereoGeneration[ws].store(-1, std::memory_order_release);
    g_stereoEyeW.store(width, std::memory_order_relaxed);
    g_stereoEyeH.store(height, std::memory_order_relaxed);
    for (int set = 0; set < kStereoSets; ++set) {
        g_stereoEyeTex[set][0].store(tex[set * 2 + 0], std::memory_order_relaxed);
        g_stereoEyeTex[set][1].store(tex[set * 2 + 1], std::memory_order_relaxed);
        g_stereoEyeDepth[set][0].store(depth ? depth[set * 2 + 0] : 0, std::memory_order_relaxed);
        g_stereoEyeDepth[set][1].store(depth ? depth[set * 2 + 1] : 0, std::memory_order_relaxed);
        g_stereoHudTex[set].store(hudTex ? hudTex[set] : 0,
                                  std::memory_order_relaxed);
    }
    g_stereoEyeNear[ws][0].store(nearZ ? nearZ[0] : 0.05f, std::memory_order_relaxed);
    g_stereoEyeNear[ws][1].store(nearZ ? nearZ[1] : 0.05f, std::memory_order_relaxed);
    g_stereoEyeFar[ws][0].store(farZ ? farZ[0] : 1000.0f, std::memory_order_relaxed);
    g_stereoEyeFar[ws][1].store(farZ ? farZ[1] : 1000.0f, std::memory_order_relaxed);
    g_stereoUnderWaterness[ws].store(
        g_pendingUnderWaterness.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    g_stereoWaterDepth[ws].store(
        g_pendingWaterDepth.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    // Snapshot the pose these pixels were rendered at into the SAME ring slot the
    // textures went to. OnCopyCameraMatrixToRWCam has already published
    // g_renderHeadPose for this GameThread frame, so it is the pose that built the
    // eye cameras. The consumer reads slot (seq - kStereoReadLag), whose pose was
    // stored when its seq was current — so pose and pixels always match.
    int laserHand = -1;
    HandPose laserPose{};
    TwoHandVisualState laserTwoHand{};
    driving::WheelVisualState drivingWheel{};
    // Driving::UpdateInput takes driving-state -> head-pose locks. Snapshot in
    // that same order, before g_headPoseMutex, to avoid the inverse-order
    // deadlock while still publishing it into this exact ring slot below.
    driving::GetWheelVisualState(&drivingWheel);
    drivingWheel.trackedHandsEnabled = driving::ShouldUseTrackedHands();
    g_lastDashModelId.store(
        (drivingWheel.active || drivingWheel.dashAnchorValid)
            ? drivingWheel.modelId : -1,
        std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        g_stereoRenderPose[ws]      = g_renderHeadPose;
        g_stereoRenderPoseValid[ws] = g_renderHeadPoseValid;
        g_stereoHandPose[ws][0] = g_renderedHandPoseValid
            ? g_renderedHandPose[0] : g_handPose[0];
        g_stereoHandPose[ws][1] = g_renderedHandPoseValid
            ? g_renderedHandPose[1] : g_handPose[1];
        g_stereoWeaponHandMask[ws]  = g_weaponHandMask.load(std::memory_order_relaxed);
        g_stereoTwoHandVisualState[ws] = g_twoHandVisualState;
        g_stereoScopeState[ws] = scopeaim::Snapshot();
        g_stereoDrivingWheelState[ws] = drivingWheel;
        g_stereoMobileColor[ws] = g_pendingMobileColor;
        g_stereoThrowableTrajectory[ws][0] = g_throwableTrajectory[0];
        g_stereoThrowableTrajectory[ws][1] = g_throwableTrajectory[1];
        laserHand = g_weaponHand.load(std::memory_order_relaxed);
        if (laserHand >= 0 && laserHand < 2) {
            laserPose = g_stereoHandPose[ws][laserHand];
            laserTwoHand = g_stereoTwoHandVisualState[ws];
        }
    }
    // The model-bound ray consults the PhysicalWeapon state. Keep that work
    // outside g_headPoseMutex: PhysicalWeapon::Update publishes marker data in
    // the opposite direction (physical-state lock -> head-pose lock), so nesting
    // the locks here would deadlock the game and compositor threads.
    const LaserRay laserRay = laserHand >= 0 && laserHand < 2
        ? BuildCalibratedLaserRay(laserPose, laserHand, laserTwoHand)
        : LaserRay{};
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        g_stereoLaserRay[ws] = laserRay;
    }
    // Publish only after texture, depth and pose for this ring slot are complete.
    g_stereoPublishedNs[ws].store(MonotonicNowNs(), std::memory_order_relaxed);
    g_stereoGeneration[ws].store(seq, std::memory_order_release);
    g_stereoSeq.store(seq, std::memory_order_release);
}

bool StereoEnsure(int width, int height) {
    // Without a shared XR context the consumer cannot sample these textures, so
    // producing them is worse than useless (it would black the eyes). Bail so the
    // game thread renders a single mono frame that the consumer can still show.
    if (!g_stereoShared) return false;
    if (g_eyeReady && width == g_eyeW && height == g_eyeH) {
        return true;
    }
    static bool loggedVer = false;
    if (!loggedVer) {
        const GLubyte* ver = glGetString(GL_VERSION);
        LOGI("[stereo] engine GL context: %s", ver ? (const char*)ver : "?");
        loggedVer = true;
    }
    // Tear down any previous set on a resize.
    for (auto& buf : g_eyeBuf) {
        for (int e = 0; e < 2; ++e) {
            if (buf.fbo[e]) glDeleteFramebuffers(1, &buf.fbo[e]);
            if (buf.tex[e]) glDeleteTextures(1, &buf.tex[e]);
            buf.fbo[e] = buf.tex[e] = 0;
        }
        if (buf.fence) { glDeleteSync(buf.fence); buf.fence = nullptr; }
    }
    g_eyeReady = false;
    {
        std::lock_guard<std::mutex> lock(g_eyePublishMutex);
        g_eyePublished = -1;
        g_eyeConsuming = -1;
    }

    for (auto& buf : g_eyeBuf) {
        for (int e = 0; e < 2; ++e) {
            glGenTextures(1, &buf.tex[e]);
            glBindTexture(GL_TEXTURE_2D, buf.tex[e]);
            // Unsized GL_RGBA internal format: valid on both GLES2 and GLES3, so
            // this works whichever version the engine's context is.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const GLenum texErr = glGetError();
            if (texErr != GL_NO_ERROR) {
                LOGE("[stereo] glTexImage2D %dx%d err 0x%x", width, height, texErr);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenFramebuffers(1, &buf.fbo[e]);
            glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo[e]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, buf.tex[e], 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                LOGE("[stereo] eye FBO incomplete: 0x%x", status);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glBindTexture(GL_TEXTURE_2D, 0);
                return false;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_eyeW = width;
    g_eyeH = height;
    g_eyeReady = true;
    LOGI("[stereo] eye textures ready %dx%d (double-buffered)", width, height);
    return true;
}

int StereoBeginFrame() {
    if (!g_eyeReady) return -1;
    int published, consuming;
    {
        std::lock_guard<std::mutex> lock(g_eyePublishMutex);
        published = g_eyePublished;
        consuming = g_eyeConsuming;
    }
    // Any buffer that is neither on display nor held by the consumer. With three
    // buffers there is always one, so its fence can never be the one the consumer
    // is waiting on.
    for (int i = 0; i < kEyeBuffers; ++i) {
        if (i != published && i != consuming) return i;
    }
    return -1;
}

unsigned int StereoEyeTex(int idx, int e) {
    if (idx < 0 || idx >= kEyeBuffers || e < 0 || e > 1) return 0;
    return g_eyeBuf[idx].tex[e];
}
unsigned int StereoEyeFbo(int idx, int e) {
    if (idx < 0 || idx >= kEyeBuffers || e < 0 || e > 1) return 0;
    return g_eyeBuf[idx].fbo[e];
}

void StereoCommit(int idx) {
    if (idx < 0 || idx >= kEyeBuffers) return;
    // idx came from StereoBeginFrame, so it is neither published nor consuming:
    // the consumer never holds this buffer's fence, so replacing it here is safe.
    if (g_eyeBuf[idx].fence) glDeleteSync(g_eyeBuf[idx].fence);
    g_eyeBuf[idx].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();   // ensure the fence and the eye renders are in the GPU pipe
    std::lock_guard<std::mutex> lock(g_eyePublishMutex);
    g_eyePublished = idx;
}

void SetStereoRenderFovX(float degrees) {
    if (degrees > 20.0f && degrees < 160.0f) {
        g_stereoRenderFovXDeg.store(degrees, std::memory_order_relaxed);
    }
}

void SetRenderHeadPose(const float position[3], const float orientation[4]) {
    std::lock_guard<std::mutex> lock(g_headPoseMutex);
    g_renderHeadPose.position    = {position[0], position[1], position[2]};
    g_renderHeadPose.orientation = {orientation[0], orientation[1], orientation[2], orientation[3]};
    g_renderHeadPoseValid = true;
}

void SetGameFrame(unsigned int texture, const float* transform) {
    s.gameTexture = texture;
    if (transform != nullptr) {
        std::memcpy(s.gameTransform, transform, sizeof(s.gameTransform));
    }
}

bool Initialize(JavaVM* vm, jobject activity) {
    if (s.instance != XR_NULL_HANDLE) {
        return true;
    }

    LOGI("SAVR version %s", kModVersion);
    SetupHudTextRenderer(vm,activity);

    // On Android the loader has to be handed the VM and Activity before any
    // other call, otherwise it cannot find the runtime at all.
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    if (!Check(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR)),
               "xrGetInstanceProcAddr(xrInitializeLoaderKHR)")) {
        return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInit{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInit.applicationVM       = vm;
    loaderInit.applicationContext  = activity;
    if (!Check(xrInitializeLoaderKHR(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&loaderInit)),
               "xrInitializeLoaderKHR")) {
        return false;
    }

    // Enumerate what the runtime offers so we only enable optional extensions it
    // actually has (enabling a missing one makes xrCreateInstance fail = black VR).
    uint32_t availCount = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(
            nullptr, 0, &availCount, nullptr),
            "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }
    std::vector<XrExtensionProperties> avail(availCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (availCount && !Check(xrEnumerateInstanceExtensionProperties(
            nullptr, availCount, &availCount, avail.data()),
            "xrEnumerateInstanceExtensionProperties(list)")) {
        return false;
    }
    auto hasExt = [&](const char* n) {
        for (const auto& e : avail) if (std::strcmp(e.extensionName, n) == 0) return true;
        return false;
    };

    std::vector<const char*> extensions = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };
    // CPU/GPU clock boost + thread scheduling. The port is CPU-bound (RenderScene
    // recorded twice); a clock hint alone barely helped because the heavy GameThread
    // can sit on a little core — registering it as the app-main thread pins it to a
    // big core, which is what actually lifts the frame rate (matches the VC build).
    g_hasPerfExt = hasExt(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (g_hasPerfExt) extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    g_hasThreadExt = hasExt(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    if (g_hasThreadExt) extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    g_hasDisplayRefreshExt = hasExt(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (g_hasDisplayRefreshExt)
        extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    g_hasPerformanceMetricsExt = hasExt(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
    if (g_hasPerformanceMetricsExt)
        extensions.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
    LOGI("[perf.init] xr_ext perf_settings=%d thread_settings=%d refresh=%d meta_metrics=%d",
         g_hasPerfExt ? 1 : 0, g_hasThreadExt ? 1 : 0,
         g_hasDisplayRefreshExt ? 1 : 0,
         g_hasPerformanceMetricsExt ? 1 : 0);
    // Capability evidence only. Do not enable foveation here: GTA renders the
    // expensive world into intermediate RenderWare eye textures, so XR-swapchain
    // foveation would currently optimize only the final blit.
    LOGI("[gpu.caps] xr_fb foveation=%d update_state=%d config=%d gles_update=%d",
         hasExt(XR_FB_FOVEATION_EXTENSION_NAME) ? 1 : 0,
         hasExt(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) ? 1 : 0,
         hasExt(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) ? 1 : 0,
         hasExt(XR_FB_SWAPCHAIN_UPDATE_STATE_OPENGL_ES_EXTENSION_NAME) ? 1 : 0);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM       = vm;
    androidInfo.applicationActivity = activity;

    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.next                  = &androidInfo;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.enabledExtensionNames = extensions.data();
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    std::strcpy(create.applicationInfo.applicationName, "GTA SA VR");
    std::strcpy(create.applicationInfo.engineName, "RenderWare");

    if (!Check(xrCreateInstance(&create, &s.instance), "xrCreateInstance")) {
        return false;
    }
    ResolveDiagnosticExtensions();

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (Check(xrGetInstanceProperties(s.instance, &properties), "xrGetInstanceProperties")) {
        LOGI("runtime: %s %u.%u.%u", properties.runtimeName,
             XR_VERSION_MAJOR(properties.runtimeVersion),
             XR_VERSION_MINOR(properties.runtimeVersion),
             XR_VERSION_PATCH(properties.runtimeVersion));
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(s.instance, &systemInfo, &s.systemId), "xrGetSystem")) {
        return false;
    }

    if (!EnumerateViews()) {
        return false;
    }

    LOGI("OpenXR instance and system ready, eye %ux%u",
         s.configViews[0].recommendedImageRectWidth,
         s.configViews[0].recommendedImageRectHeight);
    return true;
}

bool CreateSession() {
    if (s.session != XR_NULL_HANDLE) {
        return true;
    }
    if (s.instance == XR_NULL_HANDLE) {
        LOGE("CreateSession before Initialize");
        return false;
    }

    // Required by the spec before session creation, and Meta's runtime does
    // enforce it: skipping this call makes xrCreateSession fail outright.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!GetProc("xrGetOpenGLESGraphicsRequirementsKHR", getRequirements)) {
        return false;
    }
    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!Check(getRequirements(s.instance, s.systemId, &requirements),
               "xrGetOpenGLESGraphicsRequirementsKHR")) {
        return false;
    }

    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
        LOGE("no current EGL context on this thread - session must be created on the render thread");
        return false;
    }
    EGLConfig config = ConfigOfContext(display, context);
    if (config == nullptr) {
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = display;
    binding.config  = config;
    binding.context = context;

    XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
    create.next     = &binding;
    create.systemId = s.systemId;
    if (!Check(xrCreateSession(s.instance, &create, &s.session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    if (!Check(xrCreateReferenceSpace(s.session, &spaceInfo, &s.space), "xrCreateReferenceSpace")) {
        return false;
    }

    // A head-locked space for the in-world screen. Presenting the game frame on a
    // quad rigidly attached to the head means it never trembles during head
    // rotation (unlike a projection layer, whose reprojection reference frame does
    // not match the game camera); the view simply lags smoothly.
    XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    if (!Check(xrCreateReferenceSpace(s.session, &viewInfo, &s.viewSpace), "xrCreateReferenceSpace(view)")) {
        return false;
    }

    if (!CreateSwapchains()) {
        return false;
    }

    glGenFramebuffers(1, &s.framebuffer);
    glGenFramebuffers(1, &s.eyeReadFramebuffer);
    if (FxaaRequested()) BuildFxaaProgram();
    LOGI("[fxaa] startup requested=%d ready=%d errors=%d",
         FxaaRequested() ? 1 : 0,
         s.fxaaProgram != 0 && !g_fxaaRuntimeFailed ? 1 : 0,
         g_fxaaErrorCount);
    CreateInput();
    InitializeSessionDiagnostics();
    LOGI("OpenXR session created on EGL context %p", context);
    return true;
}

// ---------------------------------------------------------------------------
// Head-locked FPS/debug panel — a faithful port of the Vice City VR overlay:
// a 512x512 RGBA panel with a 5x7 software font, toggled by both grips + A,
// presented as a head-locked quad. No font asset or shader needed.
// ---------------------------------------------------------------------------
unsigned char g_panelPixels[kPanelW * kPanelH * 4];
std::uint64_t g_profilerPanelRevision = ~std::uint64_t{0};
std::vector<std::uint64_t> g_debugImageRevisions;
std::vector<unsigned int> g_debugImageNames;
XrSwapchain g_debugCacheSwapchain = XR_NULL_HANDLE;

struct PanelGlyph { char c; unsigned char rows[7]; };
const PanelGlyph kPanelFont[] = {
    {' ', {0,0,0,0,0,0,0}},        {':', {0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5', {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
    {'6', {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}}, {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9', {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}}, {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x19,0x15,0x13,0x13,0x11}}, {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}}, {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'J', {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}}, {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}, {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},  {'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'<', {0x02,0x04,0x08,0x10,0x08,0x04,0x02}},  {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
};

const unsigned char* PanelGlyphRows(char c) {
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    for (const PanelGlyph& g : kPanelFont) if (g.c == c) return g.rows;
    return kPanelFont[0].rows;   // space
}

void PanelPixel(int x, int y, unsigned char r, unsigned char gr, unsigned char b, unsigned char a) {
    if (x < 0 || y < 0 || x >= kPanelW || y >= kPanelH) return;
    // Store bottom-up: the compositor samples the GL swapchain texture with v=0 at
    // the bottom, so flipping here keeps the text upright (y is top-down here).
    const int fy = kPanelH - 1 - y;
    unsigned char* p = &g_panelPixels[(fy * kPanelW + x) * 4];
    p[0] = r; p[1] = gr; p[2] = b; p[3] = a;
}

void PanelText(const char* text, int centreX, int y, int scale,
               unsigned char r, unsigned char gr, unsigned char b) {
    const int advance = scale * 6;
    int x = centreX - static_cast<int>(std::strlen(text)) * advance / 2;
    for (const char* ch = text; *ch; ++ch, x += advance) {
        const unsigned char* rows = PanelGlyphRows(*ch);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (rows[row] & (1 << (4 - col)))
                    for (int py = 0; py < scale; ++py)
                        for (int px = 0; px < scale; ++px)
                            PanelPixel(x + col * scale + px, y + row * scale + py, r, gr, b, 255);
    }
}

// Like PanelText, but drops the glyph scale until the line fits the panel
// width (long strings such as URLs would otherwise run off both edges).
void PanelTextFit(const char* text, int centreX, int y, int maxScale,
                  unsigned char r, unsigned char gr, unsigned char b) {
    const int len = static_cast<int>(std::strlen(text));
    int scale = maxScale;
    while (scale > 1 && len * 6 * scale > kPanelW - 8) --scale;
    PanelText(text, centreX, y, scale, r, gr, b);
}

// Rebuild the profiler from the same approximately-1Hz aggregates written to the
// CSV. Stable windows are much easier to read while driving than raw frame-to-
// frame values, and make the headset display agree with the later log analysis.
void BuildPanel(const perf::DebugStatsSnapshot& stats) {
    for (int i = 0; i < kPanelW * kPanelH; ++i) {
        g_panelPixels[i * 4 + 0] = 0; g_panelPixels[i * 4 + 1] = 0;
        g_panelPixels[i * 4 + 2] = 0; g_panelPixels[i * 4 + 3] = 210;
    }
    char line[160];
    const int cx = kPanelW / 2;
    PanelText("PERFORMANCE", cx, 8, 3, 255, 230, 64);

    if (!stats.gameValid) {
        const int fps = g_fpsValue.load(std::memory_order_relaxed);
        std::snprintf(line, sizeof(line), "GAME FPS %d", fps);
        PanelText(line, cx, 54, 3, 255, 230, 64);
        PanelText("WAITING FOR 1HZ SAMPLE", cx, 104, 2, 150, 190, 210);
        PanelText("GRIPS+A CLOSE", cx, kPanelH - 24, 2, 150, 185, 150);
        return;
    }

    const int targetFps = stats.frameLimit > 0 ? stats.frameLimit : 60;
    const double cpuBudget = 1000.0 / static_cast<double>(targetFps);
    const double cpuLoad = cpuBudget > 0.0
        ? std::clamp(100.0 * stats.renderCpuMs / cpuBudget, 0.0, 999.0) : 0.0;
    const double gpuLoad = stats.runtimeGpuValid && stats.budgetMs > 0.0
        ? std::clamp(100.0 * stats.runtimeGpuMs / stats.budgetMs, 0.0, 999.0) : 0.0;
    const bool diagnosticFault = stats.dedupeFaults > 0 ||
                                 stats.cullAttributionFaults > 0 ||
                                 stats.modelDrawRestoreFaults > 0 ||
                                 stats.endFailures > 0 || stats.ringRaces > 0 ||
                                 stats.fxaaErrors > 0 || stats.fxaaFallbacks > 0 ||
                                 stats.renderQueueFinishPendingFaults > 0 ||
                                 stats.renderQueueFinishPendingDepthMax > 1 ||
                                 (stats.fxaaRequested && !stats.fxaaActive);
    const double dominantLoad = std::max(cpuLoad, gpuLoad);
    const bool loadCritical = diagnosticFault || dominantLoad >= 90.0;
    const unsigned char loadR = loadCritical ? 255 : (dominantLoad >= 75.0 ? 255 : 120);
    const unsigned char loadG = loadCritical ? 90  : (dominantLoad >= 75.0 ? 210 : 255);
    const unsigned char loadB = loadCritical ? 90  : (dominantLoad >= 75.0 ? 80  : 160);
    const auto hudValue = [](double value) {
        return std::clamp(value, -999.0, 9999.0);
    };
    const auto hudCount = [](int value) {
        return std::clamp(value, -999, 9999);
    };
    const auto hudShort = [](double value) {
        return std::clamp(value, 0.0, 999.0);
    };
    const traffic_census::Snapshot trafficCensus =
        traffic_census::GetSnapshot();
    static bool censusRateBaseline = false;
    static std::uint64_t previousCensusBirths = 0;
    static std::uint64_t previousCensusDeaths = 0;
    std::uint64_t censusBirthsWindow = 0;
    std::uint64_t censusDeathsWindow = 0;
    if (trafficCensus.valid) {
        if (censusRateBaseline &&
            trafficCensus.birthsTotal >= previousCensusBirths &&
            trafficCensus.deathsTotal >= previousCensusDeaths) {
            censusBirthsWindow =
                trafficCensus.birthsTotal - previousCensusBirths;
            censusDeathsWindow =
                trafficCensus.deathsTotal - previousCensusDeaths;
        }
        previousCensusBirths = trafficCensus.birthsTotal;
        previousCensusDeaths = trafficCensus.deathsTotal;
        censusRateBaseline = true;
    } else {
        censusRateBaseline = false;
    }

    std::snprintf(line, sizeof(line), "GAME %.1F/%d  XR %.1F",
                  stats.renderedHz, targetFps,
                  stats.displayValid ? stats.displayHz : 0.0);
    PanelText(line, cx, 40, 2, 255, 230, 64);
    std::snprintf(line, sizeof(line), "LOAD CPU %.0F GPU %.0F",
                  cpuLoad, gpuLoad);
    PanelText(line, cx, 70, 2, loadR, loadG, loadB);
    std::snprintf(line, sizeof(line), "ENGINE W/C %.1F/%.1F WAIT %.1F",
                  stats.renderWallMs, stats.renderCpuMs, stats.renderBlockedMs);
    PanelText(line, cx, 100, 2, 255, 150, 150);
    std::snprintf(line, sizeof(line), "REC W/C %.1F/%.1F ECPU %.1F/%.1F",
                  stats.recordWallMs, stats.recordCpuMs,
                  stats.sceneLeftCpuMs, stats.sceneRightCpuMs);
    PanelText(line, cx, 130, 2, 255, 170, 130);
    // B/MAIN/A are additive parts of engine_pre; STOCK is a subset of MAIN.
    // Use the narrow inter-row gaps at scale 1 so the established 512x512
    // layout and every menu surface remain unchanged.
    if (stats.enginePreBreakdownValid) {
        std::snprintf(line, sizeof(line),
                      "EP W B/MAIN/A/RES %.2F/%.2F/%.2F/%.2F STOCK %.2F",
                      stats.enginePreBeforeScanWallMs,
                      stats.enginePreMainScanWallMs,
                      stats.enginePreAfterScanWallMs,
                      stats.enginePreBreakdownResidualWallMs,
                      stats.enginePreStockScanWallMs);
    } else {
        std::snprintf(line, sizeof(line), "EP BREAKDOWN WAITING F%d",
                      stats.enginePreBreakdownFaults);
    }
    PanelText(line, cx, 145, 1, 140, 220, 255);
    // P/S/T/O = engine before the stereo hook, stereo setup, stereo hook tail,
    // and the engine work after the hook. These four rows are the old opaque
    // `engine_other`; record stays on the line above.
    std::snprintf(line, sizeof(line), "W PRE/SET/TAIL/POST %.1F/%.1F/%.1F/%.1F",
                  stats.enginePreWallMs, stats.stereoPrepareWallMs,
                  stats.stereoTailWallMs, stats.enginePostWallMs);
    PanelText(line, cx, 160, 2, 120, 220, 255);
    if (stats.enginePreBreakdownValid) {
        std::snprintf(line, sizeof(line),
                      "EP C B/MAIN/A/RES %.2F/%.2F/%.2F/%.2F STOCK %.2F",
                      stats.enginePreBeforeScanCpuMs,
                      stats.enginePreMainScanCpuMs,
                      stats.enginePreAfterScanCpuMs,
                      stats.enginePreBreakdownResidualCpuMs,
                      stats.enginePreStockScanCpuMs);
    } else {
        std::snprintf(line, sizeof(line), "EP CPU BREAKDOWN WAITING F%d",
                      stats.enginePreBreakdownFaults);
    }
    PanelText(line, cx, 175, 1, 120, 205, 255);
    std::snprintf(line, sizeof(line), "C PRE/SET/TAIL/POST %.1F/%.1F/%.1F/%.1F",
                  stats.enginePreCpuMs, stats.stereoPrepareCpuMs,
                  stats.stereoTailCpuMs, stats.enginePostCpuMs);
    PanelText(line, cx, 190, 2, 120, 200, 255);
    if (stats.renderQueueWaitHookActive &&
        stats.enginePreBreakdownValid) {
        std::snprintf(line, sizeof(line),
                      "RQ BLK FIN/FLUSH %.2F/%.2F PH B/M/A %.2F/%.2F/%.2F",
                      stats.renderQueueFinishBlockedMs,
                      stats.renderQueueFlushBlockedMs,
                      stats.renderQueueWaitBeforeScanWallMs,
                      stats.renderQueueWaitMainScanWallMs,
                      stats.renderQueueWaitAfterScanWallMs);
    } else if (!stats.renderQueueWaitHookActive) {
        std::snprintf(line, sizeof(line), "RQ WAIT HOOK INACTIVE X%d",
                      stats.renderQueueWaitClassificationFaults);
    } else {
        std::snprintf(line, sizeof(line), "RQ DATA INVALID F%d X%d",
                      stats.enginePreBreakdownFaults,
                      stats.renderQueueWaitClassificationFaults);
    }
    PanelText(line, cx, 205, 1, 255, 190, 120);
    std::snprintf(
        line, sizeof(line),
        "DEF A%d Q/D/R/F %.1F/%.1F/%.1F/%.1F O%.2F "
        "W/C/B/M %.2F/%.2F/%.2F/%.2F P%d X%d",
        stats.renderQueueFinishDeferActive ? 1 : 0,
        stats.renderQueueFinishRequestCalls,
        stats.renderQueueFinishDeferredCalls,
        stats.renderQueueFinishDrainCalls,
        stats.renderQueueFinishFallbackCalls,
        stats.renderQueueFinishOverlapWallMs,
        stats.renderQueueFinishDrainWallMs,
        stats.renderQueueFinishDrainCpuMs,
        stats.renderQueueFinishDrainBlockedMs,
        stats.renderQueueFinishDrainMaxWallMs,
        stats.renderQueueFinishPendingDepthMax,
        stats.renderQueueFinishPendingFaults);
    PanelText(line, cx, 213, 1,
              stats.renderQueueFinishPendingFaults > 0 ? 255 : 150,
              stats.renderQueueFinishPendingFaults > 0 ? 90 : 220,
              stats.renderQueueFinishPendingFaults > 0 ? 90 : 180);
    if (stats.runtimeGpuValid) {
        std::snprintf(line, sizeof(line),
                      "GPU %.1F/%.1F AA%d D%d F%d E%d C%.2F",
                      stats.runtimeGpuMs, stats.budgetMs,
                      stats.fxaaActive ? 1 : 0,
                      hudCount(stats.fxaaDraws), hudCount(stats.fxaaFallbacks),
                      hudCount(stats.fxaaErrors), stats.fxaaSubmitWallMs);
    } else {
        std::snprintf(line, sizeof(line), "GPU N/A AA%d D%d F%d E%d C%.2F",
                      stats.fxaaActive ? 1 : 0,
                      hudCount(stats.fxaaDraws), hudCount(stats.fxaaFallbacks),
                      hudCount(stats.fxaaErrors), stats.fxaaSubmitWallMs);
    }
    const bool fxaaFault = stats.fxaaErrors > 0 || stats.fxaaFallbacks > 0 ||
                           (stats.fxaaRequested && !stats.fxaaActive);
    PanelText(line, cx, 220, 2, fxaaFault ? 255 : 180,
              fxaaFault ? 100 : 170, fxaaFault ? 100 : 255);
    if (stats.lodWitnessValid) {
        std::snprintf(line, sizeof(line), "LIST E/L/S %.0F/%.0F/%.0F",
                      hudValue(stats.entities), hudValue(stats.visibleLods),
                      hudValue(stats.visibleSuperLods));
    } else {
        std::snprintf(line, sizeof(line), "LIST WITNESS WAITING");
    }
    PanelText(line, cx, 244, 2, 120, 255, 160);
    if (stats.lodWitnessValid) {
        std::snprintf(line, sizeof(line), "STREAM Q/P %.0F/%.0F MIB %.0F/%.0F",
                      hudValue(stats.streamingRequests),
                      hudValue(stats.streamingPriorityRequests),
                      hudValue(stats.streamingMemoryUsedMiB),
                      hudValue(stats.streamingMemoryAvailableMiB));
    } else {
        std::snprintf(line, sizeof(line), "STREAM WITNESS WAITING");
    }
    PanelText(line, cx, 263, 2, 120, 255, 160);
    if (stats.nearbyScanActive) {
        std::snprintf(line, sizeof(line),
                      "NEAR %.0FM S%.0F +E%.0F W/C %.2F/%.2F",
                      hudValue(stats.nearbyScanRadiusM),
                      hudValue(stats.nearbyScanSectors),
                      hudValue(stats.nearbyScanVisibleAdded),
                      stats.nearbyScanWallMs, stats.nearbyScanCpuMs);
        PanelText(line, cx, 282, 2, 120, 255, 160);
    } else if (stats.cullAttributionValid) {
        std::snprintf(line, sizeof(line),
                      "CULL D %.0F/%.0F/%.0F S%.0F %.0F/%.0F/%.0F",
                      hudValue(stats.dynamicMatrixCalls),
                      hudValue(stats.dynamicTests),
                      hudValue(stats.dynamicFallbackCulled),
                      hudValue(stats.staticSafetyRadiusM),
                      hudValue(stats.staticSafetyTests),
                      hudValue(stats.staticSafetyStockVisible),
                      hudValue(stats.staticSafetyAccepts));
        PanelText(line, cx, 282, 2, 120, 255, 160);
    } else {
        PanelText("DYN ATTR WAITING FOR STEREO", cx, 282, 2, 150, 190, 210);
    }
    if (trafficCensus.valid) {
        const int births = static_cast<int>(std::min<std::uint64_t>(
            censusBirthsWindow, 9999u));
        const int deaths = static_cast<int>(std::min<std::uint64_t>(
            censusDeathsWindow, 9999u));
        std::snprintf(line, sizeof(line),
                      "POOL V%d R%d M%d P%d B%d D%d",
                      static_cast<int>(trafficCensus.live),
                      static_cast<int>(trafficCensus.random),
                      static_cast<int>(trafficCensus.mission),
                      static_cast<int>(trafficCensus.parked), births, deaths);
        PanelText(line, cx, 301, 2, 255, 210, 120);
    } else if (stats.cullAttributionValid) {
        std::snprintf(line, sizeof(line), "VIS WIDE/FALL %.0F/%.0F",
                      hudValue(stats.dynamicWidenAccepts),
                      hudValue(stats.dynamicFallbackVisible));
        PanelText(line, cx, 301, 2, 120, 255, 160);
    } else {
        PanelText("POOL CENSUS WAITING", cx, 301, 2, 150, 190, 210);
    }
    if (stats.carPopulationValid) {
        std::snprintf(line, sizeof(line),
                      "CAR R/L/M/P %.0F/%.0F/%.0F/%.0F G%d:%d A%.0F B%.0F W%.0F",
                      hudValue(stats.carRandom), hudValue(stats.carLaw),
                      hudValue(stats.carMission), hudValue(stats.carParked),
                      stats.ambientCarGateActive ? 1 : 0,
                      hudCount(stats.ambientCarTarget),
                      hudShort(stats.ambientCarAttempts),
                      hudShort(stats.ambientCarBlocked),
                      hudShort(stats.ambientCarWantedPasses));
    } else {
        std::snprintf(line, sizeof(line), "CAR POP WITNESS WAITING");
    }
    PanelText(line, cx, 320, 2, 255, 210, 120);
    if (stats.pedPopulationValid) {
        std::snprintf(line, sizeof(line),
                      "PED T/C/G/M/P %.0F/%.0F/%.0F/%.0F/%.0F "
                      "G%d A/S/B %.0F/%.0F/%.0F",
                      hudValue(stats.pedTotal), hudValue(stats.pedCiv),
                      hudValue(stats.pedGang), hudValue(stats.pedMission),
                      hudValue(stats.pedCarPassenger),
                      hudCount(stats.pedAmbientCap),
                      hudShort(stats.pedAmbientAttempts),
                      hudShort(stats.pedAmbientSuccesses),
                      hudShort(stats.pedAmbientBlocked));
    } else {
        std::snprintf(line, sizeof(line), "PED POP WITNESS WAITING");
    }
    PanelText(line, cx, 339, 2, 255, 210, 120);
    if (!stats.lodHandoffHookActive) {
        std::snprintf(line, sizeof(line), "LINK0 HOOK INACTIVE");
    } else if (stats.handoffModelId >= 0 && stats.handoffResult >= 0) {
        std::snprintf(line, sizeof(line),
                      "LINK T/N %.0F/%.0F BD %.0F/%.0F MD%d M%d R%d",
                      hudValue(stats.linkedEntityTests),
                      hudValue(stats.linkedNearThreshold),
                      hudShort(stats.buildingDetailTests),
                      hudShort(stats.buildingDetailOverrides),
                      hudCount(stats.modelDrawRestoreFaults),
                      hudCount(stats.handoffModelId), stats.handoffResult);
    } else {
        std::snprintf(line, sizeof(line),
                      "LINK T/N %.0F/%.0F BD %.0F/%.0F MD%d",
                      hudValue(stats.linkedEntityTests),
                      hudValue(stats.linkedNearThreshold),
                      hudShort(stats.buildingDetailTests),
                      hudShort(stats.buildingDetailOverrides),
                      hudCount(stats.modelDrawRestoreFaults));
    }
    PanelText(line, cx, 358, 2, stats.lodHandoffHookActive ? 120 : 255,
              stats.lodHandoffHookActive ? 255 : 120, 180);
    if (stats.lodPrefetchActive) {
        std::snprintf(line, sizeof(line),
                      "PF X%.1F B%.0F S%.0F Q%.0F E%.0F P%.0F T%.0F A%.0F",
                      stats.lodPrefetchFactor,
                      hudShort(stats.prefetchBand),
                      hudShort(stats.prefetchStreamMe),
                      hudShort(stats.prefetchRequestCalls),
                      hudShort(stats.prefetchEnqueues),
                      hudShort(stats.prefetchAlreadyPending),
                      hudShort(stats.prefetchThrottled),
                      hudShort(stats.prefetchStateAnomalies));
    } else {
        std::snprintf(line, sizeof(line), "PREF REQUEST-ONLY INACTIVE X%.2F",
                      stats.lodPrefetchFactor);
    }
    PanelText(line, cx, 377, 2, stats.lodPrefetchActive ? 120 : 255,
              stats.lodPrefetchActive ? 255 : 120, 190);
    if (stats.vehicleNearValid) {
        std::snprintf(line, sizeof(line),
                      "CAR O/H %d/%d T%.0F C%.0F D/TH %.0F/%.0F %s",
                      stats.vehicleLodOverrideActive ? 1 : 0,
                      stats.vehicleLodSampleHookActive ? 1 : 0,
                      stats.vehicleLodTargetM,
                      hudShort(stats.vehicleSampleCalls),
                      stats.vehicleNearDistance, stats.vehicleNearThreshold,
                      stats.vehicleNearHigh ? "HI" : "LO");
    } else {
        std::snprintf(line, sizeof(line),
                      "CAR O/H %d/%d T%.0F C%.0F NO EDGE",
                      stats.vehicleLodOverrideActive ? 1 : 0,
                      stats.vehicleLodSampleHookActive ? 1 : 0,
                      stats.vehicleLodTargetM,
                      hudShort(stats.vehicleSampleCalls));
    }
    const bool vehicleControlsGood = stats.vehicleLodOverrideActive &&
                                     stats.vehicleLodSampleHookActive;
    PanelText(line, cx, 396, 2, vehicleControlsGood ? 120 : 255,
              vehicleControlsGood ? 255 : 140, 180);
    if (stats.propModelId >= 0 && stats.propResult >= 0) {
        const double propFadeOut = stats.propAppliedThreshold +
                                   22.0 * stats.lodScale;
        std::snprintf(line, sizeof(line),
                      "P%.0F W/T %.0F/%.0F M%d D%.0F E%.0F>%.0F O%.0F R%dL%dX%d",
                      stats.streetPropFloorM,
                      hudShort(stats.streetPropWrites),
                      hudShort(stats.streetPropTests),
                      hudCount(stats.propModelId), stats.propDistance,
                      stats.propStockThreshold, stats.propAppliedThreshold,
                      propFadeOut,
                      stats.propResult, stats.propLoaded ? 1 : 0,
                      stats.propTargeted ? 1 : 0);
    } else {
        std::snprintf(line, sizeof(line),
                      "P%.0F W/T %.0F/%.0F NO EDGE",
                      stats.streetPropFloorM,
                      hudShort(stats.streetPropWrites),
                      hudShort(stats.streetPropTests));
    }
    PanelText(line, cx, 415, 2,
              stats.streetPropFloorActive ? 120 : 180,
              stats.streetPropFloorActive ? 255 : 210, 180);
    std::snprintf(line, sizeof(line),
                  "GUARD A/S/L/RX %d/%d/%d/%d ERR D/A/M %d/%d/%d",
                  stats.alphaHookActive ? 1 : 0, stats.shadowHookActive ? 1 : 0,
                  stats.limiterActive ? 1 : 0, stats.limiterRxRestored ? 1 : 0,
                  hudCount(stats.dedupeFaults),
                  hudCount(stats.cullAttributionFaults),
                  hudCount(stats.modelDrawRestoreFaults));
    const bool guardsGood = stats.alphaHookActive && stats.shadowHookActive &&
                            stats.limiterActive && stats.limiterRxRestored;
    PanelText(line, cx, 434, 2, guardsGood ? 130 : 255,
              guardsGood ? 255 : 100, guardsGood ? 170 : 100);
    std::snprintf(line, sizeof(line),
                  "SYNC W/R/T %d/%d/%d %.2FMS REP%d",
                  hudCount(stats.stereoSyncWaits),
                  hudCount(stats.stereoSyncRescued),
                  hudCount(stats.stereoSyncTimeouts),
                  stats.stereoSyncWaitMs, hudCount(stats.repeated));
    const bool syncGood = stats.stereoSyncTimeouts == 0 &&
                          stats.repeated <= 1;
    PanelText(line, cx, 453, 2, syncGood ? 130 : 255,
              syncGood ? 255 : 180, syncGood ? 170 : 100);
    if (stats.lodWitnessValid) {
        std::snprintf(line, sizeof(line),
                      "S%.2F C%.2F G%.2F A%d D%.1F L%.0F H%d B%.0F",
                      stats.lodScale, stats.cameraLodMultiplier,
                      stats.cameraGenerationMultiplier,
                      vrcam::GetTrafficCarCap(),
                      vrcam::GetPedDensityScale(),
                      vrcam::GetPedLifecycleRetentionMeters(),
                      hudCount(static_cast<int>(
                          vrcam::GetPedLifecycleOverrideCount())),
                      vrcam::GetBuildingDetailFloorMeters());
    } else {
        std::snprintf(line, sizeof(line), "LOD WITNESS WAITING");
    }
    PanelText(line, cx, 472, 2, 170, 190, 220);
    PanelText("GRIPS+A CLOSE  1HZ SMOOTH", cx, kPanelH - 10,
              1, 150, 185, 150);
}

void PanelFillRect(int x0, int y0, int x1, int y1, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) PanelPixel(x, y, r, g, b, a);
}

// Draw the cheat list: a title, one row per cheat with the selected row
// highlighted, and a control hint at the bottom.
// Clear the panel to the standard translucent-black menu background.
void PanelClear() {
    for (int i = 0; i < kPanelW * kPanelH; ++i) {
        g_panelPixels[i * 4 + 0] = 0; g_panelPixels[i * 4 + 1] = 0;
        g_panelPixels[i * 4 + 2] = 0; g_panelPixels[i * 4 + 3] = 228;
    }
}

// Root VR menu: the list of sections (Vice City style). A enters a section, B
// closes. The stick moves the highlight.
// Vice City parity: the CONTROLS page. One layout row (DEFAULT / SWAPPED
// HANDS / CUSTOM) and one row per face button choosing the action it
// triggers on foot; vehicles always keep the shipped layout.
// Every control nuance that is NOT discoverable from the buttons themselves,
// one screen, no navigation: the manual players kept asking for.
// The Vice City port's welcome/about window: opens itself once on the first
// movement in gameplay, and stays reachable from the main menu afterwards.
void BuildAboutMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    const bool firstRun = g_aboutFirstRun.load(std::memory_order_relaxed);
    PanelTextFit(firstRun ? "WELCOME TO SAN ANDREAS VR"
                          : "ABOUT SAN ANDREAS VR",
                 cx, 22, 3, 100, 225, 255);
    char versionLine[64];
    std::snprintf(versionLine, sizeof(versionLine),
                  "VERSION %s ALPHA - NOT FOR SALE", kModVersionShown);
    PanelTextFit(versionLine, cx, 54, 2, 170, 190, 210);
    static const char* const kLines[] = {
        "VR MENU: HOLD BOTH GRIPS + MENU (OR Y)",
        "BUTTON REMAP AND TIPS: MENU > CONTROLS",
        "IMMERSIVE DRIVING: VEHICLE SETTINGS",
        "CALIBRATE WEAPONS IF A GRIP LOOKS OFF",
        "PROMPTS: TAP R2 YES  L2 NO  HOLD R2",
        "RECRUIT: LOOK AT GROVE PED + HOLD R2",
        "RECRUITING NEEDS RESPECT (OR CHEAT)",
        "L3+R3 RECENTERS THE VIEW",
        "ACTIVE DEVELOPMENT - EXPECT ROUGH EDGES",
        "DISCUSSION: FLAT2VR DISCORD",
        "discord.com/channels/747967102895390741/1540234546182750228",
        "PRESS ANY BUTTON TO CLOSE",
    };
    const int count = static_cast<int>(sizeof(kLines) / sizeof(kLines[0]));
    const int top = 92, rowH = 32;
    for (int i = 0; i < count; ++i) {
        const bool close = i == count - 1;
        const bool link = i == count - 2;
        PanelTextFit(kLines[i], cx, top + i * rowH, 2,
                     close ? 255 : (link ? 140 : 210),
                     close ? 205 : (link ? 200 : 225),
                     close ? 80 : (link ? 255 : 235));
    }
}

void BuildControlsTipsMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("CONTROL TIPS", cx, 16, 4, 100, 225, 255);
    static const char* const kTips[] = {
        "GRIPS+MENU (OR Y)  OPEN VR MENU",
        "Y ENTER/EXIT   X JUMP   A SPRINT",
        "B  FIRE HELD WEAPON IN VEHICLES / TANK",
        "R2 TAP  ANSWER PROMPTS YES   L2  NO",
        "R2 HOLD  TAP-AND-HOLD PROMPTS",
        "PLANES: PULL/PUSH YOKE = PITCH",
        "HYDRA NOZZLES + HELI YAW: RIGHT STICK",
        "HELI: LEFT STICK = ROLL + PITCH",
        "CLIMB BOOST: LEFT GRIP + RIGHT TRIGGER",
        "PARACHUTE: JUMP OUT, A = OPEN CANOPY",
        "SKYDIVE: HOLD RIGHT TRIGGER = DIVE",
        "STEER CANOPY WITH STICKS OR RISERS",
        "RECRUIT: LOOK AT GROVE PED + HOLD R2",
        "DISMISS GROUP: LOOK AWAY + HOLD R2",
        "RECRUITING NEEDS RESPECT (MISSIONS OR",
        "  THE MAX RESPECT CHEAT)",
        "HORN: PRESS PALM ON THE WHEEL HUB",
        "ANSWER PHONE: BOTH GRIPS + R2",
        "L3+R3 RECENTER   GRIPS+R3 CAMERA VIEW",
    };
    const int count = static_cast<int>(sizeof(kTips) / sizeof(kTips[0]));
    const int top = 48, rowH = 23;
    for (int i = 0; i < count; ++i)
        PanelText(kTips[i], cx, top + i * rowH, 2, 210, 225, 235);
    PanelText("A / B BACK", cx, kPanelH - 18, 2, 150, 185, 150);
}

void BuildControlsMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("CONTROLS", cx, 20, 5, 100, 225, 255);
    PanelTextFit("ON FOOT ONLY - VEHICLES KEEP DEFAULTS",
                 cx, 56, 2, 150, 185, 150);
    char rows[7][64];
    std::snprintf(rows[0], sizeof(rows[0]), "LAYOUT  < %s >",
                  locomotion::ControlsLayoutName());
    for (int srcRow = 0; srcRow < locomotion::BIND_SRC_COUNT; ++srcRow) {
        std::snprintf(rows[1 + srcRow], sizeof(rows[0]), "%s  < %s >",
                      locomotion::ButtonSourceName(srcRow),
                      locomotion::ButtonActionName(
                          locomotion::GetButtonBinding(srcRow)));
    }
    std::snprintf(rows[5], sizeof(rows[0]), "CONTROL TIPS");
    std::snprintf(rows[6], sizeof(rows[0]), "BACK");
    const int sel = g_controlsSel.load(std::memory_order_relaxed);
    const int top = 96, rowH = 50;
    for (int i = 0; i < 7; ++i) {
        const int y = top + i * rowH;
        if (i == sel) PanelFillRect(40, y - 6, kPanelW - 40, y + 30, 120, 40, 110, 235);
        const int c = (i == sel) ? 255 : 200;
        PanelText(rows[i], cx, y, 2, c, c, (i == sel) ? 120 : c);
    }
    PanelText("STICK SEL   L2/R2 CHANGE   B BACK", cx, kPanelH - 22, 2, 150, 185, 150);
}

void BuildMainMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("VR MENU", cx, 20, 5, 100, 225, 255);
    char versionLine[32];
    std::snprintf(versionLine, sizeof(versionLine), "VERSION %s", kModVersionShown);
    PanelText(versionLine, cx, 52, 2, 120, 190, 210);
    char handSkinRow[48];
    std::snprintf(handSkinRow, sizeof(handSkinRow), "HAND SKIN   < %s >",
                  appearance::HandSkinName());
    const char* kItems[] = {
        "WEAPON CALIBRATION",
        "HOLSTER CALIBRATION",
        "HOLSTER LOADOUT",
        "VEHICLE SETTINGS",
        "LOCOMOTION",
        "HUD",
        handSkinRow,
        "CHEATS",
        "GRAPHICS",
        "CONTROLS",
        "ABOUT",
        "CLOSE"
    };
    const int N   = 12;
    const int sel = g_mainSel.load(std::memory_order_relaxed);
    const int top = 82, rowH = 34;
    for (int i = 0; i < N; ++i) {
        const int y = top + i * rowH;
        if (i == sel) PanelFillRect(40, y - 6, kPanelW - 40, y + 30, 120, 40, 110, 235);
        const bool submenu=i==0||i==1||i==2||i==3||i==4||i==5||
                           i==7||i==8||i==9||i==10;
        if (submenu)
            PanelText(kItems[i],cx,y,2,i==sel?255:100,
                      i==sel?235:225,i==sel?120:255);
        else {
            const int c=(i==sel)?255:200;
            PanelText(kItems[i],cx,y,2,c,c,(i==sel)?120:c);
        }
    }
    PanelText("STICK SELECT   A ENTER   B CLOSE", cx, kPanelH - 22, 2, 150, 185, 150);
}

// Per-weapon holstered model pose. Row zero cycles owned weapons; the six values
// apply to that weapon type at every body point, exactly like the PC SA page.
void BuildHolsterCalibMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    const int sel = g_holsterCalibSel.load(std::memory_order_relaxed);
    int point = -1, slot = -1, type = 0;
    const bool havePreview = holster::GetCalibrationPreview(&point, &slot, &type);

    PanelText("HOLSTERED MODEL CALIBRATION", cx, 12, 3, 100, 225, 255);
    PanelText("ONE POSE PER WEAPON TYPE", cx, 44, 2, 150, 190, 210);

    constexpr int kRows = 8;
    const int top = 82, rowH = 46;
    for (int rowIndex = 0; rowIndex < kRows; ++rowIndex) {
        const int y = top + rowIndex * rowH;
        if (rowIndex == sel)
            PanelFillRect(18, y - 4, kPanelW - 18, y + 20, 120, 40, 110, 235);
        const int c = rowIndex == sel ? 255 : 200;
        char row[80];
        if (rowIndex == 0) {
            if (havePreview) {
                std::snprintf(row, sizeof(row), "WEAPON   < %s >",
                              calib::WeaponName(type));
            } else {
                std::snprintf(row, sizeof(row), "WEAPON   < NONE OWNED >");
            }
        } else if (rowIndex == 7) {
            std::snprintf(row, sizeof(row), "BACK");
        } else {
            const int field = rowIndex - 1;
            std::snprintf(row, sizeof(row), "%s   < %+.1f %s >",
                          calib::HolsterFieldLabel(field),
                          calib::HolsterDisplayValue(calib::GetHolsterField(type, field)),
                          calib::HolsterFieldUnit(field));
        }
        PanelText(row, cx, y, 2, c, c, rowIndex == sel ? 120 : c);
    }
    PanelText("A NEXT WEAPON   L2/R2 ADJUST   B BACK", cx, kPanelH - 18,
              2, 150, 185, 150);
}

// Seven fixed Vice City body points. The menu assigns an SA inventory category
// to each point; the centre chest row remains THROWABLE by design.
void BuildHolsterMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("HOLSTER LOADOUT", cx, 14, 4, 100, 225, 255);
    PanelText("OWNED + UNASSIGNED CATEGORIES ONLY", cx, 48, 2, 150, 190, 210);
    const int sel = g_holsterMenuSel.load(std::memory_order_relaxed);
    const int rows = holster::PointCount() + 4;
    const int top = 74, rowH = 38;
    for (int i = 0; i < rows; ++i) {
        const int y = top + i * rowH;
        if (i == sel) PanelFillRect(18, y - 4, kPanelW - 18, y + 20, 120, 40, 110, 235);
        const int c = (i == sel) ? 255 : 200;
        char row[72];
        if (i == holster::PointCount()) {
            std::snprintf(row, sizeof(row), "GRAB REACH   < %d CM >",
                          holster::GrabRadiusCm());
        } else if (i == holster::PointCount() + 1) {
            std::snprintf(row, sizeof(row), "GRIP LOCK   < %s >",
                          holster::GripLockEnabled() ? "ON" : "OFF");
        } else if (i == holster::PointCount() + 2) {
            std::snprintf(row, sizeof(row), "HOLSTER MARKERS   < %s >",
                          holster::GripMarkersEnabled() ? "ON" : "OFF");
        } else if (i == holster::PointCount() + 3) {
            std::snprintf(row, sizeof(row), "BACK");
        } else {
            const int slot = holster::PointSlot(i);
            std::snprintf(row, sizeof(row), "%s   < %s >%s",
                          holster::PointName(i), holster::SlotName(slot),
                          holster::IsPointFixed(i) ? "  [FIXED]" : "");
        }
        PanelText(row, cx, y, 2, c, c, (i == sel) ? 120 : c);
    }
    const char* hint = "L2/R2 CHOOSE (EMPTY CLEARS)   B BACK";
    if (sel >= holster::PointCount() && sel <= holster::PointCount() + 2)
        hint = "A/B BACK";
    else if (holster::IsPointFixed(sel))
        hint = "THROWABLE IS FIXED   B BACK";
    else if (holster::PointSlot(sel) < 0)
        hint = "L2/R2 CHOOSE OWNED WEAPON   B BACK";
    PanelText(hint, cx, kPanelH - 18, 2, 150, 185, 150);
}

// Graphics submenu: Quest eye-buffer scale, performance hints and distances.
void BuildGraphicsMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("GRAPHICS", cx, 20, 5, 100, 225, 255);
    const int sel = g_gfxSel.load(std::memory_order_relaxed);
    const int cpu = g_cpuPerfIdx.load(std::memory_order_relaxed);
    const int gpu = g_gpuPerfIdx.load(std::memory_order_relaxed);
    char rows[8][64];
    std::snprintf(rows[0], sizeof(rows[0]), "RENDER SCALE  < %d%% >",
                  vrcam::GetRenderScalePercent());
    std::snprintf(rows[1], sizeof(rows[1]), "CPU  < %s >", kPerfNames[cpu]);
    std::snprintf(rows[2], sizeof(rows[2]), "GPU  < %s >", kPerfNames[gpu]);
    std::snprintf(rows[3], sizeof(rows[3]), "NEON SIGNS  < %s >",
                  vrcam::AreNeonSignsEnabled() ? "ON" : "OFF");
    std::snprintf(rows[4], sizeof(rows[4]), "COLOR GRADING  < %s >",
                  vrcam::IsColorGradingEnabled() ? "ON" : "OFF");
    std::snprintf(rows[5], sizeof(rows[5]), "DRAW DISTANCES");
    std::snprintf(rows[6], sizeof(rows[6]), "RESET DEFAULTS");
    std::snprintf(rows[7], sizeof(rows[7]), "BACK");
    const int top = 82, rowH = 48;
    for (int i = 0; i < 8; ++i) {
        const int y = top + i * rowH;
        if (i == sel) PanelFillRect(40, y - 6, kPanelW - 40, y + 30, 120, 40, 110, 235);
        const int c = (i == sel) ? 255 : 200;
        PanelText(rows[i], cx, y, 2, c, c, (i == sel) ? 120 : c);
    }
    PanelText("STICK SEL   L2/R2 ADJUST   B BACK", cx, kPanelH - 22, 2, 150, 185, 150);
}

void BuildGraphicsDistanceMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("DRAW DISTANCES", cx, 13, 4, 100, 225, 255);
    PanelText("EFFECTIVE WORLD METRES", cx, 43, 2, 150, 190, 210);
    const int sel = g_gfxDistanceSel.load(std::memory_order_relaxed);
    const int settings = vrcam::GetGraphicsDistanceSettingCount();
    const int rows = settings + 1;
    const int top = 72;
    const int rowH = 30;
    for (int i = 0; i < rows; ++i) {
        char row[96]{};
        if (i == settings) {
            std::snprintf(row, sizeof(row), "BACK");
        } else {
            std::snprintf(row, sizeof(row), "%s  < %dM >%s",
                          vrcam::GetGraphicsDistanceSettingName(i),
                          vrcam::GetGraphicsDistanceSettingMeters(i),
                          vrcam::GraphicsDistanceSettingNeedsRestart(i)
                              ? "  [RESTART]" : "");
        }
        const int y = top + i * rowH;
        if (i == sel)
            PanelFillRect(22, y - 4, kPanelW - 22, y + 22,
                          120, 40, 110, 235);
        const int c = (i == sel) ? 255 : 200;
        PanelText(row, cx, y, 2, c, c, (i == sel) ? 120 : c);
    }
    PanelText("L2/R2 ADJUST   A/B BACK", cx, kPanelH - 18,
              2, 150, 185, 150);
}

void BuildDrivingMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("VEHICLE SETTINGS", cx, 12, 4, 100, 225, 255);
    PanelText("CATEGORY, DRIVING TYPE AND PER-MODEL SETUP", cx, 46, 2,
              150, 190, 210);
    const int sel = g_drivingSel.load(std::memory_order_relaxed);
    const int vehicleType=g_drivingVehicleType.load(std::memory_order_relaxed);
    const int rowCount=driving::GetMenuItemCount(vehicleType);
    char rows[18][96]{};
    bool available[18]{};
    const bool haveModel=driving::HasCurrentModelForType(vehicleType);
    for (int row=0;row<rowCount;++row) {
        const int item=driving::GetMenuItemForRow(vehicleType,row);
        available[row]=driving::IsMenuItemAvailable(vehicleType,item);
        switch(item) {
            case driving::MENU_VEHICLE_TYPE:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "VEHICLE TYPE   < %s >",driving::VehicleTypeName(vehicleType));
                break;
            case driving::MENU_DRIVING_TYPE:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "DRIVING TYPE   < %s >",
                    driving::GetModeNameForVehicleType(vehicleType));
                break;
            case driving::MENU_CAMERA_VIEW:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "CAMERA VIEW   < %s >",
                    driving::GetCameraViewName(vehicleType));
                break;
            case driving::MENU_YOKE_SENSITIVITY:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "YOKE SENSITIVITY   < %d%% >",
                    driving::GetYokeSensitivityPercent());
                break;
            case driving::MENU_BICYCLE_MODE:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "BICYCLE CONTROL   < %s >",
                    driving::GetBicycleImmersiveModeName());
                break;
            case driving::MENU_BIKE_ACCELERATOR:
                if (available[row])
                    std::snprintf(rows[row],sizeof(rows[row]),
                        "THIS BIKE ACCELERATOR   < %s >",
                        driving::GetCurrentBikeAcceleratorModeName());
                else
                    std::snprintf(rows[row],sizeof(rows[row]),
                        "THIS BIKE ACCELERATOR   < ENTER BIKE >");
                break;
            case driving::MENU_GLOBAL_FORWARD:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "ALL %sS SEAT FORWARD   < %+d CM >",
                    driving::VehicleTypeName(vehicleType),
                    driving::GetGlobalSeatForwardCm(vehicleType));
                break;
            case driving::MENU_GLOBAL_HEIGHT:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "ALL %sS SEAT HEIGHT   < %+d CM >",
                    driving::VehicleTypeName(vehicleType),
                    driving::GetGlobalSeatHeightCm(vehicleType));
                break;
            case driving::MENU_MODEL_FORWARD:
                if (haveModel)
                    std::snprintf(rows[row],sizeof(rows[row]),
                        "THIS MODEL FORWARD CORRECTION   < %+d CM >",
                        driving::GetCurrentModelSeatForwardCm(vehicleType));
                else
                    std::snprintf(rows[row],sizeof(rows[row]),
                        "THIS MODEL FORWARD   < ENTER %s >",
                        driving::VehicleTypeName(vehicleType));
                break;
            case driving::MENU_MODEL_HEIGHT:
                if (haveModel)
                    std::snprintf(rows[row],sizeof(rows[row]),
                        "THIS MODEL HEIGHT CORRECTION   < %+d CM >",
                        driving::GetCurrentModelSeatHeightCm(vehicleType));
                else
                    std::snprintf(rows[row],sizeof(rows[row]),
                        "THIS MODEL HEIGHT   < ENTER %s >",
                        driving::VehicleTypeName(vehicleType));
                break;
            case driving::MENU_CONTROL_CALIBRATION:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "CONTROL GRIP CALIBRATION   < %s >",
                    available[row]?"OPEN":"ENTER VEHICLE");
                break;
            case driving::MENU_HANDLE_HIGHLIGHTS:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "CONTROL GRIP HIGHLIGHTS   < %s >",
                    driving::AreHandleHighlightsEnabled()?"ON":"OFF");
                break;
            case driving::MENU_BIKE_HAND_TILT:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "GRABBED HAND TILT   < %s >",
                    driving::DoBikeHandsFollowTilt()?"FOLLOW BIKE":"CONTROLLER");
                break;
            case driving::MENU_LOCAL_HORIZON:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "LOCAL HORIZON   < %s >",
                    driving::IsBikeHorizonLocked()?"ON":"OFF");
                break;
            case driving::MENU_BIKE_VISUAL_LEAN:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "VISUAL BIKE LEAN   < %d%% >",
                    driving::GetBikeVisualLeanPercent());
                break;
            case driving::MENU_KEEP_RIDER_ON_FLIPS:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "KEEP RIDER ON FLIPS   < %s >",
                    driving::KeepRiderOnFlipsEnabled()?"ON":"OFF");
                break;
            case driving::MENU_WHEEL_VISIBLE:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "VIRTUAL STEERING WHEEL   < %s >",
                    driving::IsWheelVisible()?"VISIBLE":"HIDDEN");
                break;
            case driving::MENU_INTERIOR_GLASS:
                std::snprintf(rows[row],sizeof(rows[row]),
                    "INTERIOR WINDOW TINT   < %s >",
                    driving::IsInteriorGlassHidden()?"OFF":"ON");
                break;
            case driving::MENU_RESET:
                std::snprintf(rows[row],sizeof(rows[row]),"RESET %s %s PRESET",
                    driving::VehicleTypeName(vehicleType),
                    driving::GetModeNameForVehicleType(vehicleType));
                break;
            default:
                std::snprintf(rows[row],sizeof(rows[row]),"BACK TO SETTINGS");
                break;
        }
    }
    char activeLine[96];
    if (haveModel)
        std::snprintf(activeLine,sizeof(activeLine),"CURRENT: %s  (MODEL %d)",
            driving::GetActiveVehicleModelName(),driving::GetActiveVehicleModelId());
    else
        std::snprintf(activeLine,sizeof(activeLine),
            "CURRENT: NO %s - MODEL OPTIONS DISABLED",
            driving::VehicleTypeName(vehicleType));
    PanelText(activeLine,cx,68,2,haveModel?130:225,haveModel?225:165,
              haveModel?255:105);

    constexpr int kWin=10;
    const int shown=std::min(kWin,rowCount);
    int start=std::clamp(sel-kWin/2,0,std::max(0,rowCount-kWin));
    if (start>0) PanelText("- MORE -",cx,84,2,120,150,120);
    const int top=100,rowH=36;
    for (int w=0;w<shown;++w) {
        const int i = start + w;
        const int y = top + w * rowH;
        if (i == sel)
            PanelFillRect(20,y-5,kPanelW-20,y+27,120,40,110,235);
        const int item=driving::GetMenuItemForRow(vehicleType,i);
        const bool submenu=item==driving::MENU_CONTROL_CALIBRATION;
        if (submenu&&available[i])
            PanelText(rows[i],cx,y,2,i==sel?255:100,
                      i==sel?235:225,i==sel?120:255);
        else {
            const int c=!available[i]?105:(i==sel?255:205);
            PanelText(rows[i],cx,y,2,c,c,i==sel&&available[i]?120:c);
        }
    }
    if (start<rowCount-kWin)
        PanelText("- MORE -", cx, top + kWin * rowH, 2, 120, 150, 120);
    PanelText("STICK SELECT   L2/R2 CHANGE   A OPEN   B BACK",cx,kPanelH-18,
              2, 150, 185, 150);
}

void BuildDrivingCalibrationMenu() {
    PanelClear();
    const int cx=kPanelW/2;
    char heading[96];
    std::snprintf(heading,sizeof(heading),"CONTROLS - %s (MODEL %d)",
        driving::GetActiveVehicleModelName(),driving::GetActiveVehicleModelId());
    PanelText(heading,cx,14,3,100,225,255);
    PanelText("SAVED ONLY FOR THIS VEHICLE MODEL",cx,46,2,150,190,210);
    const int sel=g_drivingCalibSel.load(std::memory_order_relaxed);
    const int hand=g_drivingCalibHand.load(std::memory_order_relaxed);
    const int vehicleType=g_drivingVehicleType.load(std::memory_order_relaxed);
    const bool hasWheel=vehicleType!=driving::VEHICLE_BIKE;
    const int wheelRows=hasWheel?driving::WHEEL_CAL_FIELD_COUNT:0;
    const int rows=wheelRows+driving::CONTROL_FIELD_COUNT+2;
    char text[driving::WHEEL_CAL_FIELD_COUNT+
              driving::CONTROL_FIELD_COUNT+2][88]{};
    for (int field=0;field<wheelRows;++field) {
        const int value=driving::GetWheelCalibrationValue(field);
        if (field>=driving::WHEEL_CAL_PITCH)
            std::snprintf(text[field],sizeof(text[field]),
                "%s   < %+.1f DEG >",
                driving::WheelCalibrationFieldName(field),value/2.0f);
        else
            std::snprintf(text[field],sizeof(text[field]),
                "%s   < %+d CM >",
                driving::WheelCalibrationFieldName(field),value);
    }
    std::snprintf(text[wheelRows],sizeof(text[wheelRows]),
                  "EDIT GRIP   < %s >",hand==0?"LEFT":"RIGHT");
    for (int field=0;field<driving::CONTROL_FIELD_COUNT;++field) {
        const int value=driving::GetControlCalibrationValue(hand,field);
        char* line=text[wheelRows+field+1];
        if (field>=driving::CONTROL_ROT_X)
            std::snprintf(line,88,"%s   < %+.1f DEG >",
                driving::ControlCalibrationFieldName(field),value/2.0f);
        else
            std::snprintf(line,88,"%s   < %+.1f CM >",
                driving::ControlCalibrationFieldName(field),value/2.0f);
    }
    std::snprintf(text[rows-1],sizeof(text[rows-1]),"BACK TO VEHICLE SETTINGS");
    const int top=94;
    const int rowH=hasWheel?40:46;
    for (int row=0;row<rows;++row) {
        const int y=top+row*rowH;
        if (row==sel)
            PanelFillRect(24,y-6,kPanelW-24,y+27,120,40,110,235);
        const int c=row==sel?255:205;
        // WHEEL rows are the primary calibration (position/radius/tilt of the
        // wheel itself) — VC amber so they read apart from grip fine-tuning.
        if (row<wheelRows)
            PanelText(text[row],cx,y,2,255,row==sel?235:200,60);
        else
            PanelText(text[row],cx,y,2,c,c,row==sel?120:c);
    }
    PanelText("SELECT VALUE   L2 MINUS   R2 PLUS   A SWITCH HAND   B BACK",
              cx,kPanelH-18,2,150,185,150);
}

void BuildLocomotionMenu() {
    PanelClear();
    const int cx=kPanelW/2;
    PanelText("LOCOMOTION",cx,16,5,100,225,255);
    const int sel=g_locomotionSel.load(std::memory_order_relaxed);
    char rows[14][72]{};
    std::snprintf(rows[0],sizeof(rows[0]),"MOVE DIRECTION   < %s >",
                  locomotion::MovementModeName());
    std::snprintf(rows[1],sizeof(rows[1]),"TURNING   < %s >",
                  locomotion::TurnModeName());
    std::snprintf(rows[2],sizeof(rows[2]),"TURN SENSITIVITY   < %d%% >",
                  locomotion::GetTurnSensitivityPercent());
    std::snprintf(rows[3],sizeof(rows[3]),"SNAP ANGLE   < %d DEG >",
                  locomotion::GetSnapAngleDegrees());
    std::snprintf(rows[4],sizeof(rows[4]),"WALKING HEAD BOB   < %s >",
                  locomotion::HeadBobEnabled()?"ON":"OFF");
    std::snprintf(rows[5],sizeof(rows[5]),"ARM SWING RUN   < %s >",
                  locomotion::GestureRunEnabled()?"ON":"OFF");
    std::snprintf(rows[6],sizeof(rows[6]),"SWIM STROKES   < %s >",
                  locomotion::GestureSwimEnabled()?"ON":"OFF");
    std::snprintf(rows[7],sizeof(rows[7]),"PARACHUTE CAMERA   < %s >",
                  locomotion::ParachuteCameraFollow()?"FOLLOW":"FIXED");
    std::snprintf(rows[8],sizeof(rows[8]),"PARACHUTE CONTROL   < %s >",
                  locomotion::ParachuteControlImmersive()?"RISERS":"STICKS");
    std::snprintf(rows[9],sizeof(rows[9]),"AUTO PARACHUTE   < %s >",
                  locomotion::AutoParachuteEnabled()?"ON":"OFF");
    std::snprintf(rows[10],sizeof(rows[10]),"FLIGHT CAMERA   < %s >",
                  locomotion::FlightCameraTilt()?"FULL TILT":"LEVEL");
    std::snprintf(rows[11],sizeof(rows[11]),"CUTSCENES   < %s >",
                  locomotion::CutsceneFirstPerson()?"FIRST PERSON":"CINEMA");
    std::snprintf(rows[12],sizeof(rows[12]),"RECENTER VIEW");
    std::snprintf(rows[13],sizeof(rows[13]),"BACK");
    // 13 rows on the 512px panel: the old 52px pitch already pushed the
    // bottom rows past the panel edge; a 32px pitch keeps everything on it.
    const int top=64,rowH=32;
    for (int i=0;i<14;++i) {
        const int y=top+i*rowH;
        if (i==sel) PanelFillRect(26,y-4,kPanelW-26,y+22,120,40,110,235);
        const int c=i==sel?255:200;
        PanelText(rows[i],cx,y,2,c,c,i==sel?120:c);
    }
    PanelText("STICK SEL   L2/R2 ADJUST   A ACTION",cx,kPanelH-18,
              2,150,185,150);
}

// Three pages: the root picks the element and the two calibration submenus
// (sprite crop/screen vs wrist placement) so neither list turns into a wall
// of mixed rows. Row indices MUST match the PG_HUD* handlers in main.cpp.
void BuildHudMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    const int sel = g_hudSel.load(std::memory_order_relaxed);
    const int page = g_hudPage.load(std::memory_order_relaxed);
    const int element=hud::CalibrationElement();
    const hud::ElementSettings settings=hud::GetElementSettings(element);
    const int wristSlot=(page==3)?hud::WRIST_SLOT_VEHICLE:
                        (page==4)?hud::WRIST_SLOT_WEAPON:
                        (page==5)?hud::WRIST_SLOT_TWOHAND:
                                  hud::WRIST_SLOT_HAND;
    const int dashModel=(page==3)
        ?g_lastDashModelId.load(std::memory_order_relaxed):-1;
    const hud::WristSettings wrist=
        hud::GetWristSettings(element,wristSlot,dashModel);
    const bool directText=hud::IsDirectTextElement(element);

    constexpr int kMaxRows=12;
    char rows[kMaxRows][80];
    int rowCount=0;

    if (page==1) {
        PanelText("HUD SPRITE CROP + SCREEN", cx, 12, 4, 100, 225, 255);
        char hint[80];
        std::snprintf(hint,sizeof(hint),directText?
                      "EDITING %s - DIRECT TEXT, SCREEN BOX ONLY":
                      "EDITING %s - CROP FROM REAL CAPTURE",
                      hud::ElementName(element));
        PanelText(hint,cx,44,2,255,205,105);
        if (directText) {
            std::snprintf(rows[0],sizeof(rows[0]),"SOURCE - DIRECT TEXT, NO CROP");
            std::snprintf(rows[1],sizeof(rows[1]),"SOURCE - DIRECT TEXT, NO CROP");
            std::snprintf(rows[2],sizeof(rows[2]),"SOURCE - DIRECT TEXT, NO CROP");
            std::snprintf(rows[3],sizeof(rows[3]),"SOURCE - DIRECT TEXT, NO CROP");
        } else {
            std::snprintf(rows[0],sizeof(rows[0]),"SOURCE X   < %d PX >",settings.sourceX);
            std::snprintf(rows[1],sizeof(rows[1]),"SOURCE Y   < %d PX >",settings.sourceY);
            std::snprintf(rows[2],sizeof(rows[2]),"SOURCE WIDTH   < %d PX >",settings.sourceWidth);
            std::snprintf(rows[3],sizeof(rows[3]),"SOURCE HEIGHT   < %d PX >",settings.sourceHeight);
        }
        std::snprintf(rows[4],sizeof(rows[4]),"SCREEN X   < %d%% >",settings.screenX);
        std::snprintf(rows[5],sizeof(rows[5]),"SCREEN Y   < %d%% >",settings.screenY);
        std::snprintf(rows[6],sizeof(rows[6]),"SCREEN WIDTH   < %d%% >",settings.screenWidth);
        std::snprintf(rows[7],sizeof(rows[7]),"SCREEN HEIGHT   < %d%% >",settings.screenHeight);
        std::snprintf(rows[8],sizeof(rows[8]),"SCALE   < %.1f X >",
                      static_cast<float>(settings.scaleTenths)/10.0f);
        if (directText)
            std::snprintf(rows[9],sizeof(rows[9]),
                          "AUTO SCAN - DIRECT TEXT HAS NO SOURCE");
        else
            std::snprintf(rows[9],sizeof(rows[9]),
                          "AUTO SCAN REAL FRAME   < %s >",
                          HudSourceScanActive()?"RUNNING":"PRESS A");
        std::snprintf(rows[10],sizeof(rows[10]),"BACK");
        rowCount=11;
    } else if (page==2||page==3) {
        const bool dash=page==3;
        PanelText(dash?"HUD DASHBOARD PLACEMENT":"HUD WRIST PLACEMENT",
                  cx, 12, 4, 100, 225, 255);
        char hint[80];
        if (directText) {
            std::snprintf(hint,sizeof(hint),
                          "%s IS A TEXT LAYER - NO PANEL",
                          hud::ElementName(element));
        } else if (dash) {
            std::snprintf(hint,sizeof(hint),
                          "EDITING %s - THIS VEHICLE (MODEL %d)",
                          hud::ElementName(element),dashModel);
        } else {
            std::snprintf(hint,sizeof(hint),
                          "EDITING %s ON THE %s ARM - LIVE PREVIEW",
                          hud::ElementName(element),
                          element==hud::RADAR?"LEFT":"RIGHT");
        }
        PanelText(hint,cx,44,2,255,205,105);
        std::snprintf(rows[0],sizeof(rows[0]),
                      dash?"TOWARD DRIVER   < %.1f CM >":
                           "ALONG ARM   < %.1f CM >",
                      wrist.alongMm/10.0f);
        std::snprintf(rows[1],sizeof(rows[1]),"ACROSS   < %.1f CM >",
                      wrist.acrossMm/10.0f);
        std::snprintf(rows[2],sizeof(rows[2]),
                      dash?"UP   < %.1f CM >":"LIFT   < %.1f CM >",
                      wrist.liftMm/10.0f);
        std::snprintf(rows[3],sizeof(rows[3]),"PITCH   < %d DEG >",
                      wrist.pitchDeg);
        std::snprintf(rows[4],sizeof(rows[4]),"YAW   < %d DEG >",
                      wrist.yawDeg);
        std::snprintf(rows[5],sizeof(rows[5]),"ROLL   < %d DEG >",
                      wrist.rollDeg);
        std::snprintf(rows[6],sizeof(rows[6]),"SIZE   < %.1f X >",
                      static_cast<float>(wrist.scaleTenths)/10.0f);
        std::snprintf(rows[7],sizeof(rows[7]),"BACK");
        rowCount=8;
    } else {
        PanelText("HUD CALIBRATION", cx, 12, 4, 100, 225, 255);
        char hint[80];
        std::snprintf(hint,sizeof(hint),
                      "ELEMENT: %s   PANELS ALWAYS SHOWN WHILE HERE",
                      hud::ElementName(element));
        PanelText(hint,cx,44,2,255,205,105);
        std::snprintf(rows[0], sizeof(rows[0]), "PRESET   < %s >", hud::PresetName());
        std::snprintf(rows[1], sizeof(rows[1]), "GAMEPLAY HUD   < %s >",
                      hud::GameplayHudEnabled() ? "ON" : "OFF");
        std::snprintf(rows[2],sizeof(rows[2]),"ELEMENT   < %s >",
                      hud::ElementName(element));
        std::snprintf(rows[3],sizeof(rows[3]),"VISIBLE   < %s >",
                      settings.enabled?"ON":"OFF");
        std::snprintf(rows[4],sizeof(rows[4]),"SPRITE CROP + SCREEN ...");
        std::snprintf(rows[5],sizeof(rows[5]),directText?
                      "WRIST - TEXT LAYER HAS NO PANEL":
                      "WRIST / DASH PLACEMENT ...");
        std::snprintf(rows[6],sizeof(rows[6]),directText?
                      "WEAPON GRIP - NO PANEL":
                      "WEAPON GRIP PLACEMENT ...");
        std::snprintf(rows[7],sizeof(rows[7]),directText?
                      "TWO-HAND GRIP - NO PANEL":
                      "TWO-HAND GRIP PLACEMENT ...");
        std::snprintf(rows[8],sizeof(rows[8]),"RESET THIS ELEMENT");
        std::snprintf(rows[9],sizeof(rows[9]),"BACK");
        rowCount=10;
    }

    constexpr int kWin=9;
    const int windowRows=std::min(rowCount,kWin);
    int start=std::clamp(sel-kWin/2,0,std::max(0,rowCount-windowRows));
    if (start>0) PanelText("- MORE -",cx,70,2,120,150,120);
    const int top=86,rowH=40;
    for (int window=0;window<windowRows;++window) {
        const int i=start+window;
        const int y=top+window*rowH;
        if (i == sel)
            PanelFillRect(34, y - 6, kPanelW - 34, y + 30, 120, 40, 110, 235);
        const int c = i == sel ? 255 : 200;
        PanelText(rows[i], cx, y, 2, c, c, i == sel ? 120 : c);
    }
    if (start<rowCount-windowRows)
        PanelText("- MORE -",cx,top+windowRows*rowH,2,120,150,120);
    PanelText(page==1?"AUTO SCAN DOES NOT SAVE   L2/R2 ADJUST":
              "A ENTER   B BACK   L2/R2 ADJUST", cx, kPanelH - 18,
              2, 150, 185, 150);
}

void BuildCheatMenu() {
    PanelClear();
    const int cx = kPanelW / 2;
    PanelText("CHEATS", cx, 12, 4, 100, 225, 255);
    const int count = g_menuCount.load(std::memory_order_relaxed);
    const int sel   = g_menuSelection.load(std::memory_order_relaxed);
    const int category = g_menuCategory.load(std::memory_order_relaxed);
    if (category >= 0)
        PanelText(cheats::CategoryName(category), cx, 38, 2, 150, 190, 210);
    constexpr int kWin = 18;
    int start = std::clamp(sel - kWin / 2, 0, std::max(0, count - kWin));
    const int shown = std::min(kWin, count);
    if (start > 0) PanelText("- MORE -", cx, 40, 2, 120, 150, 120);
    const int top = 58, rowH = 22;
    for (int w = 0; w < shown; ++w) {
        const int i = start + w;
        const int y = top + w * rowH;
        if (i == sel) PanelFillRect(24, y - 2, kPanelW - 24, y + 15, 120, 40, 110, 235);
        const bool isBack = i == count-1;
        const bool available = isBack || category < 0 ||
            cheats::CategoryItemAvailable(category,i);
        const bool submenu=category<0&&!isBack;
        const int c = available ? ((i == sel) ? 255 : 200) : 90;
        const char* label = isBack ? "BACK" :
            (category < 0 ? cheats::CategoryName(i) :
             cheats::CategoryItemName(category,i));
        if (submenu&&available)
            PanelText(label,cx,y,2,i==sel?255:100,
                      i==sel?235:225,i==sel?120:255);
        else
            PanelText(label, cx, y, 2, c, c,
                      (i == sel && available) ? 120 : c);
    }
    if (start + shown < count)
        PanelText("- MORE -", cx, top + shown * rowH, 2, 120, 150, 120);
    PanelText(category==cheats::CATEGORY_VEHICLES?
              "STICK MOVE   L2/R2 SELECT   A SPAWN   B BACK":
              "STICK MOVE   A USE   B BACK",
              cx, kPanelH - 22, 2, 150, 185, 150);
}

// Weapon calibration page: the active weapon's grip offset + barrel rotation, one
// row per param, the selected row highlighted. Left/right stick edits on the game
// side; here we just render the live values.
void BuildCalibMenu() {
    PanelClear();
    const int cx   = kPanelW / 2;
    const int type = g_calibWeaponType.load(std::memory_order_relaxed);
    const int sel  = g_calibSel.load(std::memory_order_relaxed);

    PanelText("WEAPON CALIBRATION", cx, 10, 3, 100, 225, 255);
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "%s   RIGHT MASTER",
                  savr::calib::WeaponName(type));
    PanelText(hdr, cx, 44, 2, 255, 200, 120);
    PanelText("LEFT USES THE SAME MIRRORED PROFILE", cx, 64, 2, 145, 185, 205);

    // 23 rows (0..22) in a scrolling window that keeps the selection centred, so the
    // fixed panel stays readable at arm's length instead of shrinking the font.
    constexpr int kRows = 23, kWin = 9;
    int start = sel - kWin / 2;
    if (start < 0)              start = 0;
    if (start > kRows - kWin)   start = kRows - kWin;

    if (start > 0) PanelText("- MORE -", cx, 80, 2, 120, 150, 120);
    const int top = 96, rowH = 38;
    for (int w = 0; w < kWin; ++w) {
        const int r = start + w;
        const int y = top + w * rowH;
        if (r == sel) PanelFillRect(18, y - 3, kPanelW - 18, y + 18, 120, 40, 110, 235);
        const int c = (r == sel) ? 255 : 200;
        char row[64];
        if (r == 22) {
            std::snprintf(row, sizeof(row), "BACK");
        } else if (r == 21) {
            std::snprintf(row, sizeof(row), "SAVE LASER   < %s >",
                          savr::calib::LaserLocked(type) ? "SAVED" : "PRESS A");
        } else if (r == 20) {
            std::snprintf(row, sizeof(row), "LASER BEAM   < %s >",
                          savr::calib::LaserModeName(type));
        } else if (r == 19) {
            std::snprintf(row, sizeof(row), "WEAPON LASER   < %s >",
                          savr::calib::LaserEnabled() ? "ON" : "OFF");
        } else {
            const int field = r;                           // 0..18
            if (field == savr::calib::F_SUP_STYLE) {
                std::snprintf(row, sizeof(row), "SUPPORT STYLE   < %s >",
                              savr::calib::StyleName(savr::calib::GetField(1, type, field)));
            } else {
                std::snprintf(row, sizeof(row), "%s   < %+.1f %s >",
                              savr::calib::FieldLabel(field),
                              savr::calib::DisplayValue(field, savr::calib::GetField(1, type, field)),
                              savr::calib::FieldUnit(field));
            }
        }
        PanelText(row, cx, y, 2, c, c, (r == sel) ? 120 : c);
    }
    if (start < kRows - kWin) PanelText("- MORE -", cx, top + kWin * rowH, 2, 120, 150, 120);

    PanelText("STICK SEL   L2/R2 ADJUST   A ACTION", cx, kPanelH - 18, 2, 150, 185, 150);
}

// Upload the current panel into the debug swapchain and describe it as a
// head-locked quad (view space), matching the Vice City overlay's placement.
bool PresentDebugPanel(XrCompositionLayerQuad& quad) {
    if (s.debug.handle == XR_NULL_HANDLE || s.viewSpace == XR_NULL_HANDLE) return false;
    const bool mainMenu = g_mainMenuActive.load(std::memory_order_relaxed);
    const bool calibMenu = g_calibActive.load(std::memory_order_relaxed);
    const bool holsterCalibMenu = g_holsterCalibActive.load(std::memory_order_relaxed);
    const bool holsterMenu = g_holsterMenuActive.load(std::memory_order_relaxed);
    const bool drivingMenu = g_drivingActive.load(std::memory_order_relaxed);
    const bool drivingCalibMenu =
        g_drivingCalibActive.load(std::memory_order_relaxed);
    const bool locomotionMenu = g_locomotionActive.load(std::memory_order_relaxed);
    const bool hudMenu = g_hudActive.load(std::memory_order_relaxed);
    const bool graphicsMenu = g_gfxActive.load(std::memory_order_relaxed);
    const bool graphicsDistanceMenu =
        g_gfxDistanceActive.load(std::memory_order_relaxed);
    const bool cheatMenu = g_menuVisible.load(std::memory_order_relaxed);
    const bool controlsMenuA = g_controlsMenuActive.load(std::memory_order_relaxed);
    const bool controlsTipsA = g_controlsTipsActive.load(std::memory_order_relaxed);
    const bool aboutMenuA = g_aboutActive.load(std::memory_order_relaxed);
    const bool profilerPanel = !(mainMenu || calibMenu || holsterCalibMenu ||
        holsterMenu || drivingMenu || drivingCalibMenu || locomotionMenu || hudMenu ||
        graphicsMenu || graphicsDistanceMenu || cheatMenu ||
        controlsMenuA || controlsTipsA || aboutMenuA);

    std::uint64_t contentRevision = 0;
    if (profilerPanel) {
        contentRevision = perf::DebugStatsRevision();
        if (g_profilerPanelRevision != contentRevision) {
            const perf::DebugStatsSnapshot stats = perf::GetDebugStats();
            contentRevision = stats.revision;
            BuildPanel(stats);
            g_profilerPanelRevision = contentRevision;
        }
    } else {
        // Menus are interactive and still redraw immediately. Invalidate every
        // cached profiler image so closing a menu cannot reveal its old pixels
        // from another swapchain image.
        g_profilerPanelRevision = ~std::uint64_t{0};
        std::fill(g_debugImageRevisions.begin(), g_debugImageRevisions.end(),
                  ~std::uint64_t{0});
        if      (mainMenu)          BuildMainMenu();
        else if (g_aboutActive.load(std::memory_order_relaxed))
                                    BuildAboutMenu();
        else if (g_controlsTipsActive.load(std::memory_order_relaxed))
                                    BuildControlsTipsMenu();
        else if (g_controlsMenuActive.load(std::memory_order_relaxed))
                                    BuildControlsMenu();
        else if (calibMenu)         BuildCalibMenu();
        else if (holsterCalibMenu)  BuildHolsterCalibMenu();
        else if (holsterMenu)       BuildHolsterMenu();
        else if (drivingMenu)       BuildDrivingMenu();
        else if (drivingCalibMenu)  BuildDrivingCalibrationMenu();
        else if (locomotionMenu)    BuildLocomotionMenu();
        else if (hudMenu)           BuildHudMenu();
        else if (graphicsMenu)      BuildGraphicsMenu();
        else if (graphicsDistanceMenu) BuildGraphicsDistanceMenu();
        else if (cheatMenu)         BuildCheatMenu();
    }

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(s.debug.handle, &acq, &idx), "acquire(debug)")) return false;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(s.debug.handle, &wait), "wait(debug)")) {
        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.debug.handle, &rel);
        return false;
    }

    bool debugImagesChanged = g_debugCacheSwapchain != s.debug.handle ||
                              g_debugImageNames.size() != s.debug.images.size();
    if (!debugImagesChanged) {
        for (std::size_t i = 0; i < s.debug.images.size(); ++i) {
            if (g_debugImageNames[i] != s.debug.images[i].image) {
                debugImagesChanged = true;
                break;
            }
        }
    }
    if (debugImagesChanged || g_debugImageRevisions.size() != s.debug.images.size()) {
        g_debugCacheSwapchain = s.debug.handle;
        g_debugImageNames.resize(s.debug.images.size());
        for (std::size_t i = 0; i < s.debug.images.size(); ++i)
            g_debugImageNames[i] = s.debug.images[i].image;
        g_debugImageRevisions.assign(s.debug.images.size(), ~std::uint64_t{0});
    }
    const bool needsUpload = !profilerPanel || idx >= g_debugImageRevisions.size() ||
        g_debugImageRevisions[idx] != contentRevision;
    if (needsUpload) {
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glBindTexture(GL_TEXTURE_2D, s.debug.images[idx].image);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kPanelW, kPanelH,
                        GL_RGBA, GL_UNSIGNED_BYTE, g_panelPixels);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        if (profilerPanel && idx < g_debugImageRevisions.size())
            g_debugImageRevisions[idx] = contentRevision;
    }

    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.debug.handle, &rel);

    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags        = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space             = s.viewSpace;                 // head-locked
    quad.eyeVisibility     = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain= s.debug.handle;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {kPanelW, kPanelH};
    quad.pose.orientation  = {0.0f, 0.0f, 0.0f, 1.0f};
    quad.pose.position     = {0.0f, -0.20f, -1.65f};
    // Vice City's settings surface is deliberately much larger than the old
    // square debug card. Keep the proven 512 texture but present it at a useful
    // cockpit-menu size so rows can be read without leaning forward.
    quad.size              = {1.8f, 1.8f * static_cast<float>(kPanelH) /
                                    static_cast<float>(kPanelW)};
    return true;
}

// Copy the HUD texture recorded with the submitted stereo ring slot into its
// own OpenXR swapchain. This is the same separation used by Vice City: world
// stays a reprojection-correct projection layer, while flat mobile UI is a
// comfortable binocular head-locked quad.
int PresentGameplayHud(XrCompositionLayerQuad quads[3],int sequence) {
    if (!hud::ShouldRenderClassicHud()||sequence<0||
        s.hud.handle==XR_NULL_HANDLE||s.viewSpace==XR_NULL_HANDLE) return 0;
    const int set=((sequence%kStereoSets)+kStereoSets)%kStereoSets;
    if (g_stereoGeneration[set].load(std::memory_order_acquire)!=sequence)
        return 0;
    const GLuint source=g_stereoHudTex[set].load(std::memory_order_relaxed);
    if (!source) return 0;

    uint32_t index=0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(s.hud.handle,&acquire,&index),
               "acquire(hud)")) return 0;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout=XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(s.hud.handle,&wait),"wait(hud)")) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.hud.handle,&release);
        return 0;
    }

    GLint oldDraw=0,oldRead=0,oldViewport[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
    glGetIntegerv(GL_VIEWPORT,oldViewport);
    const GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean srgb=g_swapchainSrgb?
        glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT):GL_FALSE;
    glDisable(GL_SCISSOR_TEST);
    if (g_swapchainSrgb) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s.framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,s.hud.images[index].image,0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s.eyeReadFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,source,0);
    const bool complete=
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE&&
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        // RenderWare's raster is upside-down in GL texture coordinates. Flip
        // once while copying so all cropped OpenXR regions are upright.
        glBlitFramebuffer(0,kGameplayHudHeight,kGameplayHudWidth,0,
                          0,0,s.hud.width,s.hud.height,
                          GL_COLOR_BUFFER_BIT,GL_LINEAR); // dead path; scales physical->logical
    }
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,0,0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,0,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(oldRead));
    glViewport(oldViewport[0],oldViewport[1],oldViewport[2],oldViewport[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (g_swapchainSrgb) {
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
        else glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.hud.handle,&release);
    if (!complete) return 0;

    struct Region {
        XrRect2Di rect;
        XrVector3f position;
        float width;
    };
    // One transparent RenderWare surface, presented as independent readable
    // VR panels: radar, player status, and tutorial/subtitle messages.
    const Region regions[3]={
        {{{0,0},{384,288}}, {-0.55f,-0.28f,-1.50f}, 0.55f},
        {{{640,288},{384,288}}, {0.55f,0.30f,-1.50f}, 0.65f},
        {{{0,288},{640,288}}, {0.00f,0.08f,-1.60f}, 1.25f}
    };
    for (int i=0;i<3;++i) {
        quads[i]={XR_TYPE_COMPOSITION_LAYER_QUAD};
        quads[i].layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quads[i].space=s.viewSpace;
        quads[i].eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        quads[i].subImage.swapchain=s.hud.handle;
        quads[i].subImage.imageRect=regions[i].rect;
        quads[i].pose.orientation={0.0f,0.0f,0.0f,1.0f};
        quads[i].pose.position=regions[i].position;
        quads[i].size={regions[i].width,
            regions[i].width*static_cast<float>(regions[i].rect.extent.height)/
                             static_cast<float>(regions[i].rect.extent.width)};
    }
    return 3;
}

// ===================== VR hands — real mesh (GL, present thread) ==============
// The game engine is deferred (records draw commands on a context-less thread), so
// RwIm3D never lands in our eye pass. We draw the hands ourselves in GL here on the
// present thread. The hand is Vice City's UltimateXR mesh: 2437 verts / 13092 idx,
// each vertex carrying 4 baked poses (open/grip/trigger/both). Per frame we blend
// the poses by (grip,trigger), transform into XR world space at the controller
// pose, and draw textured + lit with each eye's render pose + fov so the hands bake
// into the same image as the world and reproject with it.

inline XrVector3f vadd(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f vsub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XrVector3f vscale(XrVector3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float      vdot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline XrVector3f vcross(XrVector3f a, XrVector3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline XrVector3f vnorm(XrVector3f a) { const float l = std::sqrt(vdot(a, a)); return l > 1e-6f ? vscale(a, 1.0f / l) : XrVector3f{0, 0, 1}; }

XrVector3f RotateAroundAxis(XrVector3f v, XrVector3f axis, float angle) {
    axis = vnorm(axis);
    const float c = std::cos(angle), s = std::sin(angle);
    return vadd(vadd(vscale(v, c), vscale(vcross(axis, v), s)),
                vscale(axis, vdot(axis, v) * (1.0f - c)));
}

bool TwoHandStateUsable(const TwoHandVisualState& state) {
    if (!state.active || state.primaryHand < 0 || state.primaryHand > 1 ||
        state.supportHand < 0 || state.supportHand > 1 ||
        state.primaryHand == state.supportHand ||
        !std::isfinite(state.angleRadians)) return false;
    const XrVector3f axis{state.axis[0], state.axis[1], state.axis[2]};
    return std::isfinite(axis.x) && std::isfinite(axis.y) &&
           std::isfinite(axis.z) && vdot(axis, axis) >= 0.000001f;
}

XrVector3f TwoHandRotateVector(const TwoHandVisualState& state,
                               XrVector3f vector) {
    return RotateAroundAxis(vector,
        {state.axis[0], state.axis[1], state.axis[2]}, state.angleRadians);
}

XrVector3f TwoHandRotatePoint(const TwoHandVisualState& state,
                              XrVector3f point) {
    const XrVector3f pivot{state.pivot[0], state.pivot[1], state.pivot[2]};
    return vadd(pivot, TwoHandRotateVector(state, vsub(point, pivot)));
}

// Vice City Quest BuildAim, expressed in OpenXR LOCAL space. This is the one
// calibrated ray used by the visible beam; AIM rows move it live and SAVE LASER
// commits the chosen values. RIGHT is the canonical profile for both hands.
namespace {
LaserRay BuildCalibratedLaserRay(const HandPose& pose, int hand,
                                 const TwoHandVisualState& twoHand) {
    LaserRay ray{};
    const int weaponType = calib::ActiveWeapon();
    const bool firearm = (weaponType >= 22 && weaponType <= 34) || weaponType == 38;
    if (!pose.valid || !pose.aimValid || !firearm ||
        !calib::LaserVisibleForWeapon(weaponType) ||
        hand < 0 || hand > 1 ||
        g_weaponHand.load(std::memory_order_relaxed) != hand ||
        weaponType == 0) return ray;

    float origin[3]{};
    float direction[3]{};
    if (!vrcam::GetWeaponRayTracking(
            pose, hand, weaponType, twoHand, origin, direction)) {
        return ray;
    }
    ray.origin = {origin[0], origin[1], origin[2]};
    ray.direction = vnorm({direction[0], direction[1], direction[2]});
    ray.valid = true;
    return ray;
}
} // namespace

void MatPersp(float* m, float tanX, float tanY, float n, float f) {
    for (int i = 0; i < 16; ++i) m[i] = 0;
    m[0] = 1.0f / tanX; m[5] = 1.0f / tanY;
    m[10] = -(f + n) / (f - n); m[11] = -1.0f; m[14] = -2.0f * f * n / (f - n);
}
void MatView(float* m, const XrQuaternionf& q, const XrVector3f& p) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w),     r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w),     r21 = 2 * (y * z + x * w),     r22 = 1 - 2 * (x * x + y * y);
    m[0] = r00; m[1] = r01; m[2] = r02; m[3] = 0;
    m[4] = r10; m[5] = r11; m[6] = r12; m[7] = 0;
    m[8] = r20; m[9] = r21; m[10] = r22; m[11] = 0;
    m[12] = -(r00 * p.x + r10 * p.y + r20 * p.z);
    m[13] = -(r01 * p.x + r11 * p.y + r21 * p.z);
    m[14] = -(r02 * p.x + r12 * p.y + r22 * p.z);
    m[15] = 1;
}
void MatMul(float* o, const float* a, const float* b) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
        float sv = 0; for (int k = 0; k < 4; ++k) sv += a[k * 4 + r] * b[c * 4 + k];
        o[c * 4 + r] = sv;
    }
}

struct UxrhVertex { float pos[4][3]; float nrm[4][3]; float u, v; };   // 104 bytes
struct HandMesh   { std::vector<UxrhVertex> verts; std::vector<unsigned short> idx; bool loaded = false; };
HandMesh g_handMesh[2];
GLuint   g_handTex = 0;
bool     g_handAssetsTried = false;

bool LoadUxrh(const char* path, HandMesh& m) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { LOGE("[hands] cannot open %s", path); return false; }
    char magic[4]; uint32_t ver = 0, vc = 0, ic = 0;
    if (std::fread(magic, 1, 4, f) != 4 || std::fread(&ver, 4, 1, f) != 1 ||
        std::fread(&vc, 4, 1, f) != 1 || std::fread(&ic, 4, 1, f) != 1 ||
        std::memcmp(magic, "UXRH", 4) != 0) { std::fclose(f); LOGE("[hands] bad header %s", path); return false; }
    m.verts.resize(vc); m.idx.resize(ic);
    const bool ok = std::fread(m.verts.data(), sizeof(UxrhVertex), vc, f) == vc &&
                    std::fread(m.idx.data(), sizeof(unsigned short), ic, f) == ic;
    std::fclose(f);
    m.loaded = ok;
    LOGI("[hands] %s: %u verts %u idx ok=%d", path, vc, ic, ok);
    return ok;
}

GLuint LoadRgbaTexture(const char* path, int w, int h) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { LOGE("[hands] cannot open tex %s", path); return 0; }
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    const size_t n = std::fread(px.data(), 1, px.size(), f);
    std::fclose(f);
    if (n != px.size()) { LOGE("[hands] tex short read %zu/%zu", n, px.size()); return 0; }
    GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("[hands] albedo texture %d", t);
    return t;
}

void EnsureHandAssets() {
    if (g_handAssetsTried) return;
    g_handAssetsTried = true;
    const char* dir = "/sdcard/Android/data/com.rockstargames.gtasa/files/vrhands/";
    char path[256];
    std::snprintf(path, sizeof(path), "%sBigHandLeft.uxrh", dir);  LoadUxrh(path, g_handMesh[0]);
    std::snprintf(path, sizeof(path), "%sBigHandRight.uxrh", dir); LoadUxrh(path, g_handMesh[1]);
    std::snprintf(path, sizeof(path), "%sBigHandsAlbedo.rgba", dir); g_handTex = LoadRgbaTexture(path, 1024, 1024);
}

constexpr int  kHandMaxV = 5000, kHandMaxI = 27000;
float          g_hVerts[kHandMaxV * 8];   // pos.xyz, normal.xyz, u, v
unsigned short g_hIdx[kHandMaxI];
int            g_hVc = 0, g_hIc = 0;
int            g_hFirstIndex[2] = {0, 0};
int            g_hIndexCount[2] = {0, 0};

// Build both hands' meshes once per frame from the given pose snapshot (the pose
// the displayed eye textures were baked with), so hands + weapon share one frame.
void BuildHandMeshes(const HandPose hp[2], const TwoHandVisualState& twoHand,
                     const driving::WheelVisualState& drivingWheel) {
    g_hVc = 0; g_hIc = 0;
    g_hFirstIndex[0] = g_hFirstIndex[1] = 0;
    g_hIndexCount[0] = g_hIndexCount[1] = 0;
    if (!drivingWheel.trackedHandsEnabled) return;
    EnsureHandAssets();
    for (int hand = 0; hand < 2; ++hand) {
        if (!hp[hand].valid || !g_handMesh[hand].loaded) continue;
        const XrQuaternionf gq{hp[hand].gripOri[0], hp[hand].gripOri[1], hp[hand].gripOri[2], hp[hand].gripOri[3]};
        const XrQuaternionf aq{hp[hand].aimOri[0], hp[hand].aimOri[1], hp[hand].aimOri[2], hp[hand].aimOri[3]};
        XrVector3f pos{hp[hand].gripPos[0], hp[hand].gripPos[1], hp[hand].gripPos[2]};
        const XrVector3f gRight = vnorm(QuatRotate(gq, {1, 0, 0}));
        const XrVector3f gUp    = vnorm(QuatRotate(gq, {0, 1, 0}));
        const XrVector3f gFwd   = vnorm(QuatRotate(gq, {0, 0, -1}));
        const XrVector3f aimFwd = vnorm(QuatRotate(aq, {0, 0, -1}));
        const float sign = (1 - hand) == 0 ? 1.0f : -1.0f;
        XrVector3f forward = (vdot(aimFwd, aimFwd) > 0.1f) ? aimFwd : gUp;
        forward = vnorm(forward);
        XrVector3f up = vscale(gRight, sign);
        up = vsub(up, vscale(forward, vdot(up, forward)));
        if (vdot(up, up) < 0.0001f) up = vscale(gRight, sign);
        up = vnorm(up);
        XrVector3f right = vnorm(vcross(up, forward));
        if (vdot(right, gFwd) < 0.0f) right = vscale(right, -1.0f);

        float grip = std::clamp(hp[hand].grip, 0.0f, 1.0f);
        float trig = std::clamp(hp[hand].trigger, 0.0f, 1.0f);
        const bool steeringHand = drivingWheel.active &&
                                  drivingWheel.grabbed[hand];
        if (steeringHand) {
            // qbuild's car socket is a RenderWare matrix whose Right axis lies
            // along the rim and Forward axis is the wheel normal. Reproduce its
            // final UltimateXR anatomical decode, including LEFT parity, rather
            // than forcing a controller quaternion onto a synthetic socket.
            const XrVector3f socketRight = vnorm({
                drivingWheel.handleRight[hand][0],
                drivingWheel.handleRight[hand][1],
                drivingWheel.handleRight[hand][2]});
            const XrVector3f socketForward = vnorm({
                drivingWheel.handleForward[hand][0],
                drivingWheel.handleForward[hand][1],
                drivingWheel.handleForward[hand][2]});
            const float side = hand == 0 ? -1.0f : 1.0f;
            pos={drivingWheel.handlePosition[hand][0],
                 drivingWheel.handlePosition[hand][1],
                 drivingWheel.handlePosition[hand][2]};
            if (!drivingWheel.bike) {
                pos=vadd(pos,vscale(socketRight,side*0.025f));
                // Car-only visual pullback from current VC. Applying it to a
                // horizontal bike grip displaced the wrist from the handlebar.
                pos=vadd(pos,vscale(socketForward,-0.050f));
            }
            // FOLLOW BIKE pins the wrist to the complete live socket frame, so
            // pitch/roll from the bike is inherited even with local horizon.
            // CONTROLLER keeps the tracked wrist orientation for A/B tests; in
            // both modes the hand position stays locked to the handlebar.
            if (!drivingWheel.bike || drivingWheel.bikeHandsFollowTilt) {
                forward = socketForward;
                up = vscale(socketRight, side);
                right = vnorm(vcross(up, forward));
                if (hand == 0) right = vscale(right, -1.0f);
            }
            const float wheelGrip=drivingWheel.bike?grip:
                std::max(grip,0.90f);
            grip = wheelGrip;
            trig = std::max(trig, wheelGrip);
        } else if (TwoHandStateUsable(twoHand)) {
            if (hand == twoHand.primaryHand) {
                // The game-baked weapon used this same pivot/axis correction.
                // Move the visible wrist with it instead of leaving the hand at
                // the raw controller pose while the gun follows the support hand.
                pos     = TwoHandRotatePoint(twoHand, pos);
                forward = vnorm(TwoHandRotateVector(twoHand, forward));
                right   = vnorm(TwoHandRotateVector(twoHand, right));
                up      = vnorm(TwoHandRotateVector(twoHand, up));
            } else if (hand == twoHand.supportHand) {
                if (twoHand.supportAnchorValid &&
                    std::isfinite(twoHand.supportAnchor[0]) &&
                    std::isfinite(twoHand.supportAnchor[1]) &&
                    std::isfinite(twoHand.supportAnchor[2])) {
                    const XrVector3f anchor{twoHand.supportAnchor[0],
                                            twoHand.supportAnchor[1],
                                            twoHand.supportAnchor[2]};
                    pos = TwoHandRotatePoint(twoHand, anchor);
                }

                if (twoHand.supportBasisValid) {
                    const XrVector3f wristRight = TwoHandRotateVector(twoHand,
                        {twoHand.supportRight[0], twoHand.supportRight[1],
                         twoHand.supportRight[2]});
                    const XrVector3f wristForward = TwoHandRotateVector(twoHand,
                        {twoHand.supportForward[0], twoHand.supportForward[1],
                         twoHand.supportForward[2]});
                    const XrVector3f wristUp = TwoHandRotateVector(twoHand,
                        {twoHand.supportUp[0], twoHand.supportUp[1],
                         twoHand.supportUp[2]});
                    // PhysicalWeapon publishes the fully style-aware qbuild
                    // visual hand frame (including LEFT mesh mirroring). The UXR
                    // builder below consumes that conventional right/forward/up
                    // basis directly; only the shared shortest arc is applied here.
                    forward = vnorm(wristForward);
                    right   = vnorm(wristRight);
                    up      = vnorm(wristUp);
                    // qbuild's LEFT support matrix passes through one final
                    // controller-to-UltimateXR parity test before the mesh draw.
                    // Our direct basis bypasses that encode/decode, so reproduce
                    // its final visual result explicitly here.
                    if (hand == 0) right = vscale(right, -1.0f);
                }

                // qbuild's default MAGAZINE gesture closes every finger.  A
                // caller may explicitly publish the FROM_BELOW blend instead.
                grip = twoHand.supportGestureValid
                    ? std::clamp(twoHand.supportGestureGrip, 0.0f, 1.0f) : 1.0f;
                trig = twoHand.supportGestureValid
                    ? std::clamp(twoHand.supportGestureTrigger, 0.0f, 1.0f) : 1.0f;
            }
        }

        const float wO = (1 - grip) * (1 - trig), wG = grip * (1 - trig), wT = (1 - grip) * trig, wB = grip * trig;
        const HandMesh& m = g_handMesh[hand];
        const int base = g_hVc;
        for (const UxrhVertex& v : m.verts) {
            if (g_hVc >= kHandMaxV) break;
            const float lx = v.pos[0][0] * wO + v.pos[1][0] * wG + v.pos[2][0] * wT + v.pos[3][0] * wB;
            const float ly = v.pos[0][1] * wO + v.pos[1][1] * wG + v.pos[2][1] * wT + v.pos[3][1] * wB;
            const float lz = v.pos[0][2] * wO + v.pos[1][2] * wG + v.pos[2][2] * wT + v.pos[3][2] * wB;
            const float nx = v.nrm[0][0] * wO + v.nrm[1][0] * wG + v.nrm[2][0] * wT + v.nrm[3][0] * wB;
            const float ny = v.nrm[0][1] * wO + v.nrm[1][1] * wG + v.nrm[2][1] * wT + v.nrm[3][1] * wB;
            const float nz = v.nrm[0][2] * wO + v.nrm[1][2] * wG + v.nrm[2][2] * wT + v.nrm[3][2] * wB;
            // UXR local axes: X->forward (wrist pulled 5.5cm back), Z->right, Y->-up.
            const XrVector3f wp = vadd(vadd(vadd(pos, vscale(forward, lx - 0.055f)), vscale(right, lz)), vscale(up, -ly));
            const XrVector3f wn = vnorm(vadd(vadd(vscale(forward, nx), vscale(right, nz)), vscale(up, -ny)));
            float* o = &g_hVerts[g_hVc * 8];
            o[0] = wp.x; o[1] = wp.y; o[2] = wp.z; o[3] = wn.x; o[4] = wn.y; o[5] = wn.z; o[6] = v.u; o[7] = v.v;
            ++g_hVc;
        }
        g_hFirstIndex[hand] = g_hIc;
        for (unsigned short id : m.idx) {
            if (g_hIc >= kHandMaxI) break;
            g_hIdx[g_hIc++] = static_cast<unsigned short>(base + id);
        }
        g_hIndexCount[hand] = g_hIc - g_hFirstIndex[hand];
    }
}

GLuint g_handProg = 0, g_handVbo = 0, g_handEbo = 0, g_handDepthRbo = 0;
GLint  g_handMvpLoc = -1, g_handTexLoc = -1, g_handSkinLoc = -1;
int    g_handDepthW = 0, g_handDepthH = 0;
GLint  g_handDepthFormat = 0, g_handDepthBits = 0;
int    g_handDepthRetryCountdown = 0;
int    g_handDepthFailedW = 0, g_handDepthFailedH = 0;
GLint  g_handDepthFailedFormat = 0;
GLenum g_handDepthLastAllocationError = GL_NO_ERROR;

// A renderbuffer name is queried once, not once per compositor frame. The
// eye-ring can temporarily contain active + pending + retired generations, so
// keep enough slots for several complete six-target rings.
struct GameDepthFormatProbe {
    GLuint rbo{};
    GLint format{};
    GLint bits{};
    bool compatible{};
};
constexpr int kGameDepthFormatProbeCount = 24;
GameDepthFormatProbe g_gameDepthFormatProbes[kGameDepthFormatProbeCount]{};
int g_gameDepthFormatProbeNext = 0;
GLint g_expectedGameDepthFormat = 0;
GLint g_expectedGameDepthBits = 0;

bool QueryGameDepthFormat(GLuint rbo, GLint& format, GLint& bits,
                          GLenum& queryError) {
    format = 0;
    bits = 0;
    queryError = GL_NO_ERROR;
    if (rbo == 0) return false;

    for (const GameDepthFormatProbe& probe : g_gameDepthFormatProbes) {
        if (probe.rbo != rbo) continue;
        format = probe.format;
        bits = probe.bits;
        return probe.compatible;
    }

    GLint previousBinding = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousBinding);
    while (glGetError() != GL_NO_ERROR) {}
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,
                                 GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,
                                 GL_RENDERBUFFER_DEPTH_SIZE, &bits);
    queryError = glGetError();
    glBindRenderbuffer(GL_RENDERBUFFER,
                       static_cast<GLuint>(previousBinding));
    const GLenum restoreError = glGetError();
    if (queryError == GL_NO_ERROR) queryError = restoreError;

    const bool valid = queryError == GL_NO_ERROR && format != 0 && bits > 0;
    if (!valid) return false;  // Retry later; never cache a transient fallback.

    bool compatible = true;
    if (g_expectedGameDepthFormat == 0) {
        g_expectedGameDepthFormat = format;
        g_expectedGameDepthBits = bits;
    } else if (format != g_expectedGameDepthFormat ||
               bits != g_expectedGameDepthBits) {
        compatible = false;
        static bool mixedLogged = false;
        if (!mixedLogged) {
            mixedLogged = true;
            LOGW("[hands] mixed eye depth formats: expected=0x%x/%d rbo=%u actual=0x%x/%d; source rejected",
                 g_expectedGameDepthFormat, g_expectedGameDepthBits,
                 rbo, format, bits);
        }
    }

    g_gameDepthFormatProbes[g_gameDepthFormatProbeNext] =
        GameDepthFormatProbe{rbo, format, bits, compatible};
    g_gameDepthFormatProbeNext =
        (g_gameDepthFormatProbeNext + 1) % kGameDepthFormatProbeCount;
    LOGI("[hands] eye depth probe rbo=%u format=0x%x bits=%d compatible=%d",
         rbo, format, bits, compatible ? 1 : 0);
    return compatible;
}

bool EnsureHandDepthStorage(int width, int height, GLint desiredFormat,
                            GLenum& allocationError) {
    allocationError = GL_NO_ERROR;
    if (g_handDepthRbo != 0 && g_handDepthW == width &&
        g_handDepthH == height && g_handDepthFormat == desiredFormat) {
        return true;
    }
    if (g_handDepthRetryCountdown > 0 && width == g_handDepthFailedW &&
        height == g_handDepthFailedH &&
        desiredFormat == g_handDepthFailedFormat) {
        --g_handDepthRetryCountdown;
        allocationError = g_handDepthLastAllocationError;
        return false;
    }

    GLint previousBinding = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousBinding);
    while (glGetError() != GL_NO_ERROR) {}

    GLuint candidate = 0;
    glGenRenderbuffers(1, &candidate);
    if (candidate != 0) {
        glBindRenderbuffer(GL_RENDERBUFFER, candidate);
        glRenderbufferStorage(GL_RENDERBUFFER, desiredFormat, width, height);
    }
    GLint actualFormat = 0;
    GLint actualBits = 0;
    if (candidate != 0) {
        glGetRenderbufferParameteriv(GL_RENDERBUFFER,
                                     GL_RENDERBUFFER_INTERNAL_FORMAT,
                                     &actualFormat);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER,
                                     GL_RENDERBUFFER_DEPTH_SIZE,
                                     &actualBits);
    }
    allocationError = glGetError();
    const bool ready = candidate != 0 && allocationError == GL_NO_ERROR &&
        actualFormat == desiredFormat && actualBits > 0;

    GLuint restoreBinding = static_cast<GLuint>(previousBinding);
    if (ready) {
        const GLuint previousDepth = g_handDepthRbo;
        g_handDepthRbo = candidate;
        g_handDepthW = width;
        g_handDepthH = height;
        g_handDepthFormat = actualFormat;
        g_handDepthBits = actualBits;
        g_handDepthRetryCountdown = 0;
        g_handDepthFailedW = 0;
        g_handDepthFailedH = 0;
        g_handDepthFailedFormat = 0;
        g_handDepthLastAllocationError = GL_NO_ERROR;
        if (restoreBinding == previousDepth) restoreBinding = candidate;
        if (previousDepth != 0) glDeleteRenderbuffers(1, &previousDepth);
        LOGI("[hands] depth storage %dx%d format=0x%x bits=%d",
             width, height, actualFormat, actualBits);
    } else {
        if (candidate != 0) glDeleteRenderbuffers(1, &candidate);
        // A persistent OOM/unsupported-format failure must not create and
        // delete an RBO twice per compositor frame. Retry this exact request
        // after roughly five seconds at 60 Hz; a size/format change retries
        // immediately.
        g_handDepthRetryCountdown = 600;
        g_handDepthFailedW = width;
        g_handDepthFailedH = height;
        g_handDepthFailedFormat = desiredFormat;
        g_handDepthLastAllocationError = allocationError != GL_NO_ERROR
            ? allocationError : GL_INVALID_OPERATION;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, restoreBinding);
    const GLenum restoreError = glGetError();
    if (allocationError == GL_NO_ERROR) allocationError = restoreError;
    return ready && allocationError == GL_NO_ERROR;
}

GLuint CompileHandShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(sh, sizeof(log), nullptr, log); LOGE("[hands] shader: %s", log); }
    return sh;
}

void EnsureHandProgram() {
    if (g_handProg) return;
    const char* vs = "#version 300 es\n"
                     "layout(location=0) in vec3 aPos;\n"
                     "layout(location=1) in vec3 aNrm;\n"
                     "layout(location=2) in vec2 aUV;\n"
                     "uniform mat4 uMVP;\n"
                     "out vec3 vN; out vec2 vUV;\n"
                     "void main(){ vN = aNrm; vUV = aUV; gl_Position = uMVP * vec4(aPos,1.0); }\n";
    const char* fs = "#version 300 es\n"
                     "precision mediump float;\n"
                     "in vec3 vN; in vec2 vUV;\n"
                     "uniform sampler2D uTex; uniform float uDarkSkin; out vec4 o;\n"
                     "void main(){\n"
                     "  vec3 L = normalize(vec3(-0.2, 0.7, 0.55));\n"
                     "  float d = max(dot(normalize(vN), L), 0.0);\n"
                     "  float sh = 0.72 + 0.28 * d;\n"
                     "  vec3 base = texture(uTex, vUV).rgb;\n"
                     "  vec3 dark = base * vec3(0.48, 0.38, 0.30);\n"
                     "  o = vec4(mix(base, dark, uDarkSkin) * sh, 1.0);\n"
                     "}\n";
    const GLuint v = CompileHandShader(GL_VERTEX_SHADER, vs), f = CompileHandShader(GL_FRAGMENT_SHADER, fs);
    g_handProg = glCreateProgram();
    glAttachShader(g_handProg, v); glAttachShader(g_handProg, f); glLinkProgram(g_handProg);
    glDeleteShader(v); glDeleteShader(f);
    g_handMvpLoc = glGetUniformLocation(g_handProg, "uMVP");
    g_handTexLoc = glGetUniformLocation(g_handProg, "uTex");
    g_handSkinLoc = glGetUniformLocation(g_handProg, "uDarkSkin");
    glGenBuffers(1, &g_handVbo); glGenBuffers(1, &g_handEbo);
    LOGI("[hands] GL program ready");
}

// Flat-colour program + cube geometry for the holster markers.
GLuint g_markerProg = 0, g_markerVbo = 0, g_markerEbo = 0;
GLint  g_markerMvpLoc = -1, g_markerColLoc = -1;
GLuint g_arrowProg = 0;
GLint  g_arrowMvpLoc = -1, g_arrowColLoc = -1;
float          g_markerVerts[8 * 3 * 8];   // up to 8 cubes x 8 corners x xyz
unsigned short g_markerIdx[8 * 36];

void EnsureMarkerProgram() {
    if (g_markerProg) return;
    const char* vs = "#version 300 es\n"
                     "layout(location=0) in vec3 aPos;\n"
                     "uniform mat4 uMVP;\n"
                     "void main(){ gl_Position = uMVP * vec4(aPos,1.0); }\n";
    const char* fs = "#version 300 es\n"
                     "precision mediump float;\n"
                     "uniform vec4 uCol; out vec4 o;\n"
                     "void main(){ o = uCol; }\n";
    const GLuint v = CompileHandShader(GL_VERTEX_SHADER, vs), f = CompileHandShader(GL_FRAGMENT_SHADER, fs);
    g_markerProg = glCreateProgram();
    glAttachShader(g_markerProg, v); glAttachShader(g_markerProg, f); glLinkProgram(g_markerProg);
    glDeleteShader(v); glDeleteShader(f);
    g_markerMvpLoc = glGetUniformLocation(g_markerProg, "uMVP");
    g_markerColLoc = glGetUniformLocation(g_markerProg, "uCol");
    glGenBuffers(1, &g_markerVbo); glGenBuffers(1, &g_markerEbo);
}

void EnsureArrowProgram() {
    if (g_arrowProg) return;
    const char* vs = "#version 300 es\n"
                     "layout(location=0) in vec3 aPos;\n"
                     "layout(location=1) in vec2 aShade;\n"
                     "uniform mat4 uMVP; out vec2 vShade;\n"
                     "void main(){ vShade=aShade; gl_Position=uMVP*vec4(aPos,1.0); }\n";
    const char* fs = "#version 300 es\n"
                     "precision mediump float;\n"
                     "uniform vec4 uCol; in vec2 vShade; out vec4 o;\n"
                     "void main(){ o=vec4(uCol.rgb*vShade.x,uCol.a*vShade.y); }\n";
    const GLuint v = CompileHandShader(GL_VERTEX_SHADER, vs);
    const GLuint f = CompileHandShader(GL_FRAGMENT_SHADER, fs);
    g_arrowProg = glCreateProgram();
    glAttachShader(g_arrowProg, v);
    glAttachShader(g_arrowProg, f);
    glLinkProgram(g_arrowProg);
    glDeleteShader(v);
    glDeleteShader(f);
    g_arrowMvpLoc = glGetUniformLocation(g_arrowProg, "uMVP");
    g_arrowColLoc = glGetUniformLocation(g_arrowProg, "uCol");
}

// ===================== VR wrist HUD (Vice City Quest pattern) ================
// IMMERSIVE preset: the radar and status crops of the captured HUD texture are
// worn as panels on the inner wrists, exactly like the Vice City Quest port's
// wrist widgets — the map on the left forearm, the money/health/armour/wanted
// readout on the right. The quads are placed at the SAME baked grip poses the
// hand meshes use and drawn inside the same eye FBO/depth pass, so panel and
// hand can never slip against each other or sample different frames.

GLuint g_wristProg = 0, g_wristVbo = 0, g_wristSampler = 0;
GLint  g_wristMvpLoc = -1, g_wristTexLoc = -1, g_wristOpacityLoc = -1;

void EnsureWristProgram() {
    if (g_wristProg) return;
    const char* vs = "#version 300 es\n"
                     "layout(location=0) in vec3 aPos;\n"
                     "layout(location=1) in vec2 aUV;\n"
                     "uniform mat4 uMVP;\n"
                     "out vec2 vUV;\n"
                     "void main(){ vUV = aUV; gl_Position = uMVP * vec4(aPos,1.0); }\n";
    // Same luminance keying as the in-eye composite: the capture's alpha
    // channel is not trustworthy after RenderWare HUD blending, so dark pixels
    // turn transparent and the radar keeps its shape on the arm.
    const char* fs = "#version 300 es\n"
                     "precision mediump float;\n"
                     "in vec2 vUV;\n"
                     "uniform sampler2D uTex; uniform float uOpacity; out vec4 o;\n"
                     "void main(){\n"
                     "  vec4 c = texture(uTex, vUV);\n"
                     "  float coverage = max(c.r, max(c.g, c.b));\n"
                     "  float keyed = smoothstep(0.008, 0.055, coverage);\n"
                     "  o = vec4(c.rgb, c.a * keyed * uOpacity);\n"
                     "}\n";
    const GLuint v = CompileHandShader(GL_VERTEX_SHADER, vs), f = CompileHandShader(GL_FRAGMENT_SHADER, fs);
    g_wristProg = glCreateProgram();
    glAttachShader(g_wristProg, v); glAttachShader(g_wristProg, f); glLinkProgram(g_wristProg);
    glDeleteShader(v); glDeleteShader(f);
    GLint linked = GL_FALSE;
    glGetProgramiv(g_wristProg, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[512]{};
        glGetProgramInfoLog(g_wristProg, sizeof(log), nullptr, log);
        LOGE("[wrist] program link failed: %s", log);
        glDeleteProgram(g_wristProg);
        g_wristProg = 0;
        return;
    }
    g_wristMvpLoc = glGetUniformLocation(g_wristProg, "uMVP");
    g_wristTexLoc = glGetUniformLocation(g_wristProg, "uTex");
    g_wristOpacityLoc = glGetUniformLocation(g_wristProg, "uOpacity");
    glGenBuffers(1, &g_wristVbo);
    glGenSamplers(1, &g_wristSampler);
    glSamplerParameteri(g_wristSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_wristSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_wristSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_wristSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    LOGI("[wrist] GL program ready");
}

// Vice City Quest shipped values: panel width on the arm, offset back along
// the forearm from the grip origin, and lift off the skin.
constexpr float kWristPanelWidthMetres[2] = {0.115f, 0.130f}; // radar, status
constexpr float kWristPanelBack = 0.07f;
constexpr float kWristPanelLift = 0.035f;
// Glance reveal (VC defaults): full inside ~20 deg of gaze, gone past ~34 deg;
// the last 15cm of the reach fade the panel instead of blinking it off. The
// wrist wears the VC 60cm reach; the dashboard sits farther from the head, so
// its panels use a longer one.
constexpr float kWristGazeFullCos = 0.94f;
constexpr float kWristGazeGoneCos = 0.83f;
constexpr float kWristGazeRange = 0.60f;
constexpr float kDashGazeRange = 1.60f;
constexpr float kWristGazeFadeMetres = 0.15f;

float WristPanelGazeOpacity(const XrPosef& rp, const XrVector3f& centre,
                            float range) {
    const XrVector3f eye{rp.position.x, rp.position.y, rp.position.z};
    const XrVector3f forward = QuatRotate(rp.orientation, {0.0f, 0.0f, -1.0f});
    const XrVector3f toPanel = vsub(centre, eye);
    const float distance = std::sqrt(vdot(toPanel, toPanel));
    if (distance < 0.01f) return 1.0f;
    if (distance >= range) return 0.0f;
    const float facing = vdot(vscale(toPanel, 1.0f / distance), forward);
    if (facing <= kWristGazeGoneCos) return 0.0f;
    const float aim = facing >= kWristGazeFullCos ? 1.0f :
        (facing - kWristGazeGoneCos) / (kWristGazeFullCos - kWristGazeGoneCos);
    const float reach = distance <= range - kWristGazeFadeMetres ? 1.0f :
        (range - distance) / kWristGazeFadeMetres;
    return std::min(aim, reach);
}

// Rotates an orthonormal pair in its own plane — the Vice City helper used to
// swing a panel's frame around each of its own axes in turn.
void RotateWristAxes(XrVector3f& first, XrVector3f& second, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    const XrVector3f a = first, b = second;
    first  = vadd(vscale(a, c), vscale(b, s));
    second = vsub(vscale(b, c), vscale(a, s));
}

// One worn panel drawn at a prepared anchor. `base` already contains the
// built-in placement (wrist back/lift or the dashboard side offset); the
// saved millimetre calibration rides the along/across/lift axes BEFORE the
// rotations, so moving and turning stay independent (VC rule). `calibrating`
// forces the panel visible at full opacity (no gaze reveal, no VISIBLE gate)
// so it can be placed from the HUD menu.
void DrawWristPanel(int element, int panelIndex, bool calibrating, int slot,
                    int vehicleModel,
                    const XrVector3f& base,
                    const XrVector3f& alongAxis, const XrVector3f& acrossAxis,
                    const XrVector3f& liftAxis,
                    XrVector3f right, XrVector3f up, XrVector3f normal,
                    const XrPosef& rp, float gazeRange) {
    const hud::ElementSettings settings = hud::GetElementSettings(element);
    if ((!settings.enabled && !calibrating) || settings.sourceWidth <= 0 ||
        settings.sourceHeight <= 0) return;
    const hud::WristSettings wrist =
        hud::GetWristSettings(element, slot, vehicleModel);

    const XrVector3f centre = vadd(base,
        vadd(vadd(vscale(alongAxis, wrist.alongMm * 0.001f),
                  vscale(acrossAxis, wrist.acrossMm * 0.001f)),
             vscale(liftAxis, wrist.liftMm * 0.001f)));

    constexpr float kToRadians = 3.14159265f / 180.0f;
    if (wrist.yawDeg)
        RotateWristAxes(normal, right, wrist.yawDeg * kToRadians);
    if (wrist.pitchDeg)
        RotateWristAxes(up, normal, wrist.pitchDeg * kToRadians);
    if (wrist.rollDeg)
        RotateWristAxes(right, up, wrist.rollDeg * kToRadians);

    const float opacity =
        calibrating ? 1.0f : WristPanelGazeOpacity(rp, centre, gazeRange);
    if (opacity <= 0.0f) return;

    const float w = kWristPanelWidthMetres[panelIndex] *
                    static_cast<float>(wrist.scaleTenths) / 10.0f;
    const float h = w * static_cast<float>(settings.sourceHeight) /
                    static_cast<float>(settings.sourceWidth);
    const float u0 = static_cast<float>(settings.sourceX) /
                     kGameplayHudLogicalWidth;
    const float v0 = static_cast<float>(settings.sourceY) /
                     kGameplayHudLogicalHeight;
    const float u1 = u0 + static_cast<float>(settings.sourceWidth) /
                     kGameplayHudLogicalWidth;
    const float v1 = v0 + static_cast<float>(settings.sourceHeight) /
                     kGameplayHudLogicalHeight;

    const XrVector3f rHalf = vscale(right, w * 0.5f);
    const XrVector3f uHalf = vscale(up, h * 0.5f);
    const XrVector3f tl = vadd(vsub(centre, rHalf), uHalf);
    const XrVector3f tr = vadd(vadd(centre, rHalf), uHalf);
    const XrVector3f bl = vsub(vsub(centre, rHalf), uHalf);
    const XrVector3f br = vsub(vadd(centre, rHalf), uHalf);
    // The capture texture is stored top-down (the in-eye composite samples it
    // the same way), so v0 is the crop's top row and lands at the finger edge.
    const float verts[4 * 5] = {
        tl.x, tl.y, tl.z, u0, v0,
        bl.x, bl.y, bl.z, u0, v1,
        tr.x, tr.y, tr.z, u1, v0,
        br.x, br.y, br.z, u1, v1,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glUniform1f(g_wristOpacityLoc, opacity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Anchor for a panel worn on one arm. Starts from the raw baked grip pose,
// then follows the VISIBLE hand: the two-hand grip rotates the primary hand
// and snaps the support hand onto the weapon, and the panel must ride the
// hand the player sees, not the raw controller (VC rule).
bool BuildHandPanelAnchor(const HandPose& hp, int hand,
                          const TwoHandVisualState& twoHand,
                          XrVector3f& base, XrVector3f& alongAxis,
                          XrVector3f& acrossAxis, XrVector3f& liftAxis,
                          XrVector3f& right, XrVector3f& up,
                          XrVector3f& normal) {
    if (!hp.valid) return false;
    const XrQuaternionf q{hp.gripOri[0], hp.gripOri[1], hp.gripOri[2],
                          hp.gripOri[3]};
    XrVector3f origin{hp.gripPos[0], hp.gripPos[1], hp.gripPos[2]};
    // OpenXR grip axes: +Z runs back towards the wrist, +Y stands away from
    // the palm, +X is the palm normal (mirrored between hands). The right
    // hand takes the left frame with side and palm-up flipped — two flips
    // keep it right-handed and land the panel on the matching side of that
    // arm. This is the shipped Vice City wrist rule, ported verbatim.
    XrVector3f side   = QuatRotate(q, {1, 0, 0});
    XrVector3f palmUp = QuatRotate(q, {0, 1, 0});
    XrVector3f backward = QuatRotate(q, {0, 0, 1});
    const float handSign = (hand == 1) ? -1.0f : 1.0f;
    side = vscale(side, handSign);
    palmUp = vscale(palmUp, handSign);

    if (TwoHandStateUsable(twoHand)) {
        if (hand == twoHand.primaryHand) {
            // Same pivot/axis correction the baked weapon and hand mesh use.
            origin   = TwoHandRotatePoint(twoHand, origin);
            side     = vnorm(TwoHandRotateVector(twoHand, side));
            palmUp   = vnorm(TwoHandRotateVector(twoHand, palmUp));
            backward = vnorm(TwoHandRotateVector(twoHand, backward));
        } else if (hand == twoHand.supportHand) {
            if (twoHand.supportAnchorValid &&
                std::isfinite(twoHand.supportAnchor[0]) &&
                std::isfinite(twoHand.supportAnchor[1]) &&
                std::isfinite(twoHand.supportAnchor[2])) {
                origin = TwoHandRotatePoint(twoHand,
                    {twoHand.supportAnchor[0], twoHand.supportAnchor[1],
                     twoHand.supportAnchor[2]});
            }
            if (twoHand.supportBasisValid) {
                // The published support basis is a conventional wrist frame:
                // forward to the fingers, up away from the palm. Both are
                // already anatomical (no OpenXR hand mirroring), so the grip
                // vocabulary follows directly and side closes the frame.
                palmUp = vnorm(TwoHandRotateVector(twoHand,
                    {twoHand.supportUp[0], twoHand.supportUp[1],
                     twoHand.supportUp[2]}));
                backward = vnorm(vscale(TwoHandRotateVector(twoHand,
                    {twoHand.supportForward[0], twoHand.supportForward[1],
                     twoHand.supportForward[2]}), -1.0f));
                side = vnorm(vcross(palmUp, backward));
            }
        }
    }

    // Underside face — a watch turned to the inner wrist. VC ships both the
    // map and the status readout there.
    constexpr float faceSign = -1.0f;
    right = vscale(side, faceSign);
    up = vscale(backward, -1.0f);           // twelve o'clock to the fingers
    normal = vscale(palmUp, faceSign);
    base = vadd(origin,
        vadd(vscale(backward, kWristPanelBack),
             vscale(palmUp, faceSign * kWristPanelLift)));
    alongAxis = backward;
    acrossAxis = right;
    liftAxis = vscale(palmUp, faceSign);
    return true;
}

// Dashboard side offset of each panel from the steering-wheel centre, before
// the saved calibration: the map left of the wheel, the readout right of it.
constexpr float kDashPanelAcrossMetres[2] = {-0.32f, 0.32f};

// Both panels for the current eye. On foot they ride the visible wrists; in
// a vehicle with the immersive wheel they anchor to the wheel centre like the
// Vice City dashboard instrumentation, with their own saved calibration
// slot. Runs inside the hand pass: the depth test against the copied
// game+hand depth lets the arm and weapon occlude a panel naturally; no
// depth write, so transparent corners never mask anything.
void DrawWristPanelsForEye(const float* mvp, const HandPose hands[2],
                           const TwoHandVisualState& twoHand,
                           const driving::WheelVisualState& drivingWheel,
                           unsigned int hudTex, const XrPosef& rp,
                           unsigned int weaponHandMask) {
    const bool calibrating = g_hudActive.load(std::memory_order_relaxed);
    if (!hudTex || (!hud::ShouldRenderWristHud() && !calibrating)) return;
    EnsureWristProgram();
    if (!g_wristProg) return;
    glUseProgram(g_wristProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hudTex);
    glBindSampler(0, g_wristSampler);
    glUniform1i(g_wristTexLoc, 0);
    glUniformMatrix4fv(g_wristMvpLoc, 1, GL_FALSE, mvp);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindBuffer(GL_ARRAY_BUFFER, g_wristVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));

    const int panelElements[2] = {hud::RADAR, hud::HEALTH};
    if (drivingWheel.active || drivingWheel.dashAnchorValid) {
        // Dashboard: one shared anchor at the wheel/handlebar centre, facing
        // the driver, in BOTH driving modes (the geometry frame is published
        // even when the immersive wheel interaction is off).
        const XrVector3f wheelCentre{drivingWheel.center[0],
                                     drivingWheel.center[1],
                                     drivingWheel.center[2]};
        const XrVector3f fieldRight = vnorm({drivingWheel.right[0],
                                             drivingWheel.right[1],
                                             drivingWheel.right[2]});
        const XrVector3f fieldUp = vnorm({drivingWheel.up[0],
                                          drivingWheel.up[1],
                                          drivingWheel.up[2]});
        const XrVector3f fieldNormal = vnorm({drivingWheel.normal[0],
                                              drivingWheel.normal[1],
                                              drivingWheel.normal[2]});
        // Field meanings differ between the two builders: the car wheel frame
        // stores {right, wheel-plane up, VEHICLE-FORWARD normal}; the bike bar
        // frame stores {right, FORWARD in `up`, WORLD-UP in `normal`}. Both
        // resolve to the same panel vocabulary: right stays lateral, up is the
        // real upward axis, normal faces the driver (against forward).
        const XrVector3f panelRight = fieldRight;
        const XrVector3f panelUp = drivingWheel.bike ? fieldNormal : fieldUp;
        const XrVector3f panelNormal = vscale(
            drivingWheel.bike ? fieldUp : fieldNormal, -1.0f);
        for (int panel = 0; panel < 2; ++panel) {
            const XrVector3f base = vadd(wheelCentre,
                vscale(panelRight, kDashPanelAcrossMetres[panel]));
            DrawWristPanel(panelElements[panel], panel, calibrating,
                           hud::WRIST_SLOT_VEHICLE, drivingWheel.modelId,
                           base,
                           panelNormal, panelRight, panelUp,
                           panelRight, panelUp, panelNormal, rp,
                           kDashGazeRange);
        }
    } else {
        // Per-panel slot selection: normal wrist, one-hand weapon grip
        // (panel slides beside the gripping hand), or two-hand grip (both
        // panels re-anchor to the primary hand). Calibration pages 4/5 force
        // the respective slot so it can be tuned without holding a gun.
        const int hudMenuPage = g_hudPage.load(std::memory_order_relaxed);
        const bool twoHandHeld = twoHand.active &&
            twoHand.primaryHand >= 0 && twoHand.primaryHand < 2 &&
            (weaponHandMask & (1u << twoHand.primaryHand)) != 0;
        for (int panel = 0; panel < 2; ++panel) {
            int slot = hud::WRIST_SLOT_HAND;
            int anchorHand = panel;
            if (calibrating && hudMenuPage == 4) {
                slot = hud::WRIST_SLOT_WEAPON;
            } else if (calibrating && hudMenuPage == 5) {
                slot = hud::WRIST_SLOT_TWOHAND;
                if (twoHandHeld) anchorHand = twoHand.primaryHand;
            } else if (twoHandHeld) {
                slot = hud::WRIST_SLOT_TWOHAND;
                anchorHand = twoHand.primaryHand;
            } else if ((weaponHandMask & (1u << panel)) != 0) {
                slot = hud::WRIST_SLOT_WEAPON;
            }
            XrVector3f base, alongAxis, acrossAxis, liftAxis, right, up,
                normal;
            if (!BuildHandPanelAnchor(hands[anchorHand], anchorHand, twoHand,
                                      base, alongAxis, acrossAxis, liftAxis,
                                      right, up, normal)) continue;
            DrawWristPanel(panelElements[panel], anchorHand, calibrating,
                           slot, -1, base,
                           alongAxis, acrossAxis, liftAxis,
                           right, up, normal, rp, kWristGazeRange);
        }
    }
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindSampler(0, 0);
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("[wrist] panels active (radar=LEFT/dash-left, status=RIGHT/dash-right)");
    }
}

// Procedural per-eye sniper mask. The target port does not yet expose a safe
// RenderWare HUD-texture bridge to the OpenXR swapchains, so draw the essential
// optical crop + centre reticle directly after the eye blit. This avoids a broad
// CHud hook while keeping the reticle in the same ring epoch as the zoomed world.
GLuint g_scopeProg = 0, g_scopeVbo = 0;
GLint g_scopeAspectLoc = -1;

void EnsureScopeProgram() {
    if (g_scopeProg) return;
    const char* vs = "#version 300 es\n"
                     "layout(location=0) in vec2 aPos;\n"
                     "out vec2 vNdc;\n"
                     "void main(){ vNdc=aPos; gl_Position=vec4(aPos,0.0,1.0); }\n";
    const char* fs = "#version 300 es\n"
                     "precision highp float;\n"
                     "in vec2 vNdc; uniform float uAspect; out vec4 o;\n"
                     "void main(){\n"
                     "  vec2 p=vec2(vNdc.x*uAspect,vNdc.y);\n"
                     "  float r=length(p); const float radius=0.82;\n"
                     "  if(r>radius){ o=vec4(0.0,0.0,0.0,1.0); return; }\n"
                     "  if(abs(r-radius)<0.010){ o=vec4(0.0,0.0,0.0,1.0); return; }\n"
                     "  float ax=abs(p.x), ay=abs(p.y);\n"
                     "  bool h=ay<0.008 && ax>0.045 && r<radius-0.012;\n"
                     "  bool v=ax<0.008 && ay>0.045 && r<radius-0.012;\n"
                     "  if(h||v){\n"
                     "    bool inner=(h&&ay<0.003)||(v&&ax<0.003);\n"
                     "    o=inner?vec4(0.0,0.0,0.0,1.0):vec4(1.0,1.0,1.0,1.0); return;\n"
                     "  }\n"
                     "  if(r<0.017){ o=r<0.007?vec4(1.0,1.0,1.0,1.0):vec4(0.0,0.0,0.0,1.0); return; }\n"
                     "  discard;\n"
                     "}\n";
    const GLuint v = CompileHandShader(GL_VERTEX_SHADER, vs);
    const GLuint f = CompileHandShader(GL_FRAGMENT_SHADER, fs);
    g_scopeProg = glCreateProgram();
    glAttachShader(g_scopeProg, v);
    glAttachShader(g_scopeProg, f);
    glLinkProgram(g_scopeProg);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = 0;
    glGetProgramiv(g_scopeProg, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512]{};
        glGetProgramInfoLog(g_scopeProg, sizeof(log), nullptr, log);
        LOGE("[scope] overlay link failed: %s", log);
        glDeleteProgram(g_scopeProg);
        g_scopeProg = 0;
        return;
    }
    g_scopeAspectLoc = glGetUniformLocation(g_scopeProg, "uAspect");
    constexpr float vertices[] = {
        -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
    };
    glGenBuffers(1, &g_scopeVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_scopeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    LOGI("[scope] per-eye overlay ready");
}

void DrawScopeOverlay(int width, int height,
                      const scopeaim::VisualState& scope) {
    if (!scope.active || width <= 0 || height <= 0)
        return;
    EnsureScopeProgram();
    if (!g_scopeProg || !g_scopeVbo) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, width, height);
    glUseProgram(g_scopeProg);
    glUniform1f(g_scopeAspectLoc,
                static_cast<float>(width) / static_cast<float>(height));
    glBindBuffer(GL_ARRAY_BUFFER, g_scopeVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

// Draw the holster marker cubes (uses the depth already set up by the hand pass).
void DrawHolsterMarkers(const float* mvp) {
    float pos[8][3]; int count;
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        count = g_holsterCount;
        for (int i = 0; i < count; ++i) { pos[i][0] = g_holsterPos[i][0]; pos[i][1] = g_holsterPos[i][1]; pos[i][2] = g_holsterPos[i][2]; }
    }
    if (count <= 0) return;
    EnsureMarkerProgram();
    const float s = 0.026f;
    int vc = 0, ic = 0;
    static const unsigned short cube[36] = {0,1,2, 2,1,3, 4,6,5, 5,6,7, 0,2,4, 4,2,6,
                                            1,5,3, 3,5,7, 0,4,1, 1,4,5, 2,3,6, 6,3,7};
    for (int m = 0; m < count; ++m) {
        const int base = vc;
        for (int i = 0; i < 8; ++i) {
            float* o = &g_markerVerts[vc * 3];
            o[0] = pos[m][0] + ((i & 1) ? s : -s);
            o[1] = pos[m][1] + ((i & 2) ? s : -s);
            o[2] = pos[m][2] + ((i & 4) ? s : -s);
            ++vc;
        }
        for (int k = 0; k < 36; ++k) g_markerIdx[ic++] = static_cast<unsigned short>(base + cube[k]);
    }
    glUseProgram(g_markerProg);
    glUniformMatrix4fv(g_markerMvpLoc, 1, GL_FALSE, mvp);
    glUniform4f(g_markerColLoc, 30 / 255.0f, 205 / 255.0f, 245 / 255.0f, 1.0f);   // VC cyan
    glBindBuffer(GL_ARRAY_BUFFER, g_markerVbo);
    glBufferData(GL_ARRAY_BUFFER, vc * 3 * sizeof(float), g_markerVerts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_markerEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ic * sizeof(unsigned short), g_markerIdx, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));
    glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, nullptr);
    glDisableVertexAttribArray(0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// Parachute brake toggles: two small handles floating at shoulder height while
// the canopy is open. Orange when free, green while that hand pulls it.
void DrawParachuteToggles(const float* mvp) {
    float pos[2][3]; bool grabbed[2]; bool visible;
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        visible = g_chuteTogglesVisible;
        std::memcpy(pos, g_chuteTogglePos, sizeof(pos));
        std::memcpy(grabbed, g_chuteToggleGrabbed, sizeof(grabbed));
    }
    if (!visible) return;
    EnsureMarkerProgram();
    static const unsigned short cube[36] = {0,1,2, 2,1,3, 4,6,5, 5,6,7, 0,2,4, 4,2,6,
                                            1,5,3, 3,5,7, 0,4,1, 1,4,5, 2,3,6, 6,3,7};
    glUseProgram(g_markerProg);
    glUniformMatrix4fv(g_markerMvpLoc, 1, GL_FALSE, mvp);
    glBindBuffer(GL_ARRAY_BUFFER, g_markerVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_markerEbo);
    for (int m = 0; m < 2; ++m) {
        // Slightly elongated vertically: reads as a hanging brake handle.
        const float sx = 0.020f, sy = 0.045f, sz = 0.020f;
        int vc = 0, ic = 0;
        for (int i = 0; i < 8; ++i) {
            float* o = &g_markerVerts[vc * 3];
            o[0] = pos[m][0] + ((i & 1) ? sx : -sx);
            o[1] = pos[m][1] + ((i & 2) ? sy : -sy);
            o[2] = pos[m][2] + ((i & 4) ? sz : -sz);
            ++vc;
        }
        for (int k = 0; k < 36; ++k)
            g_markerIdx[ic++] = static_cast<unsigned short>(cube[k]);
        if (grabbed[m])
            glUniform4f(g_markerColLoc, 90/255.0f, 230/255.0f, 110/255.0f, 1.0f);
        else
            glUniform4f(g_markerColLoc, 245/255.0f, 160/255.0f, 40/255.0f, 1.0f);
        glBufferData(GL_ARRAY_BUFFER, vc * 3 * sizeof(float), g_markerVerts,
                     GL_DYNAMIC_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ic * sizeof(unsigned short),
                     g_markerIdx, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, nullptr);
    }
    glDisableVertexAttribArray(0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// qbuild's cabin wheel is procedural because many authored DFFs bake the stock
// wheel into chassis_hi.  Draw the calibrated VR wheel from the exact same two
// sockets used by the grab solver and snapped hands, so all three always share
// one centre, radius and physical angle.
void AppendDrivingBox(float* vertices, int& vertexCount,
                      unsigned short* indices, int& indexCount,
                      XrVector3f center, XrVector3f axisX,
                      XrVector3f axisY, XrVector3f axisZ,
                      float halfX, float halfY, float halfZ) {
    axisX = vnorm(axisX); axisY = vnorm(axisY); axisZ = vnorm(axisZ);
    const int base = vertexCount;
    for (int i = 0; i < 8; ++i) {
        const XrVector3f point = vadd(center,
            vadd(vscale(axisX, (i & 1) ? halfX : -halfX),
            vadd(vscale(axisY, (i & 2) ? halfY : -halfY),
                 vscale(axisZ, (i & 4) ? halfZ : -halfZ))));
        vertices[vertexCount*3+0] = point.x;
        vertices[vertexCount*3+1] = point.y;
        vertices[vertexCount*3+2] = point.z;
        ++vertexCount;
    }
    static const unsigned short cube[36] = {
        0,1,2, 2,1,3, 4,6,5, 5,6,7, 0,2,4, 4,2,6,
        1,5,3, 3,5,7, 0,4,1, 1,4,5, 2,3,6, 6,3,7
    };
    for (unsigned short index : cube)
        indices[indexCount++] = static_cast<unsigned short>(base+index);
}

void DrawDrivingBoxes(const float* mvp, const float* vertices, int vertexCount,
                      const unsigned short* indices, int indexCount,
                      float red, float green, float blue, float alpha = 1.0f) {
    if (vertexCount <= 0 || indexCount <= 0) return;
    EnsureMarkerProgram();
    glUseProgram(g_markerProg);
    glUniformMatrix4fv(g_markerMvpLoc, 1, GL_FALSE, mvp);
    glUniform4f(g_markerColLoc, red, green, blue, alpha);
    glBindBuffer(GL_ARRAY_BUFFER, g_markerVbo);
    glBufferData(GL_ARRAY_BUFFER, vertexCount*3*sizeof(float), vertices,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_markerEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 indexCount*sizeof(unsigned short), indices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float),
                          reinterpret_cast<void*>(0));
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, nullptr);
    glDisableVertexAttribArray(0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void DrawImmersiveDrivingWheel(const float* mvp,
                               const driving::WheelVisualState& wheel) {
    if (!wheel.active) return;
    const XrVector3f center{wheel.center[0],wheel.center[1],wheel.center[2]};
    // The chord is valid for both the circular car wheel and bike bars. Bike
    // hand frames deliberately face their palms inward, so handleRight itself
    // is not the visual bar direction there.
    const XrVector3f leftHandle{wheel.handlePosition[0][0],
                                wheel.handlePosition[0][1],
                                wheel.handlePosition[0][2]};
    const XrVector3f rightHandle{wheel.handlePosition[1][0],
                                 wheel.handlePosition[1][1],
                                 wheel.handlePosition[1][2]};
    const XrVector3f wheelRight = vnorm(vsub(rightHandle,leftHandle));
    const XrVector3f wheelUp = wheel.bike
        ? vnorm({wheel.handleForward[1][0],wheel.handleForward[1][1],
                 wheel.handleForward[1][2]})
        : vnorm({wheel.handleUp[1][0],wheel.handleUp[1][1],
                 wheel.handleUp[1][2]});
    const XrVector3f normal = vnorm({wheel.normal[0],wheel.normal[1],
                                     wheel.normal[2]});
    // Vice City renders a solid virtual rim only for cars. Bike controls are
    // represented by the real model and optional grip sockets; drawing our own
    // bar/stem produced the floating "stick" reported in SA.
    if (wheel.visible && !wheel.bike) {
        constexpr int segments = 24;
        constexpr int boxes = segments+6;
        float vertices[boxes*8*3]{};
        unsigned short indices[boxes*36]{};
        int vertexCount=0,indexCount=0;
        // Planes render a ram-horn yoke: the lower half arc plus two upright
        // grip horns at its ends. Cars/boats keep the full closed ring.
        const bool yoke = wheel.yoke;
        const float arcStart = yoke ? 3.14159265f : 0.0f;
        const float arcSweep = yoke ? 3.14159265f : 6.2831853071795864769f;
        for (int segment=0; segment<segments; ++segment) {
            const float a0=arcStart+arcSweep*segment/segments;
            const float a1=arcStart+arcSweep*(segment+1)/segments;
            const XrVector3f start=vadd(center,
                vadd(vscale(wheelRight,std::cos(a0)*wheel.radius),
                     vscale(wheelUp,std::sin(a0)*wheel.radius)));
            const XrVector3f end=vadd(center,
                vadd(vscale(wheelRight,std::cos(a1)*wheel.radius),
                     vscale(wheelUp,std::sin(a1)*wheel.radius)));
            const XrVector3f delta=vsub(end,start);
            const float length=std::sqrt(std::max(0.0f,vdot(delta,delta)));
            if (length<0.001f) continue;
            AppendDrivingBox(vertices,vertexCount,indices,indexCount,
                vscale(vadd(start,end),0.5f),vscale(delta,1.0f/length),
                vnorm(vsub(vscale(vadd(start,end),0.5f),center)),normal,
                length*0.52f,0.014f,0.012f);
        }
        if (yoke) {
            // Upright grip horns from the arc's horizontal ends, a short
            // centre column down to the arc and a wide hub bar.
            for (int side=0; side<2; ++side) {
                const XrVector3f base=vadd(center,
                    vscale(wheelRight,(side==0?-1.0f:1.0f)*wheel.radius));
                const float hornLength=wheel.radius*0.62f;
                AppendDrivingBox(vertices,vertexCount,indices,indexCount,
                    vadd(base,vscale(wheelUp,hornLength*0.5f)),wheelUp,
                    wheelRight,normal,hornLength*0.5f,0.016f,0.013f);
            }
            const float columnLength=wheel.radius*0.95f;
            AppendDrivingBox(vertices,vertexCount,indices,indexCount,
                vadd(center,vscale(wheelUp,-columnLength*0.5f)),wheelUp,
                wheelRight,normal,columnLength*0.5f,0.014f,0.012f);
            AppendDrivingBox(vertices,vertexCount,indices,indexCount,center,
                wheelRight,wheelUp,normal,wheel.radius*0.55f,0.030f,0.016f);
        } else {
            for (int spoke=0; spoke<3; ++spoke) {
                const float angle=6.2831853071795864769f*spoke/3.0f;
                const XrVector3f direction=vnorm(vadd(
                    vscale(wheelRight,std::cos(angle)),
                    vscale(wheelUp,std::sin(angle))));
                const float length=wheel.radius*0.72f;
                AppendDrivingBox(vertices,vertexCount,indices,indexCount,
                    vadd(center,vscale(direction,length*0.5f)),direction,
                    vnorm(vcross(normal,direction)),normal,
                    length*0.5f,0.012f,0.010f);
            }
            AppendDrivingBox(vertices,vertexCount,indices,indexCount,center,
                wheelRight,wheelUp,normal,0.050f,0.050f,0.018f);
        }
        DrawDrivingBoxes(mvp,vertices,vertexCount,indices,indexCount,
                         40.0f/255.0f,42.0f/255.0f,47.0f/255.0f);
    }

    // Approach markers remain useful when the player hides the ring. Grabbed
    // sockets are green; reachable but free sockets use the VC cyan highlight.
    for (int hand=0; hand<2; ++hand) {
        if (!wheel.markerVisible[hand]) continue;
        float vertices[8*3]{}; unsigned short indices[36]{};
        int vertexCount=0,indexCount=0;
        AppendDrivingBox(vertices,vertexCount,indices,indexCount,
            {wheel.handlePosition[hand][0],wheel.handlePosition[hand][1],
             wheel.handlePosition[hand][2]},wheelRight,wheelUp,normal,
            0.035f,0.035f,0.055f);
        if (wheel.grabbed[hand])
            DrawDrivingBoxes(mvp,vertices,vertexCount,indices,indexCount,
                             70.0f/255.0f,245.0f/255.0f,125.0f/255.0f);
        else
            DrawDrivingBoxes(mvp,vertices,vertexCount,indices,indexCount,
                             35.0f/255.0f,180.0f/255.0f,245.0f/255.0f);
    }
}

void DrawThrowableTrajectories(const float* mvp,
                               const ThrowableTrajectory trajectory[2]) {
    if (!trajectory) return;
    for (int hand = 0; hand < 2; ++hand) {
        const ThrowableTrajectory& arc = trajectory[hand];
        if (arc.count <= 1) continue;
        float vertices[(kMaxThrowableTrajectoryPoints - 1) * 8 * 3]{};
        unsigned short indices[(kMaxThrowableTrajectoryPoints - 1) * 36]{};
        int vertexCount = 0, indexCount = 0;
        for (int i = 1; i < arc.count; ++i) {
            const XrVector3f start = arc.points[i - 1];
            const XrVector3f end = arc.points[i];
            const XrVector3f delta = vsub(end, start);
            const float length = std::sqrt(std::max(0.0f, vdot(delta, delta)));
            if (length < 0.001f) continue;
            const XrVector3f direction = vscale(delta, 1.0f / length);
            XrVector3f right = vcross(direction, {0.0f, 1.0f, 0.0f});
            if (vdot(right, right) < 0.0001f)
                right = vcross(direction, {1.0f, 0.0f, 0.0f});
            right = vnorm(right);
            const XrVector3f up = vnorm(vcross(right, direction));
            AppendDrivingBox(vertices, vertexCount, indices, indexCount,
                vscale(vadd(start, end), 0.5f), right, up, direction,
                0.0045f, 0.0045f, length * 0.5f);
        }
        glDepthMask(GL_FALSE);
        DrawDrivingBoxes(mvp, vertices, vertexCount, indices, indexCount,
                         45.0f / 255.0f, 225.0f / 255.0f, 1.0f);
        if (arc.hit) {
            float hitVertices[8 * 3]{};
            unsigned short hitIndices[36]{};
            int hitVertexCount = 0, hitIndexCount = 0;
            AppendDrivingBox(hitVertices, hitVertexCount,
                hitIndices, hitIndexCount, arc.points[arc.count - 1],
                {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                {0.0f, 0.0f, 1.0f}, 0.022f, 0.022f, 0.022f);
            DrawDrivingBoxes(mvp, hitVertices, hitVertexCount,
                hitIndices, hitIndexCount, 1.0f, 185.0f / 255.0f,
                45.0f / 255.0f);
        }
        glDepthMask(GL_TRUE);
    }
}

// Thin 3D laser prism. GLES line width is implementation-dependent, so a real
// box remains visible on Quest while still respecting the copied world/weapon
// depth. It never writes depth and therefore cannot punch holes in later hands.
int SnapshotBulletTracers(BulletTracer out[kMaxBulletTracers]) {
    if (!out) return 0;
    const std::uint64_t now = MonotonicNowNs();
    int count = 0;
    std::lock_guard<std::mutex> lock(g_bulletTracerMutex);
    for (BulletTracer& tracer : g_bulletTracers) {
        if (!tracer.valid) continue;
        if (tracer.expiresNs <= now) {
            tracer.valid = false;
            continue;
        }
        out[count++] = tracer;
    }
    return count;
}

int SnapshotObjectiveMarkers(
    ObjectiveMarkerVisual out[kMaxObjectiveMarkers]) {
    if (!out) return 0;
    const std::uint64_t now = MonotonicNowNs();
    int count = 0;
    std::lock_guard<std::mutex> lock(g_objectiveMarkerMutex);
    for (ObjectiveMarkerVisual& marker : g_objectiveMarkers) {
        if (!marker.valid) continue;
        if (marker.expiresNs <= now) {
            marker.valid = false;
            continue;
        }
        if (marker.stockCone ? !hud::ObjectiveMarkersIncludeOriginal()
                             : !hud::ObjectiveMarkersIncludeHighlight()) {
            continue;
        }
        out[count++] = marker;
    }
    return count;
}

void DrawStockObjectiveArrow(const float* mvp,
                             const ObjectiveMarkerVisual& marker) {
    // Exact low-poly diamond_3 silhouette used by the mobile game: one bright
    // downward tip and an 11-vertex darker translucent ring, with no cap.
    constexpr int ringCount = 11;
    constexpr float assetRadius = 0.443975f;
    constexpr float assetRingY = 0.288632f;
    constexpr float assetTipY = -1.021123f;
    constexpr float ringShade = 57.0f / 255.0f;
    constexpr float ringAlpha = 127.0f / 255.0f;
    float vertices[(ringCount + 1) * 5]{};
    unsigned short indices[ringCount * 3]{};
    const float scale = marker.radius / assetRadius;

    vertices[0] = marker.center.x;
    vertices[1] = marker.center.y + assetTipY * scale;
    vertices[2] = marker.center.z;
    vertices[3] = 1.0f;
    vertices[4] = 1.0f;
    for (int ring = 0; ring < ringCount; ++ring) {
        const float angle = 6.2831853071795864769f * ring / ringCount;
        float* vertex = &vertices[(ring + 1) * 5];
        vertex[0] = marker.center.x + std::cos(angle) * marker.radius;
        vertex[1] = marker.center.y + assetRingY * scale;
        vertex[2] = marker.center.z + std::sin(angle) * marker.radius;
        vertex[3] = ringShade;
        vertex[4] = ringAlpha;
        indices[ring * 3 + 0] = 0;
        indices[ring * 3 + 1] = static_cast<unsigned short>(ring + 1);
        indices[ring * 3 + 2] = static_cast<unsigned short>(
            1 + (ring + 1) % ringCount);
    }

    EnsureMarkerProgram();
    EnsureArrowProgram();
    GLuint vbo = 0, ebo = 0;
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glUseProgram(g_arrowProg);
    glUniformMatrix4fv(g_arrowMvpLoc, 1, GL_FALSE, mvp);
    glUniform4f(g_arrowColLoc, marker.red / 255.0f,
               marker.green / 255.0f, marker.blue / 255.0f, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glDrawElements(GL_TRIANGLES, ringCount * 3, GL_UNSIGNED_SHORT, nullptr);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &vbo);
}

// A restrained depth-tested cage around the exact object targeted by the
// original CRadar arrow. It does not show through walls and never writes depth,
// so it reads as an optional target highlight without obscuring the model.
void DrawObjectiveHighlights(const float* mvp,
                             const ObjectiveMarkerVisual* markers,
                             int count) {
    if (!markers || count <= 0) return;
    count = std::min(count, kMaxObjectiveMarkers);
    const float timeSeconds = static_cast<float>(
        MonotonicNowNs() % 2000000000ULL) / 1000000000.0f;
    const float pulse = 0.5f + 0.5f * std::sin(
        timeSeconds * 6.2831853071795864769f);

    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    for (int markerIndex = 0; markerIndex < count; ++markerIndex) {
        const ObjectiveMarkerVisual& marker = markers[markerIndex];
        if (marker.stockCone) {
            DrawStockObjectiveArrow(mvp, marker);
            continue;
        }
        constexpr int segments = 16;
        constexpr int ringCount = 3;
        constexpr int railCount = 4;
        constexpr int boxCount = segments * ringCount + railCount;
        float vertices[boxCount * 8 * 3]{};
        unsigned short indices[boxCount * 36]{};
        int vertexCount = 0, indexCount = 0;
        const float halfHeight = marker.height * 0.5f;

        for (int ring = 0; ring < ringCount; ++ring) {
            const float y = marker.center.y +
                (-halfHeight + marker.height * ring / (ringCount - 1));
            for (int segment = 0; segment < segments; ++segment) {
                const float a0 = 6.2831853071795864769f * segment / segments;
                const float a1 = 6.2831853071795864769f *
                                 (segment + 1) / segments;
                const XrVector3f start{
                    marker.center.x + std::cos(a0) * marker.radius, y,
                    marker.center.z + std::sin(a0) * marker.radius};
                const XrVector3f end{
                    marker.center.x + std::cos(a1) * marker.radius, y,
                    marker.center.z + std::sin(a1) * marker.radius};
                const XrVector3f delta = vsub(end, start);
                const float length = std::sqrt(std::max(0.0f,
                                                        vdot(delta, delta)));
                if (length < 0.001f) continue;
                const XrVector3f midpoint = vscale(vadd(start, end), 0.5f);
                const XrVector3f radial = vnorm({
                    midpoint.x - marker.center.x, 0.0f,
                    midpoint.z - marker.center.z});
                AppendDrivingBox(vertices, vertexCount, indices, indexCount,
                    midpoint, vscale(delta, 1.0f / length),
                    {0.0f, 1.0f, 0.0f}, radial,
                    length * 0.52f, 0.008f, 0.008f);
            }
        }
        for (int rail = 0; rail < railCount; ++rail) {
            const float angle = 6.2831853071795864769f * rail / railCount;
            const XrVector3f radial{std::cos(angle), 0.0f, std::sin(angle)};
            const XrVector3f tangent{-radial.z, 0.0f, radial.x};
            const XrVector3f railCenter = vadd(
                marker.center, vscale(radial, marker.radius));
            AppendDrivingBox(vertices, vertexCount, indices, indexCount,
                railCenter, {0.0f, 1.0f, 0.0f}, radial, tangent,
                halfHeight, 0.008f, 0.008f);
        }

        const float red = std::max(0.24f, marker.red / 255.0f);
        const float green = std::max(0.24f, marker.green / 255.0f);
        const float blue = std::max(0.24f, marker.blue / 255.0f);
        DrawDrivingBoxes(mvp, vertices, vertexCount, indices, indexCount,
                         red, green, blue, 0.16f + 0.12f * pulse);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void DrawWeaponLaser(const float* mvp, const LaserRay& ray) {
    if (!ray.valid) return;
    EnsureMarkerProgram();
    const XrVector3f d = vnorm(ray.direction);
    XrVector3f side = vcross(d, {0.0f, 1.0f, 0.0f});
    if (vdot(side, side) < 1e-5f) side = vcross(d, {1.0f, 0.0f, 0.0f});
    side = vnorm(side);
    const XrVector3f beamUp = vnorm(vcross(side, d));
    constexpr float halfWidth = 0.0020f;
    const XrVector3f end = vadd(ray.origin, vscale(d, 60.0f));
    float verts[8 * 3];
    for (int i = 0; i < 8; ++i) {
        const XrVector3f center = (i & 4) ? end : ray.origin;
        const float sx = (i & 1) ? halfWidth : -halfWidth;
        const float sy = (i & 2) ? halfWidth : -halfWidth;
        const XrVector3f p = vadd(center, vadd(vscale(side, sx), vscale(beamUp, sy)));
        verts[i * 3 + 0] = p.x; verts[i * 3 + 1] = p.y; verts[i * 3 + 2] = p.z;
    }
    static const unsigned short idx[36] = {
        0,1,2, 2,1,3, 4,6,5, 5,6,7, 0,2,4, 4,2,6,
        1,5,3, 3,5,7, 0,4,1, 1,4,5, 2,3,6, 6,3,7
    };
    glDepthMask(GL_FALSE);
    glUseProgram(g_markerProg);
    glUniformMatrix4fv(g_markerMvpLoc, 1, GL_FALSE, mvp);
    glUniform4f(g_markerColLoc, 1.0f, 0.035f, 0.025f, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, g_markerVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_markerEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
    glDisableVertexAttribArray(0);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// A short-lived, depth-tested VR streak. It deliberately keeps the prior long
// custom tracer's readability, but uses a soft amber glow plus a narrow white
// core instead of an opaque yellow box that looked like a second laser.
void DrawBulletTracers(const float* mvp, XrVector3f eyePosition,
                       const BulletTracer* tracers, int count) {
    if (!tracers || count <= 0) return;
    count = std::min(count, kMaxBulletTracers);
    const std::uint64_t now = MonotonicNowNs();
    static const unsigned short indices[6] = {0, 1, 2, 2, 1, 3};

    EnsureMarkerProgram();
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glUseProgram(g_markerProg);
    glUniformMatrix4fv(g_markerMvpLoc, 1, GL_FALSE, mvp);
    glBindBuffer(GL_ARRAY_BUFFER, g_markerVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_markerEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));

    for (int t = 0; t < count; ++t) {
        const BulletTracer& tracer = tracers[t];
        const XrVector3f delta = vsub(tracer.end, tracer.start);
        const float lengthSq = vdot(delta, delta);
        if (!std::isfinite(lengthSq) || lengthSq < 0.0001f ||
            tracer.expiresNs <= tracer.bornNs || now >= tracer.expiresNs) {
            continue;
        }
        const XrVector3f direction = vnorm(delta);
        XrVector3f side = vcross(vnorm(vsub(tracer.start, eyePosition)),
                                 direction);
        if (vdot(side, side) < 1.0e-5f)
            side = vcross(direction, {0.0f, 1.0f, 0.0f});
        if (vdot(side, side) < 1.0e-5f)
            side = vcross(direction, {1.0f, 0.0f, 0.0f});
        side = vnorm(side);

        const float age = static_cast<float>(now - tracer.bornNs) /
                          static_cast<float>(tracer.expiresNs - tracer.bornNs);
        const float remaining = 1.0f - std::clamp(age, 0.0f, 1.0f);
        // Hold the full ray briefly, then fade quickly. This remains readable at
        // Quest resolution while still looking like a gunshot flash, not a laser.
        const float fade = remaining * remaining;
        const float widths[2] = {0.022f, 0.006f};
        const float alphas[2] = {0.42f * fade, 0.95f * fade};
        for (int layer = 0; layer < 2; ++layer) {
            const XrVector3f offset = vscale(side, widths[layer]);
            const XrVector3f p[4] = {
                vsub(tracer.start, offset), vadd(tracer.start, offset),
                vsub(tracer.end, offset),   vadd(tracer.end, offset),
            };
            float verts[12]{};
            for (int i = 0; i < 4; ++i) {
                verts[i * 3 + 0] = p[i].x;
                verts[i * 3 + 1] = p[i].y;
                verts[i * 3 + 2] = p[i].z;
            }
            if (layer == 0)
                glUniform4f(g_markerColLoc, 1.0f, 0.68f, 0.18f, alphas[layer]);
            else
                glUniform4f(g_markerColLoc, 1.0f, 0.96f, 0.72f, alphas[layer]);
            glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        }
    }

    glDisableVertexAttribArray(0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

// ---- Stage 1 weapon rendering: object-space geometry -> right-hand pose ----
// The active weapon's object-space verts (published by the GameThread) are
// transformed here into XR world space at the hand pose and drawn flat, in the
// SAME eye FBO / mvp / depth pass as the hand, so the two can't slip apart.
constexpr int kWpnMaxV = 8000;
float          g_wVerts[kWpnMaxV * 3];   // xyz only (Stage 1: no normals/uv)
unsigned short g_wIdx[kWpnMaxV * 3];
int            g_wVc = 0, g_wIc = 0;
GLuint         g_wpnVbo = 0, g_wpnEbo = 0;

// Weapon calibration: object space -> hand frame. Live-tunable (edit + rebuild);
// refined in Stage 2 once we can see it. rot=0 -> object axes map straight onto
// (right, up, forward). Object units are ~metres already (RW ~1u = 1m).
int   g_wpnHand      = 1;                       // 1 = right hand
float g_wpnScale     = 1.0f;
float g_wpnRotDeg[3] = {0.0f, 0.0f, 0.0f};      // Euler XYZ applied to object vert
float g_wpnTrans[3]  = {0.0f, 0.0f, 0.0f};      // (right, up, forward) offset, metres

void EulerXYZ(const float deg[3], float R[9]) {
    const float cx = std::cos(deg[0] * 0.0174532925f), sx = std::sin(deg[0] * 0.0174532925f);
    const float cy = std::cos(deg[1] * 0.0174532925f), sy = std::sin(deg[1] * 0.0174532925f);
    const float cz = std::cos(deg[2] * 0.0174532925f), sz = std::sin(deg[2] * 0.0174532925f);
    // Rz * Ry * Rx
    R[0] = cz * cy;                 R[1] = cz * sy * sx - sz * cx;  R[2] = cz * sy * cx + sz * sx;
    R[3] = sz * cy;                 R[4] = sz * sy * sx + cz * cx;  R[5] = sz * sy * cx - cz * sx;
    R[6] = -sy;                     R[7] = cy * sx;                 R[8] = cy * cx;
}

// Build the weapon's world-space triangle mesh at the right hand's pose. Mirrors
// the basis math in BuildHandMeshes for the same hand so the gun sits in the same
// frame as the hand mesh. Runs on the present thread, once per frame.
void BuildWeaponMesh(const HandPose hp[2]) {
    g_wVc = 0; g_wIc = 0;
    std::vector<float> obj; std::vector<unsigned short> idx;
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        if (g_wpnObjVerts.empty() || g_wpnObjIdx.empty()) return;
        obj = g_wpnObjVerts; idx = g_wpnObjIdx;      // small copy out under lock
    }
    const int hand = (g_wpnHand == 0) ? 0 : 1;
    {
        static int dbg = 0;
        if (dbg < 4) { ++dbg; LOGI("[wpn.dbg] build: obj verts=%d idx=%d handValid=%d",
                                   static_cast<int>(obj.size() / 3), static_cast<int>(idx.size()), hp[hand].valid ? 1 : 0); }
    }
    if (!hp[hand].valid) return;

    const XrQuaternionf gq{hp[hand].gripOri[0], hp[hand].gripOri[1], hp[hand].gripOri[2], hp[hand].gripOri[3]};
    const XrQuaternionf aq{hp[hand].aimOri[0], hp[hand].aimOri[1], hp[hand].aimOri[2], hp[hand].aimOri[3]};
    const XrVector3f pos{hp[hand].gripPos[0], hp[hand].gripPos[1], hp[hand].gripPos[2]};
    const XrVector3f gRight = vnorm(QuatRotate(gq, {1, 0, 0}));
    const XrVector3f gFwd   = vnorm(QuatRotate(gq, {0, 0, -1}));
    const XrVector3f aimFwd = vnorm(QuatRotate(aq, {0, 0, -1}));
    const float sign = (1 - hand) == 0 ? 1.0f : -1.0f;
    XrVector3f forward = (vdot(aimFwd, aimFwd) > 0.1f) ? aimFwd : QuatRotate(gq, {0, 1, 0});
    forward = vnorm(forward);
    XrVector3f up = vscale(gRight, sign);
    up = vsub(up, vscale(forward, vdot(up, forward)));
    if (vdot(up, up) < 0.0001f) up = vscale(gRight, sign);
    up = vnorm(up);
    XrVector3f right = vnorm(vcross(up, forward));
    if (vdot(right, gFwd) < 0.0f) right = vscale(right, -1.0f);

    float R[9]; EulerXYZ(g_wpnRotDeg, R);
    const int nv = static_cast<int>(obj.size() / 3);
    for (int i = 0; i < nv && g_wVc < kWpnMaxV; ++i) {
        const float ox = obj[i * 3 + 0] * g_wpnScale;
        const float oy = obj[i * 3 + 1] * g_wpnScale;
        const float oz = obj[i * 3 + 2] * g_wpnScale;
        const float rx = R[0] * ox + R[1] * oy + R[2] * oz + g_wpnTrans[0];
        const float ry = R[3] * ox + R[4] * oy + R[5] * oz + g_wpnTrans[1];
        const float rz = R[6] * ox + R[7] * oy + R[8] * oz + g_wpnTrans[2];
        const XrVector3f wp = vadd(pos, vadd(vscale(right, rx), vadd(vscale(up, ry), vscale(forward, rz))));
        g_wVerts[g_wVc * 3 + 0] = wp.x; g_wVerts[g_wVc * 3 + 1] = wp.y; g_wVerts[g_wVc * 3 + 2] = wp.z;
        ++g_wVc;
    }
    // Filter a whole triangle at a time: if the vertex cap truncated any of a
    // triangle's three verts, drop the triangle entirely (a clean hole) rather
    // than emitting stray indices that would desync the whole triangle stream.
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (g_wIc + 3 > kWpnMaxV * 3) break;
        const unsigned short a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a < g_wVc && b < g_wVc && c < g_wVc) {
            g_wIdx[g_wIc++] = a; g_wIdx[g_wIc++] = b; g_wIdx[g_wIc++] = c;
        }
    }
}

// Draw the weapon mesh flat-shaded (orange) with the marker program, sharing the
// hand pass's depth buffer. `mvp` is the per-eye matrix already built by the hand.
void DrawWeaponForEye(const float* mvp) {
    if (g_wVc <= 0 || g_wIc <= 0) return;
    EnsureMarkerProgram();
    if (!g_wpnVbo) { glGenBuffers(1, &g_wpnVbo); glGenBuffers(1, &g_wpnEbo); }
    glUseProgram(g_markerProg);
    glUniformMatrix4fv(g_markerMvpLoc, 1, GL_FALSE, mvp);
    glUniform4f(g_markerColLoc, 245 / 255.0f, 150 / 255.0f, 30 / 255.0f, 1.0f);   // orange
    glBindBuffer(GL_ARRAY_BUFFER, g_wpnVbo);
    glBufferData(GL_ARRAY_BUFFER, g_wVc * 3 * sizeof(float), g_wVerts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_wpnEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, g_wIc * sizeof(unsigned short), g_wIdx, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));
    glDrawElements(GL_TRIANGLES, g_wIc, GL_UNSIGNED_SHORT, nullptr);
    glDisableVertexAttribArray(0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    static bool logged = false;
    if (!logged) { LOGI("[wpn] mesh drawn %d verts %d idx", g_wVc, g_wIc); logged = true; }
}

// Draw the built hand meshes into the currently-bound swapchain framebuffer for
// one eye. rp = the presented set's render pose; e = 0 left / 1 right.
void DrawHandsForEye(const XrPosef& rp, int e, int fbW, int fbH,
                     unsigned int gameDepthRbo, int gameDepthW, int gameDepthH,
                     float renderedNearZ, float renderedFarZ,
                     const LaserRay& laser, unsigned int weaponHandMask,
                     const BulletTracer* tracers, int tracerCount,
                     const ObjectiveMarkerVisual* objectiveMarkers,
                     int objectiveMarkerCount,
                     const scopeaim::VisualState& scope,
                     const driving::WheelVisualState& drivingWheel,
                     const ThrowableTrajectory throwableTrajectory[2],
                     const HandPose bakedHands[2],
                     const TwoHandVisualState& bakedTwoHand,
                     unsigned int hudSourceTex) {
    const bool trackedVisuals = !scope.active && drivingWheel.trackedHandsEnabled;
    const bool haveHands = trackedVisuals &&
                           g_hVc > 0 && g_hIc > 0 && g_handTex != 0;
    // Wrist HUD panels ride the visible wrists on foot, and the steering-
    // wheel/handlebar centre (Vice City dashboard pattern) in ANY driving
    // mode — the dash anchor does not need tracked hands. While the HUD menu
    // is open they are forced on regardless of preset so they can be
    // calibrated.
    const bool hudCalibrating = g_hudActive.load(std::memory_order_relaxed);
    const bool dashAnchored =
        drivingWheel.active || drivingWheel.dashAnchorValid;
    const bool wristWanted = !scope.active && hudSourceTex != 0 &&
        (hud::ShouldRenderWristHud() || hudCalibrating) &&
        (dashAnchored ||
         (trackedVisuals &&
          (bakedHands[0].valid || bakedHands[1].valid)));
    if (!haveHands && (!trackedVisuals || g_holsterCount <= 0) &&
        (!trackedVisuals || g_wVc <= 0) &&
        (!trackedVisuals || !laser.valid) &&
        (!trackedVisuals || !drivingWheel.active) &&
        (!trackedVisuals || (throwableTrajectory[0].count <= 1 &&
                             throwableTrajectory[1].count <= 1)) &&
        !wristWanted &&
        tracerCount <= 0 && objectiveMarkerCount <= 0) return;
    if (haveHands) EnsureHandProgram();

    const float sign = (e == 0) ? -1.0f : 1.0f;
    const XrVector3f rr = QuatRotate(rp.orientation, {sign * kStereoHalfIpd, 0, 0});
    const XrVector3f eyePos{rp.position.x + rr.x, rp.position.y + rr.y, rp.position.z + rr.z};
    float view[16], proj[16], mvp[16];
    MatView(view, rp.orientation, eyePos);
    const float nearZ = std::max(0.001f, renderedNearZ);
    const float farZ = std::max(nearZ + 1.0f, renderedFarZ);
    // Tracers are world geometry drawn after the blit. They must use the same
    // narrowed frustum as the scoped eye pixels; the projection layer itself
    // still advertises the normal tangents below.
    const float zoom = scope.active ? std::max(1.0f, scope.zoom) : 1.0f;
    MatPersp(proj, g_stereoTanX.load(std::memory_order_relaxed) / zoom,
             g_stereoTanY.load(std::memory_order_relaxed) / zoom,
             nearZ, farZ);
    MatMul(mvp, proj, view);

    const bool sourceIsRbo =
        gameDepthRbo != 0 && glIsRenderbuffer(gameDepthRbo) == GL_TRUE;
    GLint sourceDepthFormat = 0;
    GLint sourceDepthBits = 0;
    GLenum sourceQueryError = GL_NO_ERROR;
    const bool sourceFormatValid = sourceIsRbo && QueryGameDepthFormat(
        gameDepthRbo, sourceDepthFormat, sourceDepthBits, sourceQueryError);

    // Before the first eye RBO becomes share-visible, retain the proven D16
    // fallback. As soon as a real source is queryable, allocate the late-hand
    // depth buffer with that exact format (D24 on the requested retail path).
    const GLint desiredDepthFormat = sourceFormatValid
        ? sourceDepthFormat
        : (g_expectedGameDepthFormat != 0
               ? g_expectedGameDepthFormat
               : static_cast<GLint>(GL_DEPTH_COMPONENT16));
    GLenum handAllocationError = GL_NO_ERROR;
    const bool handDepthReady = EnsureHandDepthStorage(
        fbW, fbH, desiredDepthFormat, handAllocationError);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.framebuffer);
    glViewport(0, 0, fbW, fbH);

    if (!handDepthReady) {
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, 0);
        g_gameDepthUsable.store(false, std::memory_order_release);
        static bool allocationFailedLogged = false;
        if (!allocationFailedLogged) {
            allocationFailedLogged = true;
            LOGW("[hands] depth storage unavailable desired=0x%x err=0x%x; late overlays skipped",
                 desiredDepthFormat, handAllocationError);
        }
        return;
    }

    // The game eye target already contains world + opaque weapon depth. Copy its
    // shared renderbuffer into an identically formatted local buffer, then
    // render hands against that copy. This reproduces the PC/VC order without
    // writing back into a ring target that the game's render thread will soon
    // reuse.
    bool usingGameDepth = false;
    GLenum readStatus = 0, drawStatus = 0, copyError = GL_NO_ERROR;
    if (sourceFormatValid && g_handDepthFormat == sourceDepthFormat &&
        g_handDepthBits == sourceDepthBits) {
        while (glGetError() != GL_NO_ERROR) {}
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.eyeReadFramebuffer);
        glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, gameDepthRbo);
        readStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.framebuffer);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, g_handDepthRbo);
        drawStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (readStatus == GL_FRAMEBUFFER_COMPLETE && drawStatus == GL_FRAMEBUFFER_COMPLETE) {
            // The game eye may be 60..100% of the OpenXR swapchain. Upscale its
            // depth with NEAREST into the late-hand target so hand/weapon/world
            // occlusion remains valid at every render-scale setting.
            glBlitFramebuffer(0, 0, gameDepthW, gameDepthH, 0, 0, fbW, fbH,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            copyError = glGetError();
            usingGameDepth = copyError == GL_NO_ERROR;
        }
        // Container FBOs are context-local; remove the shared attachment after the
        // copy while leaving the RenderWare-owned renderbuffer itself untouched.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.eyeReadFramebuffer);
        glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.framebuffer);
        static bool depthLogged = false;
        if (usingGameDepth && !depthLogged) {
            depthLogged = true;
            GLint framebufferDepthBits = 0;
            glGetIntegerv(GL_DEPTH_BITS, &framebufferDepthBits);
            LOGI("[hands] game depth copied rbo=%u src=0x%x/%d hand=0x%x/%d fboBits=%d read=0x%x draw=0x%x err=0x%x near=%.5f far=%.1f",
                 gameDepthRbo, sourceDepthFormat, sourceDepthBits,
                 g_handDepthFormat, g_handDepthBits, framebufferDepthBits,
                 readStatus, drawStatus, copyError, nearZ, farZ);
        }
        static bool depthFailedLogged = false;
        if (!usingGameDepth && !depthFailedLogged) {
            depthFailedLogged = true;
            LOGW("[hands] game depth copy failed rbo=%u src=0x%x/%d hand=0x%x/%d queryErr=0x%x allocErr=0x%x read=0x%x draw=0x%x blitErr=0x%x; held hand hidden",
                 gameDepthRbo, sourceDepthFormat, sourceDepthBits,
                 g_handDepthFormat, g_handDepthBits, sourceQueryError,
                 handAllocationError, readStatus, drawStatus, copyError);
        }
    } else if (sourceIsRbo) {
        static bool formatFailedLogged = false;
        if (!formatFailedLogged) {
            formatFailedLogged = true;
            LOGW("[hands] game depth format rejected rbo=%u src=0x%x/%d hand=0x%x/%d queryErr=0x%x; held hand hidden",
                 gameDepthRbo, sourceDepthFormat, sourceDepthBits,
                 g_handDepthFormat, g_handDepthBits, sourceQueryError);
        }
    }
    g_gameDepthUsable.store(usingGameDepth, std::memory_order_release);
    if (!usingGameDepth) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.framebuffer);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, g_handDepthRbo);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE);
    glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);

    // qbuild order: the virtual cabin control is world-depth tested first, then
    // the closed hands are drawn over it at the exact animated rim sockets.
    if (trackedVisuals && drivingWheel.active)
        DrawImmersiveDrivingWheel(mvp, drivingWheel);
    // Never turn the optional highlight into a wallhack: if the shared game
    // depth cannot be copied this frame, omit it instead of drawing on top.
    if (usingGameDepth)
        DrawObjectiveHighlights(mvp, objectiveMarkers, objectiveMarkerCount);

    if (haveHands) {
        glUseProgram(g_handProg);
        glUniformMatrix4fv(g_handMvpLoc, 1, GL_FALSE, mvp);
        glUniform1f(g_handSkinLoc,
                    appearance::GetHandSkin() == appearance::HAND_SKIN_DARK
                        ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_handTex);
        glUniform1i(g_handTexLoc, 0);
        glBindBuffer(GL_ARRAY_BUFFER, g_handVbo);
        glBufferData(GL_ARRAY_BUFFER, g_hVc * 8 * sizeof(float), g_hVerts, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_handEbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, g_hIc * sizeof(unsigned short), g_hIdx, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
        for (int hand = 0; hand < 2; ++hand) {
            if (!usingGameDepth && (weaponHandMask & (1u << hand)) != 0) continue;
            if (g_hIndexCount[hand] <= 0) continue;
            const std::uintptr_t byteOffset =
                static_cast<std::uintptr_t>(g_hFirstIndex[hand]) * sizeof(unsigned short);
            glDrawElements(GL_TRIANGLES, g_hIndexCount[hand], GL_UNSIGNED_SHORT,
                           reinterpret_cast<void*>(byteOffset));
        }
        glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (trackedVisuals) {
        DrawHolsterMarkers(mvp);   // cyan cubes at the holster anchors (shares this depth pass)
        DrawParachuteToggles(mvp); // canopy brake handles while parachuting
        DrawWeaponLaser(mvp, laser); // calibrated red beam, occluded by world + weapon
        DrawWeaponForEye(mvp);     // active weapon mesh pinned to the hand (Stage 1)
        DrawThrowableTrajectories(mvp, throwableTrajectory);
    }
    DrawBulletTracers(mvp, eyePos, tracers, tracerCount); // original-SA short streak, same stereo depth
    // IMMERSIVE preset: radar + status crops worn on the wrists (or the
    // dashboard while driving), same baked poses as the hand meshes, same
    // depth pass (arm/weapon occlude the panel).
    if (wristWanted)
        DrawWristPanelsForEye(mvp, bakedHands, bakedTwoHand, drivingWheel,
                              hudSourceTex, rp, weaponHandMask);

    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    static bool logged = false;
    if (!logged) { LOGI("[hands] mesh drawn %d verts %d idx", g_hVc, g_hIc); logged = true; }
}

// True per-eye stereo present: blit each eye's game-rendered texture into that
// eye's swapchain and describe it as a world-locked PROJECTION view carrying the
// exact pose+frustum the eye was rendered at. Because it is a projection layer in
// LOCAL space (not a head-locked billboard), the compositor's TimeWarp reprojects
// the baked frame to the live head pose — correct at ALL depths, since rotation
// has no parallax. That is what removes the rotation ghosting AND the flat-quad
// depth swim. Fills `layer` (a projection layer) and its two `views`.
bool RenderStereoEyeProjection(XrCompositionLayerProjection& layer,
                               XrCompositionLayerProjectionView views[2],
                               int& submittedSequence,
                               bool& generationRace,
                               double& submittedSequenceAgeMs,
                               FxaaFrameStats& fxaaStats) {
    if (s.swapchains.size() < 2) return false;

    // Sample ONE ring set for both eyes so the pair is always temporally matched.
    int seq = -1;
    const int set = StereoReadSet(seq);
    if (set < 0) return false;
    const int expectedGeneration = seq - kStereoReadLag;
    if (g_stereoGeneration[set].load(std::memory_order_acquire) !=
        expectedGeneration) {
        generationRace = true;
        return false;
    }
    const std::uint64_t publishedNs =
        g_stereoPublishedNs[set].load(std::memory_order_relaxed);
    const std::uint64_t sampledNs = MonotonicNowNs();
    if (publishedNs > 0 && sampledNs >= publishedNs) {
        submittedSequenceAgeMs =
            static_cast<double>(sampledNs - publishedNs) / 1e6;
    }
    // These relaxed metadata loads are ordered by StereoReadSet's acquire of the
    // sequence published last by SetStereoEyeTextures.
    const int ew = g_stereoEyeW.load(std::memory_order_relaxed);
    const int eh = g_stereoEyeH.load(std::memory_order_relaxed);
    if (ew <= 0 || eh <= 0) return false;
    const float underWaterness = std::clamp(
        g_stereoUnderWaterness[set].load(std::memory_order_relaxed),
        0.0f, 1.0f);
    const float waterDepth = std::clamp(
        g_stereoWaterDepth[set].load(std::memory_order_relaxed),
        0.0f, 100.0f);
    // The pose these pixels were rendered at — the quad is anchored here so the
    // compositor can reproject to the live head pose (kills rotation ghosting).
    XrPosef rp{};
    HandPose bakedHands[2]{};
    LaserRay bakedLaser{};
    TwoHandVisualState bakedTwoHand{};
    scopeaim::VisualState bakedScope{};
    driving::WheelVisualState drivingWheel{};
    MobileColorState mobileColor{};
    ThrowableTrajectory throwableTrajectory[2]{};
    unsigned int bakedWeaponHandMask = 0;
    bool haveRenderPose = false;
    {
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        haveRenderPose = g_stereoRenderPoseValid[set];
        rp             = g_stereoRenderPose[set];
        bakedHands[0]  = g_stereoHandPose[set][0];   // pose the weapon was baked with
        bakedHands[1]  = g_stereoHandPose[set][1];
        bakedLaser     = g_stereoLaserRay[set];
        bakedTwoHand   = g_stereoTwoHandVisualState[set];
        bakedScope     = g_stereoScopeState[set];
        drivingWheel   = g_stereoDrivingWheelState[set];
        mobileColor    = g_stereoMobileColor[set];
        throwableTrajectory[0] = g_stereoThrowableTrajectory[set][0];
        throwableTrajectory[1] = g_stereoThrowableTrajectory[set][1];
        bakedWeaponHandMask = g_stereoWeaponHandMask[set];
    }
    const bool shaderCopyMayRun = fxaaStats.requested ||
        underWaterness > 0.001f || mobileColor.mode != 0;
    // A projection layer MUST carry the pose its pixels were rendered at, or the
    // reprojection is wrong (this was the old "projection layer trembles" bug).
    // Until we have one, fall back to the theater screen rather than guess.
    if (!haveRenderPose) return false;

    if (!bakedScope.active) {
        BuildHandMeshes(bakedHands, bakedTwoHand, drivingWheel); // same ring pose + cockpit/support correction
        BuildWeaponMesh(bakedHands);   // (weapon-mesh path; dead until Approach-B geometry exists)
    }
    BulletTracer liveTracers[kMaxBulletTracers]{};
    const int liveTracerCount = SnapshotBulletTracers(liveTracers);
    ObjectiveMarkerVisual liveObjectiveMarkers[kMaxObjectiveMarkers]{};
    const int liveObjectiveMarkerCount =
        SnapshotObjectiveMarkers(liveObjectiveMarkers);

    GLint prevDraw = 0, prevRead = 0, prevVp[4]{};
    FxaaGlState fxaaGlState{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    if (shaderCopyMayRun) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &fxaaGlState.program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &fxaaGlState.vertexArray);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &fxaaGlState.activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &fxaaGlState.texture0);
        glGetIntegerv(GL_SAMPLER_BINDING, &fxaaGlState.sampler0);
        glActiveTexture(static_cast<GLenum>(fxaaGlState.activeTexture));
        glGetBooleanv(GL_COLOR_WRITEMASK, fxaaGlState.colorMask);
    }
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND),
                    cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST),
                    framebufferSrgb = g_swapchainSrgb
                                          ? glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT)
                                          : GL_FALSE;
    GLboolean stencil = GL_FALSE, rasterizerDiscard = GL_FALSE;
    if (shaderCopyMayRun) {
        stencil = glIsEnabled(GL_STENCIL_TEST);
        rasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);

    const float tanX = g_stereoTanX.load(std::memory_order_relaxed);
    const float tanY = g_stereoTanY.load(std::memory_order_relaxed);
    const GLuint hudSource=g_stereoHudTex[set].load(
        std::memory_order_relaxed);
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e) {
        Swapchain& chain = s.swapchains[e];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!Check(xrAcquireSwapchainImage(chain.handle, &acq, &idx), "acquire(eye)")) { ok = false; break; }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (!Check(xrWaitSwapchainImage(chain.handle, &wait), "wait(eye)")) {
            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(chain.handle, &rel); ok = false; break;
        }

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.framebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               chain.images[idx].image, 0);
        if (g_swapchainSrgb) glDisable(GL_FRAMEBUFFER_SRGB_EXT);  // raw copy + display-referred late overlays
        const GLuint sourceTexture =
            g_stereoEyeTex[set][e].load(std::memory_order_relaxed);
        const bool usedFxaa = fxaaStats.requested &&
            DrawEyeFxaa(sourceTexture, ew, eh, chain.width, chain.height,
                        underWaterness, waterDepth, mobileColor,
                        fxaaGlState, fxaaStats);
        const bool usedUnderwaterCopy = !usedFxaa &&
            DrawEyeUnderwaterCopy(sourceTexture, chain.width, chain.height,
                                  underWaterness, waterDepth, mobileColor,
                                  fxaaGlState);
        // Bind the source only after FXAA has finished sampling it. This keeps
        // the shader path free of attached-texture ambiguity while preserving
        // the proven complete READ FBO required by the late-hand depth copy.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.eyeReadFramebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, sourceTexture, 0);
        if (!usedFxaa && !usedUnderwaterCopy) {
            if (fxaaStats.requested) ++fxaaStats.fallbacks;
            // Exact pre-FXAA path. Keep it byte-for-byte equivalent so property
            // OFF and every shader failure remain a trustworthy A/B baseline.
            glBlitFramebuffer(0, 0, ew, eh, 0, 0, chain.width, chain.height,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        // Mission/tutorial text is an independent immersive layer. Disabling
        // CLASSIC hides phone-HUD crops, not text or calibration samples.
        if (hudSource)
            DrawHudComposite(hudSource,chain.width,chain.height,
                             hud::ShouldRenderClassicHud(),e);
        DrawHandsForEye(
            rp, e, chain.width, chain.height,
            g_stereoEyeDepth[set][e].load(std::memory_order_relaxed),
            ew, eh,
            g_stereoEyeNear[set][e].load(std::memory_order_relaxed),
            g_stereoEyeFar[set][e].load(std::memory_order_relaxed),
            bakedLaser, bakedWeaponHandMask,
            liveTracers, liveTracerCount,
            liveObjectiveMarkers, liveObjectiveMarkerCount,
            bakedScope, drivingWheel,
            throwableTrajectory, bakedHands, bakedTwoHand, hudSource);
        DrawScopeOverlay(chain.width, chain.height, bakedScope);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(chain.handle, &rel);

        // Describe this eye as a projection view: the exact eye pose (head pose
        // shifted along its right axis by half the IPD, in LOCAL space) and the
        // exact frustum the game rendered with. Both eyes share the head
        // orientation; the horizontal parallax is the IPD-shifted position plus
        // what is already baked into the pixels.
        const float sign = (e == 0) ? -1.0f : 1.0f;   // 0 = left, 1 = right
        const XrVector3f right = QuatRotate(rp.orientation, XrVector3f{sign * kStereoHalfIpd, 0.0f, 0.0f});
        XrCompositionLayerProjectionView& v = views[e];
        v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        v.pose.orientation           = rp.orientation;
        v.pose.position              = {rp.position.x + right.x,
                                        rp.position.y + right.y,
                                        rp.position.z + right.z};
        v.fov.angleLeft              = -std::atan(tanX);
        v.fov.angleRight             =  std::atan(tanX);
        v.fov.angleUp                =  std::atan(tanY);
        v.fov.angleDown              = -std::atan(tanY);
        v.subImage.swapchain         = chain.handle;
        v.subImage.imageRect.offset  = {0, 0};
        v.subImage.imageRect.extent  = {chain.width, chain.height};
        v.subImage.imageArrayIndex   = 0;
    }

    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (shaderCopyMayRun) {
        if (stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        if (rasterizerDiscard) glEnable(GL_RASTERIZER_DISCARD);
        else                   glDisable(GL_RASTERIZER_DISCARD);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (shaderCopyMayRun) {
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, static_cast<GLuint>(fxaaGlState.sampler0));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(fxaaGlState.texture0));
        glActiveTexture(static_cast<GLenum>(fxaaGlState.activeTexture));
        glBindVertexArray(static_cast<GLuint>(fxaaGlState.vertexArray));
        glUseProgram(static_cast<GLuint>(fxaaGlState.program));
        glColorMask(fxaaGlState.colorMask[0], fxaaGlState.colorMask[1],
                    fxaaGlState.colorMask[2], fxaaGlState.colorMask[3]);
    }
    if (g_swapchainSrgb) {
        if (framebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
        else                 glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    if (g_stereoGeneration[set].load(std::memory_order_acquire) !=
        expectedGeneration) {
        generationRace = true;
        return false;
    }
    if (!ok) return false;

    // One world-locked projection layer holding both eye views. LOCAL space + the
    // true render pose per view = the compositor reprojects render->scan-out.
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space     = s.space;
    layer.viewCount = 2;
    layer.views     = views;
    submittedSequence = expectedGeneration;
    return true;
}

void RenderFrame(double consumerUpdateMs, double consumerUpdateCpuMs) {
    if (s.session == XR_NULL_HANDLE) {
        return;
    }
    const double threadCpuT0 = perf::ThreadCpuMs();

    PollEvents();
    if (!s.running) {
        // STOPPING/IDLE has no xrWaitFrame to pace the present thread. Without
        // an explicit wait this loop calls updateTexImage/JNI hundreds of
        // thousands of times per second while Horizon is trying to resume the
        // Activity after the Meta menu. Apart from burning a core, that can
        // starve the lifecycle/UI threads badly enough that READY never arrives
        // and the last submitted frame appears frozen forever. The native Quest
        // build uses the same short idle wait while it keeps polling events.
        timespec idle{0, 5 * 1000 * 1000};
        nanosleep(&idle, nullptr);
        return;
    }

    // (Re)apply CPU/GPU perf hints and the app-main thread pin when they change
    // (menu edit, or the GameThread TID first arriving). Cheap when not dirty.
    if (g_perfDirty.exchange(false, std::memory_order_relaxed))   ApplyPerfLevels(s.instance, s.session);
    if (g_threadDirty.exchange(false, std::memory_order_relaxed)) ApplyThreadSettings(s.instance, s.session);

    // --- detailed present-path timing (find the frame-rate cap) ---
    auto nowNs = [] { timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                      return static_cast<long long>(t.tv_sec) * 1000000000LL + t.tv_nsec; };
    static long long s_lastEnter = 0;
    static long long sPredLast = 0;
    static int sLastSubmittedStereoSequence = -1;
    if (g_resetPresentTiming) {
        s_lastEnter = 0;
        sPredLast = 0;
        sLastSubmittedStereoSequence = -1;
        g_resetPresentTiming = false;
    }
    const long long tEnter = nowNs();
    const long long loopPeriod = s_lastEnter ? (tEnter - s_lastEnter) : 0;
    s_lastEnter = tEnter;

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    frameState{XR_TYPE_FRAME_STATE};
    const long long tWaitStart = nowNs();
    if (!Check(xrWaitFrame(s.session, &waitInfo, &frameState), "xrWaitFrame")) {
        return;
    }
    const long long tWaitEnd = nowNs();

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(s.session, &beginInfo), "xrBeginFrame")) {
        return;
    }

    SyncInput(frameState.predictedDisplayTime);

#ifdef SAVR_DEV
    // Developer builds: the Vice City grips+A chord toggles the FPS panel.
    {
        InputState in{};
        GetInput(in);
        const bool chord = in.grip[0] >= 0.75f && in.grip[1] >= 0.75f && in.a;
        static bool chordDown = false;
        if (chord && !chordDown) {
            s.debugVisible = !s.debugVisible;
            LOGI("[vr] debug panel %s", s.debugVisible ? "ON" : "OFF");
        }
        chordDown = chord;
    }
#else
    // Player builds: the FPS/debug panel is a developer tool; the chord was
    // too easy to hit by accident. The rendering machinery stays for dev use.
    s.debugVisible = false;
#endif

    // Turn the GameThread frame counter into a rate every half second (using the
    // predicted display time as a monotonic clock in nanoseconds).
    {
        static XrTime  lastT  = 0;
        static uint32_t lastGF = 0;
        const XrTime   now = frameState.predictedDisplayTime;
        const uint32_t gf  = g_gameFrames.load(std::memory_order_relaxed);
        if (lastT == 0) { lastT = now; lastGF = gf; }
        else if (now - lastT >= 500000000LL) {   // 0.5 s
            const double dt = static_cast<double>(now - lastT) / 1e9;
            g_fpsValue.store(static_cast<int>((gf - lastGF) / dt + 0.5), std::memory_order_relaxed);
            lastT = now; lastGF = gf;
        }
    }

    // Theater for menus/loading/cutscenes (a flat 2D screen reads wrong per-eye);
    // stereo projection once the game thread reports real gameplay. The game
    // thread owns that judgement — it can see the player, the menu and the
    // cutscene manager — so we just follow its call here.
    s.theaterMode = !vrcam::IsStereoActive();
    {
        static int pn = 0;
        if ((++pn % 180) == 1) {
            LOGI("[vr] present path: %s  renderFovX=%.1f",
                 s.theaterMode ? "THEATER (menu/cutscene)" : "projection (in-world fused)",
                 g_stereoRenderFovXDeg.load(std::memory_order_relaxed));
        }
    }

    XrCompositionLayerQuad       theaterQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad       hudQuads[3]{{XR_TYPE_COMPOSITION_LAYER_QUAD},
                                             {XR_TYPE_COMPOSITION_LAYER_QUAD},
                                             {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerQuad       debugQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection eyeProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView eyeViews[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[6]{};
    uint32_t layerCount = 0;
    int submittedStereoSequence = -1;
    bool stereoGenerationRace = false;
    double submittedStereoSequenceAgeMs = 0.0;
    StereoSyncWaitResult stereoSyncWait{};
    FxaaFrameStats fxaaStats{};
    fxaaStats.requested = FxaaRequested();
    fxaaStats.active = fxaaStats.requested && s.fxaaProgram != 0 &&
                       !g_fxaaRuntimeFailed;
    fxaaStats.errors = g_fxaaErrorCount;

    // Always locate the eye views and publish the head pose, so the game camera
    // hook can track the head no matter which screen we present below.
    {
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime           = frameState.predictedDisplayTime;
        locateInfo.space                 = s.space;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t    viewCount = 0;
        const XrResult located = xrLocateViews(s.session, &locateInfo, &viewState,
                                               static_cast<uint32_t>(s.views.size()),
                                               &viewCount, s.views.data());
        const bool posesValid =
            XR_SUCCEEDED(located) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

        if (posesValid) {
            const XrPosef& l = s.views[0].pose;
            const XrPosef& r = s.views[viewCount > 1 ? 1 : 0].pose;
            XrPosef centre = l;
            centre.position.x = (l.position.x + r.position.x) * 0.5f;
            centre.position.y = (l.position.y + r.position.y) * 0.5f;
            centre.position.z = (l.position.z + r.position.z) * 0.5f;
            std::lock_guard<std::mutex> lock(g_headPoseMutex);
            g_headPose      = centre;
            g_eyePose[0]    = l;
            g_eyePose[1]    = r;
            g_headPoseValid = true;
        }
    }

    // Locate both hands' grip + aim poses (LOCAL space) for the VR hands.
    {
        HandPose hp[2];
        for (int h = 0; h < 2; ++h) {
            XrSpaceLocation gl{XR_TYPE_SPACE_LOCATION}, al{XR_TYPE_SPACE_LOCATION};
            constexpr XrSpaceLocationFlags kValid =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            const bool gok = s.handGripSpace[h] != XR_NULL_HANDLE &&
                XR_SUCCEEDED(xrLocateSpace(s.handGripSpace[h], s.space, frameState.predictedDisplayTime, &gl)) &&
                (gl.locationFlags & kValid) == kValid;
            const bool aok = s.handAimSpace[h] != XR_NULL_HANDLE &&
                XR_SUCCEEDED(xrLocateSpace(s.handAimSpace[h], s.space, frameState.predictedDisplayTime, &al)) &&
                (al.locationFlags & kValid) == kValid;
            hp[h].valid = gok;
            hp[h].aimValid = aok;
            if (gok) {
                hp[h].gripPos[0] = gl.pose.position.x; hp[h].gripPos[1] = gl.pose.position.y; hp[h].gripPos[2] = gl.pose.position.z;
                hp[h].gripOri[0] = gl.pose.orientation.x; hp[h].gripOri[1] = gl.pose.orientation.y;
                hp[h].gripOri[2] = gl.pose.orientation.z; hp[h].gripOri[3] = gl.pose.orientation.w;
            }
            if (aok) {
                hp[h].aimPos[0] = al.pose.position.x; hp[h].aimPos[1] = al.pose.position.y; hp[h].aimPos[2] = al.pose.position.z;
                hp[h].aimOri[0] = al.pose.orientation.x; hp[h].aimOri[1] = al.pose.orientation.y;
                hp[h].aimOri[2] = al.pose.orientation.z; hp[h].aimOri[3] = al.pose.orientation.w;
            }
            hp[h].grip    = s.input.grip[h];
            hp[h].trigger = s.input.triggers[h];
        }
        std::lock_guard<std::mutex> lock(g_headPoseMutex);
        g_handPose[0] = hp[0];
        g_handPose[1] = hp[1];
    }

    // Present the game frame on a screen quad. Menus: a world-locked cinema
    // screen. Gameplay: a HEAD-LOCKED screen that fills the view — rigid to the
    // head, so it never trembles during head rotation (the old projection layer
    // did, because its reprojection frame did not match the game camera). The
    // view simply lags smoothly instead.
    if (frameState.shouldRender == XR_TRUE) {
        if (!s.theaterMode) {
            stereoSyncWait =
                WaitForFreshStereoPair(sLastSubmittedStereoSequence);
        }
        int gateSeq = -1;
        const bool haveStereo = !s.theaterMode && StereoReadSet(gateSeq) >= 0;
        if (haveStereo && RenderStereoEyeProjection(
                eyeProjection, eyeViews, submittedStereoSequence,
                stereoGenerationRace, submittedStereoSequenceAgeMs,
                fxaaStats)) {
            layers[0]  = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&eyeProjection);
            layerCount = 1;
        } else {
            const XrSpace presentSpace = s.theaterMode ? s.space : s.viewSpace;
            if (RenderTheaterQuad(presentSpace, theaterQuad, !s.theaterMode)) {
                layers[0]  = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&theaterQuad);
                layerCount = 1;
            }
        }

        // The classic HUD is composited into each projection eye above. Do not
        // submit the old three crop quads: their opaque black background and
        // guessed SA regions were the broken path.

        // FPS/debug panel on top, as a head-locked quad (both grips + A toggles it).
        if ((s.debugVisible || g_menuVisible.load(std::memory_order_relaxed) ||
             g_calibActive.load(std::memory_order_relaxed) ||
             g_mainMenuActive.load(std::memory_order_relaxed) ||
             g_holsterCalibActive.load(std::memory_order_relaxed) ||
             g_holsterMenuActive.load(std::memory_order_relaxed) ||
             g_drivingActive.load(std::memory_order_relaxed) ||
             g_drivingCalibActive.load(std::memory_order_relaxed) ||
             g_locomotionActive.load(std::memory_order_relaxed) ||
             g_hudActive.load(std::memory_order_relaxed) ||
             g_gfxActive.load(std::memory_order_relaxed) ||
             g_gfxDistanceActive.load(std::memory_order_relaxed) ||
             g_controlsMenuActive.load(std::memory_order_relaxed) ||
             g_controlsTipsActive.load(std::memory_order_relaxed) ||
             g_aboutActive.load(std::memory_order_relaxed)) && PresentDebugPanel(debugQuad)) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&debugQuad);
        }
    }

    const long long tEndStart = nowNs();
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime          = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount           = layerCount;
    endInfo.layers               = layerCount ? layers : nullptr;
    const XrResult ended = xrEndFrame(s.session, &endInfo);
    Check(ended, "xrEndFrame");
    const long long tEndDone = nowNs();
    if (XR_SUCCEEDED(ended) && submittedStereoSequence >= 0) {
        sLastSubmittedStereoSequence = submittedStereoSequence;
    }
    const double frameThreadCpuMs = perf::ThreadCpuMs() - threadCpuT0;

    // Where the frame time actually goes: loop = full RenderFrame period; wait =
    // xrWaitFrame block (compositor pacing); work = our blit+layer build; end =
    // xrEndFrame; predDelta = compositor's own frame cadence (target period).
    const long long predDelta = sPredLast
        ? (frameState.predictedDisplayTime - sPredLast) : 0;
    sPredLast = frameState.predictedDisplayTime;

    // META counter queries can involve the runtime, so sample them only once per
    // second. The per-frame CPU measurement above intentionally excludes them.
    static long long sLastRuntimeQuery = 0;
    if (sLastRuntimeQuery == 0 || tEndDone - sLastRuntimeQuery >= 1000000000LL) {
        QueryRuntimeDiagnostics();
        sLastRuntimeQuery = tEndDone;
    }

    perf::SubmitPresentFrame({
        .monoMs = static_cast<double>(tEndDone) / 1e6,
        .loopPeriodMs = static_cast<double>(loopPeriod) / 1e6,
        .consumerUpdateMs = consumerUpdateMs,
        .consumerUpdateCpuMs = consumerUpdateCpuMs,
        .waitFrameMs = static_cast<double>(tWaitEnd - tWaitStart) / 1e6,
        .workMs = static_cast<double>(tEndStart - tWaitEnd) / 1e6,
        .endFrameMs = static_cast<double>(tEndDone - tEndStart) / 1e6,
        .threadCpuMs = frameThreadCpuMs,
        .predictedDeltaMs = static_cast<double>(predDelta) / 1e6,
        .submittedStereoSequence = submittedStereoSequence,
        .stereoGenerationRace = stereoGenerationRace,
        .stereoSyncWaitAttempted = stereoSyncWait.attempted,
        .stereoSyncWaitRescued = stereoSyncWait.rescued,
        .stereoSyncWaitTimedOut = stereoSyncWait.timedOut,
        .stereoSyncWaitMs = stereoSyncWait.waitMs,
        .submittedStereoSequenceAgeMs = submittedStereoSequenceAgeMs,
        .theaterMode = s.theaterMode,
        .shouldRender = frameState.shouldRender == XR_TRUE,
        .layerCount = layerCount,
        .endSucceeded = XR_SUCCEEDED(ended),
        .endResult = static_cast<int>(ended),
        .displayRefreshValid = g_displayRefreshValid,
        .displayRefreshHz = g_displayRefreshHz,
        .runtimeCpuValid = g_runtimeCpuValid,
        .runtimeCpuMs = g_runtimeCpuMs,
        .runtimeGpuValid = g_runtimeGpuValid,
        .runtimeGpuMs = g_runtimeGpuMs,
        .fxaaRequested = fxaaStats.requested,
        .fxaaActive = fxaaStats.active,
        .fxaaDraws = fxaaStats.draws,
        .fxaaFallbacks = fxaaStats.fallbacks,
        .fxaaErrors = fxaaStats.errors,
        .fxaaSubmitWallMs = fxaaStats.submitWallMs,
    });

    // The one measurement that settles why the headset is black: whether we even
    // submit a layer, and whether the runtime accepts it. A black headset with
    // shouldRender=1, layers=1, endFrame=0 means the picture is going out and the
    // fault is upstream (source/blit); anything else points straight at the gate
    // that dropped it.
    static unsigned long long n = 0;
    if (++n % 180 == 0) {
        GLenum glErr = glGetError();
        LOGI("present: shouldRender=%d layers=%u endFrame=%s glErr=0x%x makeCurrent=%p/%p",
             frameState.shouldRender, layerCount, XrName(ended), glErr,
             eglGetCurrentContext(), eglGetCurrentSurface(EGL_DRAW));
    }
}

bool IsSessionRunning() {
    return s.running;
}

bool IsSessionFocused() {
    return g_sessionFocused.load(std::memory_order_acquire);
}

bool RecommendedEyeSize(int& width, int& height) {
    if (s.configViews.empty()) {
        return false;
    }
    width  = static_cast<int>(s.configViews[0].recommendedImageRectWidth);
    height = static_cast<int>(s.configViews[0].recommendedImageRectHeight);
    return true;
}

} // namespace savr::xr
