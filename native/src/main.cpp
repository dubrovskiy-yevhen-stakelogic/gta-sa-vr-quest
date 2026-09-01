#include "Appearance.h"
#include "Calib.h"
#include "Cheats.h"
#include "Driving.h"
#include "FrameTarget.h"
#include "HdWeapons.h"
#include "Holster.h"
#include "HudSettings.h"
#include "Locomotion.h"
#include "Log.h"
#include "Melee.h"
#include "PerfTelemetry.h"
#include "Basketball.h"
#include "MapZoom.h"
#include "BrainDiag.h"
#include "Pickups.h"
#include "PhysicalWeapon.h"
#include "ScopeAim.h"
#include "Symbols.h"
#include "Throwable.h"
#include "VrCamera.h"
#include "VrFire.h"
#include "Weapon.h"
#include "Xr.h"

#include <jni.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iterator>

namespace savr {
namespace {

JavaVM* g_vm = nullptr;
jobject g_classLoader = nullptr;   // global ref
jobject g_activity = nullptr;      // global ref

bool g_sessionAttempted = false;

// Conservative baseline floor. The game currently supplies 1.75 at runtime,
// so this does not raise scene cost; the property remains available for
// explicit diagnostics without changing population/generation multipliers.
constexpr float kDefaultVrWorldLodScale = 1.35f;
constexpr float kPhoneVrWorldLodScale = 2.625f;
constexpr char kVrWorldLodScaleProperty[] = "debug.savr.lod_scale";
constexpr char kPhoneDistanceProfileProperty[] =
    "debug.savr.phone_distance";

bool PhoneDistanceProfileEnabled() {
    static const bool enabled = [] {
        char text[PROP_VALUE_MAX]{};
        bool value = true;
        if (__system_property_get(kPhoneDistanceProfileProperty, text) > 0) {
            if (std::strcmp(text, "0") == 0) value = false;
            else if (std::strcmp(text, "1") == 0) value = true;
            else LOGW("[lod] ignoring invalid %s=%s (valid 0 or 1)",
                      kPhoneDistanceProfileProperty, text);
        }
        return value;
    }();
    return enabled;
}

float VrWorldLodScaleTarget() {
    static const float target = [] {
        char text[PROP_VALUE_MAX]{};
        float value = PhoneDistanceProfileEnabled()
            ? kPhoneVrWorldLodScale : kDefaultVrWorldLodScale;
        if (__system_property_get(kVrWorldLodScaleProperty, text) > 0) {
            char* end = nullptr;
            const float parsed = std::strtof(text, &end);
            if (end != text && end != nullptr && *end == '\0' &&
                std::isfinite(parsed) && parsed >= 0.925f && parsed <= 3.0f) {
                value = parsed;
            } else {
                LOGW("[lod] ignoring invalid %s=%s (valid 0.925..3.0)",
                     kVrWorldLodScaleProperty, text);
            }
        }
        LOGI("[lod] world scale target=%.3f property=%s",
             static_cast<double>(value), kVrWorldLodScaleProperty);
        return value;
    }();
    return target;
}

void ApplyVrWorldLodScale() {
    if (!vrcam::IsStereoActive() || g.CRenderer_ms_lodDistScale == nullptr)
        return;

    const float current = *g.CRenderer_ms_lodDistScale;
    static bool rejectedLogged = false;
    if (!std::isfinite(current) || current < 0.5f || current > 3.0f) {
        if (!rejectedLogged) {
            LOGE("[lod] renderer scale rejected: %.6f",
                 static_cast<double>(current));
            rejectedLogged = true;
        }
        return;
    }

    const float target = VrWorldLodScaleTarget();
    const float applied = std::max(current, target);
    if (applied != current) *g.CRenderer_ms_lodDistScale = applied;

    static bool appliedLogged = false;
    if (!appliedLogged) {
        LOGI("[lod] stereo world scale current=%.3f target=%.3f applied=%.3f",
             static_cast<double>(current), static_cast<double>(target),
             static_cast<double>(applied));
        appliedLogged = true;
    }
}

bool QueryPhysicalFireHand(int weaponType, int* firingHand) {
    // Physical melee owns types 1..15 and applies damage from tracked sweeps.
    // Throwable owns 16/17/18/39 as an R2 hold-preview-release state machine.
    // Never let either group enter the native trigger/task path in parallel.
    const bool throwable = weaponType == 16 || weaponType == 17 ||
                           weaponType == 18 || weaponType == 39;
    if (!firingHand || weaponType <= 15 || throwable) return false;
    const int hand = physicalweapon::FiringHand();
    const int slot = physicalweapon::HeldSlot(hand);
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (hand < 0 || hand >= physicalweapon::kHandCount ||
        slot <= 0 || slot >= 13 || !ped) return false;
    const int heldType = *reinterpret_cast<const std::int32_t*>(
        reinterpret_cast<const std::uint8_t*>(ped) + 0x730 + slot * 0x20);
    if (heldType != weaponType) return false;
    *firingHand = hand;
    return true;
}

// Set once the render loop is ours; from then on the Java loop is inert.
volatile bool g_ownFrameLoop = false;

// The engine's onSurfaceCreated arrives on the UI thread, where the engine does
// its own eglMakeCurrent and so steals the shared context off our render thread
// for good. It cannot simply be dropped — the engine needs it to keep rendering.
// So it is deferred: this flag hands it to the render thread, which runs it with
// the context already current there, turning a cross-thread steal into a
// harmless same-thread rebind.
volatile bool g_pendingSurfaceCreated = false;

// Inline present: the engine drives itself on GameThread, and we consume and
// present on that same thread once this is set.
volatile bool g_inlineReady = false;
int g_gameWidth = 0;
int g_gameHeight = 0;

long long g_engineNanos = 0;
jclass    g_gameNativeClass = nullptr;   // global ref
constexpr long long kOuterPacerFallbackPeriodNs = 1000000000LL / 90;
constexpr long long kOuterPacerTargetPeriodNs =
    1000000000LL / frame_target::kFps;
// Stay on the known-good 90 Hz boot cadence unless the retail limiter patch is
// fully fingerprinted, written and verified. This prevents a failed/unknown APK
// from combining the stock 30 gate with a new target-rate outer pacer.
std::atomic<long long> g_outerPacerPeriodNs{kOuterPacerFallbackPeriodNs};
std::atomic<bool> g_resetGameTiming{true};
double g_prevGameFrameStartMs = 0.0;
long long g_lastPacerFrameNs = 0;
constexpr char kAbsoluteOuterPacerProperty[] =
    "debug.savr.absolute_pacer";

bool AbsoluteOuterPacerRequested() {
    static const bool requested = [] {
        char text[PROP_VALUE_MAX]{};
        bool value = true;
        if (__system_property_get(kAbsoluteOuterPacerProperty, text) > 0) {
            if (std::strcmp(text, "0") == 0) value = false;
            else if (std::strcmp(text, "1") == 0) value = true;
            else LOGW("[pacer] ignoring invalid %s=%s (valid 0 or 1)",
                      kAbsoluteOuterPacerProperty, text);
        }
        LOGI("[pacer] absolute_deadline=%d property=%s",
             value ? 1 : 0, kAbsoluteOuterPacerProperty);
        return value;
    }();
    return requested;
}
EGLDisplay g_display   = EGL_NO_DISPLAY;
EGLContext g_context   = EGL_NO_CONTEXT;   // the engine's context (we do not own it)
EGLSurface g_surface   = EGL_NO_SURFACE;   // the engine's window surface, refreshed each frame
EGLContext g_xrContext = EGL_NO_CONTEXT;   // OUR context: consumer + OpenXR, nothing else touches it
EGLSurface g_pbuffer   = EGL_NO_SURFACE;   // a 1x1 surface for g_xrContext

// Create the context the render thread owns outright, plus a 1x1 pbuffer to make
// it current on. This is the crux of the whole fix: the engine drags the shared
// context onto its own thread at the Rockstar gate, so our SurfaceTexture consume
// and all OpenXR work run on THIS context instead, which the engine can never
// touch. The two sides communicate only through the SurfaceTexture's BufferQueue,
// which needs no shared GL objects — so this context deliberately does NOT share
// with the engine's.
bool CreateXrGl(EGLDisplay display) {
    const EGLint spec[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint    count  = 0;
    if (eglChooseConfig(display, spec, &config, 1, &count) != EGL_TRUE || count != 1) {
        LOGE("no pbuffer/ES3 EGLConfig for the XR context");
        return false;
    }

    // Share the engine's context so this context can sample the eye textures the
    // game thread renders for stereo. Sharing only makes GL objects visible
    // across contexts; each thread still keeps its OWN context current, so this
    // never re-creates the context-steal deadlock that made us decouple in the
    // first place (that was eglMakeCurrent on the engine's context from here).
    const char* const eglExtensions = eglQueryString(display, EGL_EXTENSIONS);
    const bool prioritySupported = eglExtensions &&
        std::strstr(eglExtensions, "EGL_IMG_context_priority") != nullptr;
    const EGLint defaultCtxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    const EGLint highCtxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
        EGL_NONE,
    };
    auto createContext = [&](EGLContext shareContext, bool& requestedHigh) {
        requestedHigh = false;
        if (prioritySupported) {
            EGLContext context = eglCreateContext(
                display, config, shareContext, highCtxAttribs);
            if (context != EGL_NO_CONTEXT) {
                requestedHigh = true;
                return context;
            }
            LOGW("high-priority XR context failed (0x%x) - retrying default "
                 "priority", eglGetError());
        }
        return eglCreateContext(display, config, shareContext,
                                defaultCtxAttribs);
    };

    EGLContext share = g_context;   // set on GameThread before this thread starts
    bool shared = false;
    bool requestedHigh = false;
    if (share != EGL_NO_CONTEXT) {
        g_xrContext = createContext(share, requestedHigh);
        shared = (g_xrContext != EGL_NO_CONTEXT);
        if (!shared) {
            LOGW("shared XR context failed (0x%x) - retrying unshared, stereo eye "
                 "textures will be unavailable", eglGetError());
        }
    }
    if (g_xrContext == EGL_NO_CONTEXT) {
        g_xrContext = createContext(EGL_NO_CONTEXT, requestedHigh);
    }
    if (g_xrContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext(XR) failed: 0x%x", eglGetError());
        return false;
    }
    // Whether the context actually shares the engine's decides if the stereo eye
    // textures are usable here; if not, the bridge disables and we present mono.
    xr::SetStereoContextShared(shared);
    EGLint actualPriority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
    EGLBoolean priorityQueried = EGL_FALSE;
    EGLint priorityQueryError = EGL_SUCCESS;
    if (prioritySupported) {
        priorityQueried = eglQueryContext(display, g_xrContext,
                                          EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                                          &actualPriority);
        if (priorityQueried != EGL_TRUE) {
            priorityQueryError = eglGetError();
        }
    }
    const auto priorityName = [](EGLint priority) {
        switch (priority) {
        case EGL_CONTEXT_PRIORITY_HIGH_IMG:   return "HIGH";
        case EGL_CONTEXT_PRIORITY_MEDIUM_IMG: return "MEDIUM";
        case EGL_CONTEXT_PRIORITY_LOW_IMG:    return "LOW";
        default:                              return "UNKNOWN";
        }
    };
    LOGI("XR context created%s priority_ext=%d requested=%s actual=%s query=%d query_err=0x%x",
         shared ? " (shared with engine)" : " (UNSHARED - mono only)",
         prioritySupported ? 1 : 0, requestedHigh ? "HIGH" : "DEFAULT",
         priorityQueried == EGL_TRUE ? priorityName(actualPriority) : "N/A",
         priorityQueried == EGL_TRUE ? 1 : 0, priorityQueryError);

    const EGLint pbAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    g_pbuffer = eglCreatePbufferSurface(display, config, pbAttribs);
    if (g_pbuffer == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface(XR) failed: 0x%x", eglGetError());
        return false;
    }
    return true;
}

// libGame.so is pulled in by the game's own loader (ReLinker) after our
// Application class has already run, so it is not there when we are loaded.
// Rather than racing the loader we simply wait for it: the gap between the
// library appearing and the first frame is a few hundred milliseconds, so a
// short poll is both harmless and impossible to get wrong.
constexpr int kPollIntervalMs = 25;
constexpr int kPollTimeoutMs  = 120'000;

void SleepMs(int ms) {
    timespec ts{ms / 1000, static_cast<long>(ms % 1000) * 1'000'000L};
    nanosleep(&ts, nullptr);
}

// ---------------------------------------------------------------------------
// Intercepted JNI boundary
//
// GameNative's methods are ordinary exported Java_* symbols, which means they
// can be replaced with RegisterNatives instead of an inline hook: the JVM simply
// starts calling ours, and the originals stay reachable through the addresses
// dlsym already gave us. No trampolines, no instruction patching, nothing that
// can go wrong on a different CPU or a future game build.
// ---------------------------------------------------------------------------

void TakeOverFrameLoop();

// ---------------------------------------------------------------------------
// Rockstar Social Club gate bypass
//
// OS_RockstarShowGate / OS_OnRockstarGateComplete are plain C++ functions, not
// JNI methods, so RegisterNatives cannot reach them — this is the one place an
// inline hook is warranted. We overwrite the first 16 bytes of ShowGate with a
// load-and-branch to our own handler, which reports the gate passed and returns.
// The login UI (which on a headset only launches the Oculus Store and steals
// focus) never runs, and the engine boots straight into the game.
// ---------------------------------------------------------------------------

// The completions are NOT called from inside these hooks. ShowInitial/ShowGate
// run in the middle of the engine's own frame; calling Complete synchronously
// there re-enters the engine mid-state and froze it. Instead we record the
// request and fire the completion from a clean point between frames.
volatile bool g_pendingInitialComplete = false;
volatile bool g_pendingGateComplete = false;
volatile int  g_pendingGateId = 0;

void OnRockstarShowGate(int gateId) {
    LOGI("rockstar gate %d requested - deferring auto-pass", gateId);
    g_pendingGateId = gateId;
    g_pendingGateComplete = true;
}

void OnRockstarShowInitial() {
    LOGI("rockstar initial sign-in requested - deferring auto-complete");
    g_pendingInitialComplete = true;
}

// The SFX and stream asset packs (data_sfx*, data_streams) ship separately from
// Play and are absent from a sideload, so the track loader hits a null ZIPFile
// and crashes (ZIPFile::Find, this=0) the instant a game starts. Returning "no
// stream" here fails the load cleanly instead — the audio engine keeps running
// (so cutscenes advance on their timer rather than waiting forever), just silent.
void* OnGetDataStream(void* /*self*/, unsigned int /*id*/) {
    return nullptr;
}

// The intro cutscene blocks here waiting for its dialogue track to preload from
// the absent stream pack, so nothing ever advances. Skipping the preload lets the
// cutscene run (silent) instead of hanging the whole game on the first frame.
void OnPreloadCutsceneTrack(void* /*self*/, short /*a*/, unsigned char /*b*/) {}

// Android can call StartUserPause repeatedly from a hidden mobile frontend after
// an injected enter-car/menu input.  In focused head-tracked gameplay there is no
// visible pause frontend to dismiss, so accepting that call permanently freezes
// simulation and audio.  Preserve stock pause everywhere else (loading, theater,
// cutscenes and while the OpenXR session is not focused).
void OnStartUserPause() {
    if (xr::IsSessionFocused() && vrcam::IsStereoActive()) {
        static bool logged = false;
        if (!logged) {
            LOGW("[pause.fix] suppressed StartUserPause during focused VR gameplay");
            logged = true;
        }
        return;
    }
    if (g.CTimer_m_UserPause) *g.CTimer_m_UserPause = true;
}

bool InstallInlineHook(void* target, void* replacement) {
    auto* code = reinterpret_cast<uint32_t*>(target);

    const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t start = reinterpret_cast<uintptr_t>(code) & ~(pageSize - 1);
    const uintptr_t end   = (reinterpret_cast<uintptr_t>(code) + 16 + pageSize - 1) & ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect for the gate hook failed");
        return false;
    }

    // LDR X17, #8 ; BR X17 ; <64-bit absolute target>
    code[0] = 0x58000051;
    code[1] = 0xD61F0220;
    *reinterpret_cast<void**>(code + 2) = replacement;

    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + 16);
    return true;
}

struct GameLimiterPatchStatus {
    bool active{};
    bool rxRestored{true};
    std::uint32_t capBefore{};
    std::uint32_t gateBefore{};
    std::uint32_t capAfter{};
    std::uint32_t gateAfter{};
};

constexpr std::uint32_t EncodeMovW8Immediate(std::uint32_t value) {
    return 0x52800008u | ((value & 0xffffu) << 5u);
}

GameLimiterPatchStatus EnableSingleOuterLimiter(std::uintptr_t loadBase,
                                                 bool knownRetail211) {
    constexpr std::uintptr_t kCapWindowOffset = 0x3683c8u;
    constexpr std::uintptr_t kGameplayCapOffset = 0x3683d0u;
    constexpr std::uintptr_t kGateWindowOffset = 0x3686a8u;
    constexpr std::uintptr_t kLimiterGateOffset = 0x3686b0u;
    constexpr std::uint32_t kRetailCap30 = 0x528003c8u;  // mov w8,#30
    constexpr std::uint32_t kRetailGate = 0x540000cdu;   // b.le skip
    constexpr std::uint32_t kTargetCap =
        EncodeMovW8Immediate(frame_target::kFps);
    static_assert(EncodeMovW8Immediate(60u) == 0x52800788u &&
                  EncodeMovW8Immediate(72u) == 0x52800908u,
                  "ARM64 limiter encoding changed unexpectedly");
    constexpr std::uint32_t kNop = 0xd503201fu;
    constexpr std::uint32_t kCapWindow[4] = {
        0x79422008u, 0x34002ac8u, kRetailCap30, 0x1400009du
    };
    constexpr std::uint32_t kGateWindow[4] = {
        0x1f020100u, 0x1e212000u, kRetailGate, 0x9412389bu
    };

    GameLimiterPatchStatus status{};
    if (!knownRetail211 || loadBase == 0u) {
        LOGE("[limiter] unsupported libGame build; keeping stock gate + outer 90 Hz");
        return status;
    }

    auto* cap = reinterpret_cast<volatile std::uint32_t*>(loadBase + kGameplayCapOffset);
    auto* gate = reinterpret_cast<volatile std::uint32_t*>(loadBase + kLimiterGateOffset);
    auto* capWindow = reinterpret_cast<volatile std::uint32_t*>(loadBase + kCapWindowOffset);
    auto* gateWindow = reinterpret_cast<volatile std::uint32_t*>(loadBase + kGateWindowOffset);
    status.capBefore = *cap;
    status.gateBefore = *gate;

    const bool capNeighborsMatch =
        capWindow[0] == kCapWindow[0] && capWindow[1] == kCapWindow[1] &&
        capWindow[3] == kCapWindow[3];
    const bool gateNeighborsMatch =
        gateWindow[0] == kGateWindow[0] && gateWindow[1] == kGateWindow[1] &&
        gateWindow[3] == kGateWindow[3];
    auto selectFailurePacer = [&] {
        // If the retail gate is still present, the original 90 Hz wrapper cadence
        // is safe. Any other/partial gate word gets the target outer cadence
        // fail-safe so an unknown bypass can never run above the target rate.
        const long long safePeriod = status.gateBefore == kRetailGate
            ? kOuterPacerFallbackPeriodNs : kOuterPacerTargetPeriodNs;
        g_outerPacerPeriodNs.store(safePeriod, std::memory_order_release);
    };
    if (!capNeighborsMatch || !gateNeighborsMatch) {
        LOGE("[limiter] surrounding instruction fingerprint mismatch; "
             "leaving code untouched with a fail-safe outer cadence");
        status.capAfter = status.capBefore;
        status.gateAfter = status.gateBefore;
        selectFailurePacer();
        return status;
    }

    // A second resolver pass would see the desired pair. Treat it as active, but
    // never accept a mixed/unknown pair: both words must describe one known state.
    if (status.capBefore == kTargetCap && status.gateBefore == kNop) {
        status.active = true;
        status.capAfter = status.capBefore;
        status.gateAfter = status.gateBefore;
        g_outerPacerPeriodNs.store(kOuterPacerTargetPeriodNs, std::memory_order_release);
        LOGI("[limiter] cap%d + gate bypass already active",
             frame_target::kFps);
        return status;
    }
    if (status.capBefore != kCapWindow[2] || status.gateBefore != kGateWindow[2]) {
        LOGE("[limiter] fingerprint mismatch cap=0x%08x gate=0x%08x; "
             "keeping stock cadence", status.capBefore, status.gateBefore);
        status.capAfter = status.capBefore;
        status.gateAfter = status.gateBefore;
        selectFailurePacer();
        return status;
    }

    const long pageSizeRaw = sysconf(_SC_PAGESIZE);
    if (pageSizeRaw <= 0) {
        LOGE("[limiter] invalid page size; patch not applied");
        status.capAfter = status.capBefore;
        status.gateAfter = status.gateBefore;
        return status;
    }
    const std::uintptr_t pageSize = static_cast<std::uintptr_t>(pageSizeRaw);
    const std::uintptr_t firstAddress = loadBase + kGameplayCapOffset;
    const std::uintptr_t lastAddress = loadBase + kLimiterGateOffset + sizeof(std::uint32_t);
    const std::uintptr_t pageStart = firstAddress & ~(pageSize - 1u);
    const std::uintptr_t pageEnd = (lastAddress + pageSize - 1u) & ~(pageSize - 1u);
    if (mprotect(reinterpret_cast<void*>(pageStart), pageEnd - pageStart,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("[limiter] mprotect failed; patch not applied");
        status.capAfter = status.capBefore;
        status.gateAfter = status.gateBefore;
        return status;
    }
    status.rxRestored = false;

    // Publish the conservative state first. A concurrent callback can observe
    // target cadence + stock gate, but never gate-bypassed code with outer90.
    g_outerPacerPeriodNs.store(kOuterPacerTargetPeriodNs, std::memory_order_release);
    *cap = kTargetCap;
    *gate = kNop;
    __builtin___clear_cache(reinterpret_cast<char*>(firstAddress),
                            reinterpret_cast<char*>(lastAddress));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    status.capAfter = *cap;
    status.gateAfter = *gate;

    auto rollback = [&] {
        // Restore the gate first so the intermediate state remains capped.
        *gate = kRetailGate;
        *cap = kRetailCap30;
        __builtin___clear_cache(reinterpret_cast<char*>(firstAddress),
                                reinterpret_cast<char*>(lastAddress));
        std::atomic_thread_fence(std::memory_order_seq_cst);
        status.active = false;
        status.capAfter = *cap;
        status.gateAfter = *gate;
        const bool stockRestored = status.capAfter == kRetailCap30 &&
                                   status.gateAfter == kRetailGate;
        g_outerPacerPeriodNs.store(stockRestored ? kOuterPacerFallbackPeriodNs
                                                 : kOuterPacerTargetPeriodNs,
                                   std::memory_order_release);
        if (!stockRestored) {
            LOGE("[limiter] rollback read-back is not stock; keeping outer %d fail-safe",
                 frame_target::kFps);
        }
    };

    if (status.capAfter != kTargetCap || status.gateAfter != kNop) {
        LOGE("[limiter] write verification failed; rolling back retail words");
        rollback();
    } else {
        status.active = true;
        g_outerPacerPeriodNs.store(kOuterPacerTargetPeriodNs, std::memory_order_release);
    }

    if (mprotect(reinterpret_cast<void*>(pageStart), pageEnd - pageStart,
                 PROT_READ | PROT_EXEC) == 0) {
        status.rxRestored = true;
    } else {
        LOGE("[limiter] could not restore RX page protection; rolling back");
        if (status.active) rollback();
        if (mprotect(reinterpret_cast<void*>(pageStart), pageEnd - pageStart,
                     PROT_READ | PROT_EXEC) == 0) {
            status.rxRestored = true;
        } else {
            LOGE("[limiter] RX restore still failed after rollback");
        }
    }
    if (status.active) {
        LOGI("[limiter] single-pacer mode active: GTA cap field %d, "
             "gate bypassed, outer %d Hz",
             frame_target::kFps, frame_target::kFps);
    }
    return status;
}

constexpr GLenum kTextureExternalOES = 0x8D65;

GLuint g_gameTexture = 0;
jclass g_savrClass = nullptr;   // global ref to com.savr.SavrApplication

// FindClass on a native-created thread searches the system class loader, which
// knows nothing about the app's own classes — so it fails for SavrApplication
// from the render thread and quietly breaks texture attach and frame publish.
// Resolve the class once through the app class loader (the same one the GameNative
// interception uses) and cache it as a global ref.
jclass SavrClass(JNIEnv* env) {
    if (g_savrClass != nullptr) {
        return g_savrClass;
    }
    if (g_classLoader == nullptr) {
        LOGE("no class loader for SavrApplication lookup");
        return nullptr;
    }
    jclass loaderClass = env->GetObjectClass(g_classLoader);
    jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.savr.SavrApplication");
    auto local = static_cast<jclass>(env->CallObjectMethod(g_classLoader, loadClass, name));
    env->DeleteLocalRef(name);
    if (env->ExceptionCheck() || local == nullptr) {
        env->ExceptionClear();
        LOGE("could not load com.savr.SavrApplication");
        return nullptr;
    }
    g_savrClass = static_cast<jclass>(env->NewGlobalRef(local));
    return g_savrClass;
}

// Ask Java for the surface the engine will render into. No GL work happens here:
// the engine has not created its context yet, and a texture generated without one
// silently comes back as 0 — which reads as a perfectly healthy black headset.
jobject GameSurface(JNIEnv* env, int width, int height) {
    jclass appClass = SavrClass(env);
    if (appClass == nullptr) {
        return nullptr;
    }
    jmethodID create = env->GetStaticMethodID(appClass, "createGameSurface",
                                              "(II)Landroid/view/Surface;");
    if (create == nullptr) {
        env->ExceptionClear();
        LOGE("createGameSurface not found");
        return nullptr;
    }
    jobject surface = env->CallStaticObjectMethod(appClass, create, width, height);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return nullptr;
    }
    return surface;
}

// Now that the engine has a context, give the surface a texture to stream into.
bool AttachGameTexture(JNIEnv* env) {
    if (g_gameTexture != 0) {
        return true;
    }

    glGenTextures(1, &g_gameTexture);
    if (g_gameTexture == 0) {
        LOGE("glGenTextures returned 0 - still no GL context?");
        return false;
    }
    glBindTexture(kTextureExternalOES, g_gameTexture);
    glTexParameteri(kTextureExternalOES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(kTextureExternalOES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(kTextureExternalOES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(kTextureExternalOES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(kTextureExternalOES, 0);

    jclass appClass = SavrClass(env);
    jmethodID attach = appClass ? env->GetStaticMethodID(appClass, "attachGameTexture", "(I)V")
                                : nullptr;
    if (attach == nullptr) {
        env->ExceptionClear();
        LOGE("attachGameTexture not found");
        return false;
    }
    env->CallStaticVoidMethod(appClass, attach, static_cast<jint>(g_gameTexture));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return true;
}

// Pull the engine's newest finished frame into the texture and pass it, with its
// layout matrix, to the compositor side.
void PublishGameFrame(JNIEnv* env) {
    static jclass    appClass = nullptr;
    static jmethodID update   = nullptr;
    if (update == nullptr) {
        appClass = SavrClass(env);
        if (appClass == nullptr) {
            return;
        }
        update   = env->GetStaticMethodID(appClass, "updateGameTexture", "()[F");
        if (update == nullptr) {
            env->ExceptionClear();
            return;
        }
    }

    auto matrix = static_cast<jfloatArray>(env->CallStaticObjectMethod(appClass, update));
    if (env->ExceptionCheck()) {
        // Never swallowed silently: an exception here stops frames reaching the
        // headset while everything else keeps looking healthy, which is the most
        // expensive kind of failure to track down.
        static int reported = 0;
        if (++reported <= 3) {
            LOGE("updateGameTexture threw (occurrence %d)", reported);
            env->ExceptionDescribe();
        }
        env->ExceptionClear();
        return;
    }
    if (matrix == nullptr) {
        return;
    }

    float transform[16]{};
    env->GetFloatArrayRegion(matrix, 0, 16, transform);
    env->DeleteLocalRef(matrix);
    xr::SetGameFrame(g_gameTexture, transform);
}

void OnSurfaceChanged(JNIEnv* env, jclass clazz, jobject surface, jint width, jint height) {
    LOGI("surfaceChanged %dx%d", width, height);

    // Once the loop is ours this must be ignored outright. The shell keeps
    // recreating its panel surface, and letting the engine handle that means it
    // calls eglMakeCurrent on GameThread — which steals the context out from
    // under our render thread and makes updateTexImage fail from then on.
    if (g_ownFrameLoop) {
        LOGI("surfaceChanged ignored - the render loop is ours");
        return;
    }

    // The shell's surface is deliberately not passed on. As soon as the app is
    // immersive Horizon OS takes its 2D panel away, and an engine bound to that
    // panel loses its EGL surface and stops rendering for good. Ours is a plain
    // buffer that nothing else owns, so the render loop keeps running whatever
    // the shell does with its windows.
    int eyeWidth = width;
    int eyeHeight = height;
    xr::RecommendedEyeSize(eyeWidth, eyeHeight);
    vrcam::SetStereoBaseSize(eyeWidth, eyeHeight);

    // The engine's flat pass is only the Android frontend/map, cutscenes and
    // the SurfaceTexture source. Give that pass GTA SA's native 640:448 aspect
    // instead of the nearly-square eye aspect that crops fullscreen artwork.
    // VrCamera keeps the independent OpenXR eye size above for both stereo
    // rasters, so gameplay resolution and projection do not change.
    const int flatWidth=eyeWidth;
    const int flatHeight=std::max(64,
        static_cast<int>(std::lround(flatWidth*(448.0/640.0)))) & ~1;
    g_gameWidth = flatWidth;
    g_gameHeight = flatHeight;
    xr::SetTheaterCrop(flatWidth,flatHeight); // source aspect only; no pixel crop
    jobject offscreen = GameSurface(env, flatWidth, flatHeight);
    if (offscreen == nullptr) {
        LOGE("falling back to the shell surface - expect the render loop to die");
        offscreen = surface;
    }

    g.implOnSurfaceChanged(env, clazz, offscreen, flatWidth, flatHeight);

    // Set everything up right here, on the engine's own GameThread, with its
    // context current. We do NOT take the frame loop onto a thread of our own:
    // once the game starts loading it runs its render work on GameThread and
    // fights us for the single context (EGL_BAD_ACCESS, frozen frames). Instead
    // the engine keeps driving itself through implOnDrawFrame, and we simply
    // consume and present inline on the same thread — one context, no contention.
    if (!g_sessionAttempted) {
        g_sessionAttempted = true;
        g_display = eglGetCurrentDisplay();
        g_context = eglGetCurrentContext();
        g_surface = eglGetCurrentSurface(EGL_DRAW);

        // Apply the paired limiter change only now: this callback proves our JNI
        // wrapper is registered, and it runs on GameThread between engine frames.
        // Therefore a successful gate bypass can never exist without the outer
        // pacer that takes ownership of cadence on the very next callback.
        const std::uintptr_t drawOffset = g.LoadBase
            ? reinterpret_cast<std::uintptr_t>(g.implOnDrawFrame) - g.LoadBase : 0u;
        const bool knownRetail211 = drawOffset == 0x7d28d4u;
        const GameLimiterPatchStatus limiter =
            EnableSingleOuterLimiter(g.LoadBase, knownRetail211);
        const long long outerPacerPeriodNs =
            g_outerPacerPeriodNs.load(std::memory_order_acquire);
        const double outerPacerHz = outerPacerPeriodNs > 0
            ? 1e9 / static_cast<double>(outerPacerPeriodNs) : 0.0;
        perf::SetLimiterDebugStatus(limiter.active, limiter.rxRestored,
                                    static_cast<float>(outerPacerHz));
        LOGI("[perf.init] limiter active=%d rx=%d cap=0x%08x->0x%08x "
             "gate=0x%08x->0x%08x outer_pacer=%.1fHz(%.3fms)",
             limiter.active ? 1 : 0, limiter.rxRestored ? 1 : 0,
             limiter.capBefore, limiter.capAfter,
             limiter.gateBefore, limiter.gateAfter,
             outerPacerHz,
             static_cast<double>(outerPacerPeriodNs) / 1e6);

        g_inlineReady = true;    // OnDrawFrame may now queue frames
        g_ownFrameLoop = true;   // enables the pause/surface swallows below
        TakeOverFrameLoop();     // starts the present-only consumer thread
    }
}

void OnPause(JNIEnv* env, jclass clazz) {
    // Passed through. The engine runs on its own GameThread with its own context,
    // so its pause/resume lifecycle is its own business — and swallowing the
    // resume half left the engine stuck paused, spinning without ever advancing
    // its boot. Let the engine manage its own state.
    g.implOnPause(env, clazz);
}

void OnResume(JNIEnv* env, jclass clazz) {
    g.implOnResume(env, clazz);
    g_resetGameTiming.store(true, std::memory_order_release);
}

void OnSurfaceDestroyed(JNIEnv* env, jclass clazz) {
    // Swallowed on purpose. This event only ever means the shell reclaimed its
    // panel; the buffer the game actually draws into is still perfectly alive,
    // and forwarding the teardown would stop the engine for good.
    if (g_ownFrameLoop) {
        LOGI("surfaceDestroyed ignored - the game keeps its own buffer");
        return;
    }
    g.implOnSurfaceDestroyed(env, clazz);
}

void OnSurfaceCreated(JNIEnv* env, jclass clazz) {
    // After takeover the engine is permanently bound to our SurfaceTexture, not
    // to the shell SurfaceView. SurfaceChanged and SurfaceDestroyed already
    // ignore shell churn; forwarding only SurfaceCreated on return from the Meta
    // menu leaves the stock lifecycle half-recreated and can stop the producer
    // until a SurfaceChanged that we intentionally never forward. Treat all
    // three shell surface callbacks symmetrically.
    if (g_ownFrameLoop) {
        LOGI("surfaceCreated ignored - persistent game buffer is still bound");
        return;
    }
    g.implOnSurfaceCreated(env, clazz);
}

// One frame: best-effort drive the engine on ITS context, then always consume
// and present on OURS.
void DrawOneFrame(JNIEnv* env, float deltaTime) {
    static unsigned long long frames = 0;
    static unsigned long long enginePhaseOk = 0;
    ++frames;

    // --- engine phase: only if we can grab its context ---
    //
    // Best-effort on purpose. Before the Rockstar gate the engine has no render
    // thread of its own, so this bind succeeds and we drive implOnDrawFrame. After
    // the gate the engine keeps its context on its own thread; this bind fails
    // with EGL_BAD_ACCESS and we simply skip — the engine is drawing itself, and
    // the frames still arrive through the SurfaceTexture for us to present.
    bool droveEngine = false;
    if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) == EGL_TRUE) {
        ++enginePhaseOk;

        if (g_pendingSurfaceCreated) {
            g_pendingSurfaceCreated = false;
            g.implOnSurfaceCreated(env, g_gameNativeClass);
            EGLSurface d = eglGetCurrentSurface(EGL_DRAW);
            if (d != EGL_NO_SURFACE) g_surface = d;
            eglMakeCurrent(g_display, g_surface, g_surface, g_context);
        }

        timespec before{};
        clock_gettime(CLOCK_MONOTONIC, &before);
        g.implOnDrawFrame(env, g_gameNativeClass, deltaTime);
        timespec after{};
        clock_gettime(CLOCK_MONOTONIC, &after);
        g_engineNanos += (after.tv_sec - before.tv_sec) * 1000000000LL + (after.tv_nsec - before.tv_nsec);

        EGLSurface d = eglGetCurrentSurface(EGL_DRAW);
        if (d != EGL_NO_SURFACE) g_surface = d;
        eglSwapBuffers(g_display, g_surface);
        droveEngine = true;

        // Fire the Social Club completions here — between engine frames, with the
        // engine's context current on this thread, never re-entered from inside
        // the ShowInitial/ShowGate call.
        if (g_pendingInitialComplete && g.OS_OnRockstarInitialComplete != nullptr) {
            g_pendingInitialComplete = false;
            LOGI("firing OS_OnRockstarInitialComplete");
            g.OS_OnRockstarInitialComplete();
        }
        if (g_pendingGateComplete && g.OS_OnRockstarGateComplete != nullptr) {
            g_pendingGateComplete = false;
            LOGI("firing OS_OnRockstarGateComplete(%d)", g_pendingGateId);
            g.OS_OnRockstarGateComplete(g_pendingGateId, true);
        }
    }

    // --- present phase: always, on our own context ---
    //
    // g_xrContext is never touched by the engine, so this make-current cannot
    // fail the way the engine phase can. Everything the compositor needs — the
    // SurfaceTexture consumer and the OpenXR swapchains — lives here.
    if (eglMakeCurrent(g_display, g_pbuffer, g_pbuffer, g_xrContext) != EGL_TRUE) {
        static int failed = 0;
        if (++failed <= 5) LOGE("eglMakeCurrent(xr) failed: 0x%x", eglGetError());
        return;
    }
    const double updateT0 = perf::MonotonicMs();
    const double updateCpuT0 = perf::ThreadCpuMs();
    PublishGameFrame(env);
    xr::RenderFrame(perf::MonotonicMs() - updateT0,
                    perf::ThreadCpuMs() - updateCpuT0);

    if (frames % 300 == 0) {
        LOGI("frame %llu, session %s, engine drove %llu/300 last block, %.2f ms/frame, droveNow=%d",
             frames, xr::IsSessionRunning() ? "running" : "idle", enginePhaseOk,
             static_cast<double>(g_engineNanos) / (enginePhaseOk ? enginePhaseOk : 1) / 1e6, droveEngine);
        g_engineNanos = 0;
        enginePhaseOk = 0;
    }
}

// Forward the Quest controllers onto the game's virtual gamepad. Button codes
// are the game's own (from InputHandler in the APK): A=0 B=1 X=2 Y=3 menu=15,
// and the D-pad synthesised from the left stick uses 8/9/10/11.
void SendInputToGame(JNIEnv* env, jclass clazz) {
    if (g.implOnGamepadAxesChanged == nullptr) {
        return;
    }
    static bool connected = false;
    if (!connected) {
        if (g.implOnGamepadConnected != nullptr) g.implOnGamepadConnected(env, clazz, 0);
        connected = true;
    }

    xr::InputState in{};
    xr::GetInput(in);

    // Keep the stereo/theater decision fresh even in frames the camera hook
    // never sees — the pause menu freeze was exactly this gate going stale.
    vrcam::RefreshStereoGate();

    // Hold the game's own user pause for the whole pause-menu visit. Our
    // Menu-button path opens MobileMenu::InitForPause through the engine's
    // resume-tick flags, and that tick calls CTimer::EndUserPause just before
    // the init — so nothing was left pausing the simulation and the world
    // kept running behind the menu. Applied only in-game (a player exists);
    // the boot frontend keeps its own state. Released the moment the menu
    // closes, and only if we were the ones who paused.
    {
        static bool menuPauseApplied = false;
        const bool mobileMenuOpen = vrcam::IsMobileMenuOpen();
        const bool playerExists = g.FindPlayerPed && g.FindPlayerPed(-1);
        if (mobileMenuOpen && playerExists) {
            if (g.CTimer_m_UserPause && !*g.CTimer_m_UserPause &&
                g.CTimer_StartUserPause) {
                g.CTimer_StartUserPause();
                menuPauseApplied = true;
                LOGI("[pause.menu] user pause held for pause menu");
            }
        } else if (menuPauseApplied) {
            if (g.CTimer_m_UserPause && *g.CTimer_m_UserPause &&
                g.CTimer_EndUserPause) {
                g.CTimer_EndUserPause();
                LOGI("[pause.menu] user pause released after pause menu");
            }
            menuPauseApplied = false;
        }
    }

    // In gameplay the controllers drive the game pad; in menus they do NOT.
    // The frontend is a touch menu, and feeding it a synthesised D-pad + A/X/Y on
    // top of the laser pointer made it wander into sub-menus on its own and trap
    // the player there. So in menus only the laser pointer (tap) and Back are
    // live, which is predictable; the game pad is reconnected for gameplay.
    const bool inGameplay = vrcam::IsStereoActive();

    // VR menu (Vice City style): both grips + the Menu button open one VR menu whose
    // root lists the sections. A enters a section, B steps back (and closes from the
    // root). The left stick moves the highlight and edits values; L2/R2 also
    // decrement/increment the selected value, with tap-then-auto-repeat. Dispatch
    // runs here on the GameThread (right after the tick) so cheat handlers stream/
    // spawn where the engine expects. While any page is open the stick drives the
    // menu (SetInputBlocked) and the holster is suppressed, so opening the menu never
    // walks the player or drops/switches the weapon in hand.
    enum { PG_NONE = 0, PG_MAIN, PG_CALIB, PG_HOLSTER_CALIB, PG_HOLSTERS,
           PG_DRIVING, PG_DRIVING_CALIB, PG_LOCOMOTION, PG_HUD, PG_HUD_CROP,
           PG_HUD_WRIST, PG_CHEATS, PG_GRAPHICS, PG_GRAPHICS_DISTANCES,
           PG_CONTROLS, PG_CONTROLS_TIPS, PG_ABOUT, PG_BASKETBALL,
           PG_BASKETBALL_CALIB,
           PG_VEHICLE_CAMERA };
    static int  menuPage = PG_NONE;
    static int  mainSel = 0, cheatSel = 0, cheatCategory = -1, calibSel = 0;
    static int  calibWeaponType = 0;
    static int  holsterCalibSel = 0, holsterSel = 0, drivingSel = 0;
    static int  drivingVehicleType = savr::driving::VEHICLE_BIKE;
    static int  drivingCalibSel = 0, drivingCalibHand = 0;
    static int  hudWristMode = 0;   // 0 auto (hand/dash), 4 weapon, 5 two-hand
    static int  basketballSel = 0, basketballCalibSel = 0;
    static int  vehicleCameraSel = 0;
    static int  locomotionSel = 0, hudSel = 0, hudCropSel = 0, hudWristSel = 0,
                gfxSel = 0, gfxDistanceSel = 0, controlsSel = 0;
    static bool aboutFirstRun = false, aboutArmed = false;
    static bool openPrev = false, enterPrev = false, backPrev = false;
    static int  tUp = 0, tDown = 0, tMinus = 0, tPlus = 0;
    // One RIGHT/master calibration drives both hands. Frame counts provide the
    // x1/x3/x10 edit accelerator without relying on a second clock.
    static int  calAdjDir = 0, calAdjHeld = 0, calAdjSince = 0;
    const bool grips = in.grip[0] >= 0.75f && in.grip[1] >= 0.75f;
    const bool menuWasOpen = (menuPage != PG_NONE);   // before this frame's block
    {
        // Tap once, then auto-repeat after a short hold (nav + value edit).
        auto held = [](bool now, int& t) -> bool {
            if (!now) { t = 0; return false; }
            ++t;
            return t == 1 || (t > 18 && (t % 6) == 0);
        };
        // Value edits ramp much harder than navigation: with 0.1cm steps a
        // flat repeat took forever. Tap = 1 step; then the repeat both speeds
        // up and grows the step, reaching every-frame x3 after ~2.5s.
        auto heldValue = [](bool now, int& t) -> int {
            if (!now) { t = 0; return 0; }
            ++t;
            if (t == 1) return 1;
            if (t <= 18) return 0;
            if (t <= 54)  return (t % 6) == 0 ? 1 : 0;   // ~12/s
            if (t <= 108) return (t % 3) == 0 ? 1 : 0;   // ~24/s
            if (t <= 180) return 1;                      // ~72/s
            return 3;                                    // ~216/s
        };

        // Primary open chord is grips+Menu (Vice City style); grips+Y is the
        // fallback for setups where the Menu click is swallowed. grips+B was a
        // legacy third fallback and is deliberately GONE: B now fires the
        // drive-by/held weapon in vehicles, and the hidden chord kept opening
        // the menu mid-fight.
        // Stereo cutscenes own the stick clicks exactly like Vice City: R3
        // cycles director/actor cameras and L3 keeps the current camera for
        // this named scene. Both-click recenter resumes outside a cutscene.
        const bool cutsceneCameras = menuPage == PG_NONE &&
            vrcam::CutsceneCameraControlsActive();
        static bool cutsceneCyclePrev = false;
        static bool cutsceneStorePrev = false;
        const bool cutsceneCycle = cutsceneCameras && in.r3 && !in.l3;
        const bool cutsceneStore = cutsceneCameras && in.l3 && !in.r3;
        if (cutsceneCycle && !cutsceneCyclePrev)
            vrcam::CycleCutsceneCamera();
        if (cutsceneStore && !cutsceneStorePrev)
            vrcam::RememberCutsceneCamera();
        cutsceneCyclePrev = cutsceneCycle;
        cutsceneStorePrev = cutsceneStore;

        // L3+R3 (both thumbstick clicks) recenters the view: the current head
        // pose becomes straight-ahead. Edge-triggered so holding does it once.
        static bool recenterPrev = false;
        const bool recenterChord = !cutsceneCameras && in.l3 && in.r3;
        if (recenterChord && !recenterPrev) vrcam::RequestRecenter();
        recenterPrev = recenterChord;

        // Both grips + R3: quick first/third person toggle for the CURRENT
        // vehicle, no VR-menu trip. Only in a vehicle; L3+R3 keeps priority
        // (recenter); the Rhino is skipped — its view is forced third-person
        // and the toggle would silently flip the shared car setting instead.
        static bool viewChordPrev = false;
        const bool viewChord = !cutsceneCameras && grips && in.r3 && !in.l3;
        if (viewChord && !viewChordPrev && menuPage == PG_NONE) {
            const int vehicleType = savr::driving::GetActiveVehicleType();
            if (vehicleType >= 0 &&
                savr::driving::GetActiveVehicleModelId() != 432) {
                savr::driving::CycleCameraView(vehicleType, +1);
            }
        }
        viewChordPrev = viewChord;

        // First-run welcome waits for the first controllable player step. Stereo
        // can also be active in story/scripted cutscenes, so `inGameplay` alone
        // is not proof that the left stick belongs to CJ yet.
        const bool playerExists = g.FindPlayerPed && g.FindPlayerPed(-1);
        const bool formalCutscene = g.CCutsceneMgr_ms_running &&
            *g.CCutsceneMgr_ms_running;
        bool controlsEnabled = true;
        if (g.CPad_GetPad) {
            if (const void* pad = g.CPad_GetPad(0)) {
                controlsEnabled = *reinterpret_cast<const std::uint16_t*>(
                    static_cast<const char*>(pad) + 0x110) == 0;
            }
        }
        const bool widescreen = g.TheCamera &&
            *(reinterpret_cast<const std::uint8_t*>(g.TheCamera) + 0x43) != 0;
        if (menuPage == PG_NONE && inGameplay && playerExists &&
            !formalCutscene && controlsEnabled && !widescreen &&
            !savr::locomotion::WelcomeSeen() &&
            (std::abs(in.leftStick[0]) >= 0.2f ||
             std::abs(in.leftStick[1]) >= 0.2f)) {
            menuPage = PG_ABOUT;
            aboutFirstRun = true;
            aboutArmed = false;
        }

        const bool openChord = grips && (in.menu || in.y);
        if (openChord && !openPrev) {
            if (menuPage == PG_HOLSTER_CALIB) savr::holster::EndCalibrationPreview();
            if (menuPage == PG_BASKETBALL_CALIB)
                savr::basketball::EndHandCalibration();
            menuPage = (menuPage == PG_NONE) ? PG_MAIN : PG_NONE;   // toggle whole menu
            mainSel = 0;
        }
        openPrev = openChord;

        const bool navUp   = held(in.leftStick[1] >  0.65f, tUp);
        const bool navDown = held(in.leftStick[1] < -0.65f, tDown);
        // Ints carrying the acceleration magnitude; every existing boolean use
        // (`if (minus)`, `minus?-1:...`) keeps its meaning unchanged.
        const int minus = heldValue(
            in.leftStick[0] < -0.65f || in.triggers[0] >= 0.55f, tMinus);
        const int plus = heldValue(
            in.leftStick[0] >  0.65f || in.triggers[1] >= 0.55f, tPlus);
        const bool enter   = in.a && !enterPrev && !grips;   // A: enter/activate/reset
        const bool back    = in.b && !backPrev && !grips;    // B: step back / close (grips = open chord)
        enterPrev = in.a; backPrev = in.b;

        switch (menuPage) {
        case PG_MAIN: {
            // Weapon/holster setup, driving, HUD, hand appearance, cheats,
            // graphics, controls, about and close.
            const int N = 13;
            if (navUp)   mainSel = (mainSel - 1 + N) % N;
            if (navDown) mainSel = (mainSel + 1) % N;
            if (mainSel == 6) {
                if (minus) savr::appearance::SetHandSkin(savr::appearance::HAND_SKIN_LIGHT);
                if (plus)  savr::appearance::SetHandSkin(savr::appearance::HAND_SKIN_DARK);
                if (enter) savr::appearance::CycleHandSkin();
            }
            if (enter) {
                if      (mainSel == 0) {
                    calibWeaponType = savr::calib::ActiveWeapon();
                    menuPage = PG_CALIB;
                    calibSel = 0;
                }
                else if (mainSel == 1) {
                    savr::holster::BeginCalibrationPreview();
                    menuPage = PG_HOLSTER_CALIB;
                    holsterCalibSel = 0;
                }
                else if (mainSel == 2) { menuPage = PG_HOLSTERS; holsterSel = 0; }
                else if (mainSel == 3) {
                    const int activeType=savr::driving::GetActiveVehicleType();
                    if (activeType!=savr::driving::VEHICLE_NONE)
                        drivingVehicleType=activeType;
                    menuPage = PG_DRIVING; drivingSel = 0;
                }
                else if (mainSel == 4) { menuPage = PG_LOCOMOTION; locomotionSel = 0; }
                else if (mainSel == 5) { menuPage = PG_HUD;      hudSel = 0; }
                else if (mainSel == 7) {
                    menuPage = PG_CHEATS; cheatCategory = -1; cheatSel = 0;
                }
                else if (mainSel == 8) { menuPage = PG_GRAPHICS; gfxSel = 0; }
                else if (mainSel == 9) { menuPage = PG_CONTROLS; controlsSel = 0; }
                else if (mainSel == 10) { menuPage = PG_BASKETBALL; basketballSel = 0; }
                else if (mainSel == 11) { menuPage = PG_ABOUT; aboutFirstRun = false; aboutArmed = false; }
                else if (mainSel == 12)  menuPage = PG_NONE;
            }
            if (back) menuPage = PG_NONE;
            break;
        }
        case PG_BASKETBALL: {
            constexpr int ROWS = savr::basketball::PHYS_FIELD_COUNT + 3;
            if (navUp)   basketballSel = (basketballSel - 1 + ROWS) % ROWS;
            if (navDown) basketballSel = (basketballSel + 1) % ROWS;
            const int step = minus ? -1 : (plus ? 1 : 0);
            if (basketballSel < savr::basketball::PHYS_FIELD_COUNT) {
                if (step) savr::basketball::AdjustPhysics(basketballSel, step);
            } else if (basketballSel == savr::basketball::PHYS_FIELD_COUNT) {
                if (enter) {
                    savr::basketball::BeginHandCalibration();
                    basketballCalibSel = 0;
                    menuPage = PG_BASKETBALL_CALIB;
                }
            } else if (basketballSel == savr::basketball::PHYS_FIELD_COUNT + 1) {
                if (enter) savr::basketball::SpawnBall();
            } else if (enter) {
                menuPage = PG_MAIN;
            }
            if (back) menuPage = PG_MAIN;
            break;
        }
        case PG_BASKETBALL_CALIB: {
            constexpr int ROWS = savr::basketball::HAND_CALIB_FIELD_COUNT + 2;
            if (navUp) basketballCalibSel = (basketballCalibSel - 1 + ROWS) % ROWS;
            if (navDown) basketballCalibSel = (basketballCalibSel + 1) % ROWS;
            const int step = minus ? -minus : (plus ? plus : 0);
            if (basketballCalibSel < savr::basketball::HAND_CALIB_FIELD_COUNT) {
                if (step) savr::basketball::AdjustHandCalib(
                    basketballCalibSel, step);
            } else if (basketballCalibSel ==
                       savr::basketball::HAND_CALIB_FIELD_COUNT) {
                if (enter) savr::basketball::ResetHandCalibration();
            } else if (enter) {
                savr::basketball::EndHandCalibration();
                menuPage = PG_BASKETBALL;
            }
            if (back) {
                savr::basketball::EndHandCalibration();
                menuPage = PG_BASKETBALL;
            }
            break;
        }
        case PG_CALIB: {
            // 26 rows: 0..18 fields, 19 WEAPON LASER (global), 20 LASER BEAM
            // mode, 21 LOCK LASER, 22 WEAPON RECOIL, 23 TRACER COLOR,
            // 24 TRACER SMOKE SPREAD, 25 BACK.
            constexpr int ROWS = 26;
            if (navUp)   calibSel = (calibSel - 1 + ROWS) % ROWS;
            if (navDown) calibSel = (calibSel + 1) % ROWS;
            const int type = calibWeaponType;

            // Value adjust (accelerated): L2 / stick-left = -1, R2 / stick-right = +1.
            const int dir = (in.triggers[1] >= 0.55f || in.leftStick[0] >  0.65f) ? +1
                          : (in.triggers[0] >= 0.55f || in.leftStick[0] < -0.65f) ? -1 : 0;
            if (dir != calAdjDir) { calAdjDir = dir; calAdjHeld = 0; calAdjSince = 999; }
            bool fire = false;
            if (dir != 0) {
                const int interval = (calAdjHeld < 12) ? 100000 : 4;   // tap, then auto-repeat
                if (calAdjHeld == 0 || calAdjSince >= interval) { fire = true; calAdjSince = 0; }
                ++calAdjHeld; ++calAdjSince;
            }
            const int  mult      = (calAdjHeld < 45) ? 1 : (calAdjHeld < 105) ? 3 : 10;  // x1/x3/x10
            const bool firstTick = fire && calAdjHeld == 1;

            if (calibSel == 25) {                                  // BACK
                if (enter || firstTick || back) { savr::calib::Save(); menuPage = PG_MAIN; }
            } else if (calibSel == 24) {                           // TRACER SMOKE SPREAD
                if (enter) vrcam::AdjustTracerSmokeSpread(+1);
                else if (firstTick) vrcam::AdjustTracerSmokeSpread(dir);
            } else if (calibSel == 23) {                           // TRACER COLOR
                if (enter) vrcam::AdjustTracerColorMode(+1);
                else if (firstTick) vrcam::AdjustTracerColorMode(dir);
            } else if (calibSel == 22) {                           // WEAPON RECOIL
                if (enter) savr::calib::CycleRecoil(+1);
                else if (firstTick) savr::calib::CycleRecoil(dir);
            } else if (calibSel == 21) {                           // explicit persistent lock
                if (enter || firstTick) savr::calib::LockLaser(type);
            } else if (calibSel == 20) {                           // per-weapon beam override
                if (enter) savr::calib::CycleLaserModeForWeapon(type, +1);
                else if (firstTick) savr::calib::CycleLaserModeForWeapon(type, dir);
            } else if (calibSel == 19) {                           // global laser toggle
                if (enter) savr::calib::ToggleLaser();
                else if (firstTick) savr::calib::SetLaserEnabled(dir > 0);
            } else if (calibSel == savr::calib::F_SUP_STYLE) {     // SUPPORT STYLE toggle
                if (enter || firstTick) {
                    const int cur = savr::calib::GetField(1, type, savr::calib::F_SUP_STYLE);
                    savr::calib::SetField(1, type, savr::calib::F_SUP_STYLE, cur ? 0 : 1);
                    savr::calib::Save();
                }
            } else if (fire) {                                     // numeric fields 0..17
                savr::calib::AdjustField(1, type, calibSel, dir * mult);
            }
            if (back) { savr::calib::Save(); menuPage = PG_MAIN; }
            break;
        }
        case PG_HOLSTER_CALIB: {
            // PC SA layout: owned weapon selector + six per-type pose fields + Back.
            constexpr int ROWS = 8;
            if (navUp)   holsterCalibSel = (holsterCalibSel - 1 + ROWS) % ROWS;
            if (navDown) holsterCalibSel = (holsterCalibSel + 1) % ROWS;

            int point = -1, slot = -1, type = 0;
            const bool havePreview = savr::holster::GetCalibrationPreview(&point, &slot, &type);
            if (holsterCalibSel == 0) {
                if (minus) savr::holster::CycleCalibrationPreviewWeapon(-1);
                if (plus || enter) savr::holster::CycleCalibrationPreviewWeapon(+1);
            } else if (holsterCalibSel <= savr::calib::H_COUNT && havePreview) {
                const int step = minus ? -1 : (plus ? 1 : 0);
                if (step) savr::calib::AdjustHolsterField(type, holsterCalibSel - 1, step);
            } else if (holsterCalibSel == ROWS - 1 && enter) {
                savr::holster::EndCalibrationPreview();
                menuPage = PG_MAIN;
            }
            if (back) {
                savr::holster::EndCalibrationPreview();
                menuPage = PG_MAIN;
            }
            break;
        }
        case PG_HOLSTERS: {
            // Points, GRAB REACH, GRIP LOCK, HOLSTER MARKERS, BACK — matching
            // BuildHolsterMenu.
            const int rows = savr::holster::PointCount() + 4;
            if (navUp)   holsterSel = (holsterSel - 1 + rows) % rows;
            if (navDown) holsterSel = (holsterSel + 1) % rows;
            if (holsterSel < savr::holster::PointCount()) {
                if (minus) savr::holster::CyclePointSlot(holsterSel, -1);
                if (plus) savr::holster::CyclePointSlot(holsterSel, +1);
                // A is the per-socket visibility switch. The category remains
                // assigned, so hiding a bulky back/waist weapon is reversible.
                if (enter) savr::holster::TogglePointVisible(holsterSel);
            } else if (holsterSel == savr::holster::PointCount()) {
                if (minus) savr::holster::AdjustGrabRadiusCm(-1);
                if (plus)  savr::holster::AdjustGrabRadiusCm(+1);
            } else if (holsterSel == savr::holster::PointCount() + 1) {
                if (minus || plus || enter) savr::holster::ToggleGripLock();
            } else if (holsterSel == savr::holster::PointCount() + 2) {
                if (minus) savr::holster::SetGripMarkersEnabled(false);
                if (plus)  savr::holster::SetGripMarkersEnabled(true);
                if (enter) savr::holster::ToggleGripMarkers();
            } else if (enter) {
                menuPage = PG_MAIN;
            }
            if (back) menuPage = PG_MAIN;
            break;
        }
        case PG_DRIVING: {
            const int rows=savr::driving::GetMenuItemCount(drivingVehicleType);
            drivingSel=std::clamp(drivingSel,0,rows-1);
            if (navUp)   drivingSel = (drivingSel - 1 + rows) % rows;
            if (navDown) drivingSel = (drivingSel + 1) % rows;
            const int step = minus ? -1 : (plus ? 1 : 0);
            const int item=savr::driving::GetMenuItemForRow(
                drivingVehicleType,drivingSel);
            const bool available=savr::driving::IsMenuItemAvailable(
                drivingVehicleType,item);
            if (item==savr::driving::MENU_VEHICLE_TYPE&&(enter||step)) {
                const int typeCount=savr::driving::VEHICLE_TYPE_COUNT;
                const int direction=step<0?-1:+1;
                drivingVehicleType=(drivingVehicleType+typeCount+direction)%
                                   typeCount;
                drivingSel=0;
            } else if (item==savr::driving::MENU_DRIVING_TYPE&&(enter||step)) {
                savr::driving::CycleModeForVehicleType(
                    drivingVehicleType,step?step:+1);
                drivingSel=std::min(drivingSel,
                    savr::driving::GetMenuItemCount(drivingVehicleType)-1);
            } else if (item==savr::driving::MENU_CAMERA_VIEW&&(enter||step)) {
                savr::driving::CycleCameraView(drivingVehicleType,step?step:+1);
            } else if (item==savr::driving::MENU_YOKE_SENSITIVITY&&step) {
                savr::driving::AdjustYokeSensitivity(step);
            } else if (item==savr::driving::MENU_BICYCLE_MODE&&(enter||step)) {
                savr::driving::CycleBicycleImmersiveMode(step?step:+1);
            } else if (item==savr::driving::MENU_BIKE_ACCELERATOR&&
                      (enter||step)&&available) {
                savr::driving::CycleCurrentBikeAcceleratorMode(step?step:+1);
            } else if (item==savr::driving::MENU_GLOBAL_SIDE&&step) {
                savr::driving::AdjustGlobalSeatSideCm(drivingVehicleType,step);
            } else if (item==savr::driving::MENU_GLOBAL_FORWARD&&step) {
                savr::driving::AdjustGlobalSeatForwardCm(drivingVehicleType,step);
            } else if (item==savr::driving::MENU_GLOBAL_HEIGHT&&step) {
                savr::driving::AdjustGlobalSeatHeightCm(drivingVehicleType,step);
            } else if (item==savr::driving::MENU_MODEL_SIDE&&step&&available) {
                savr::driving::AdjustCurrentModelSeatSideCm(drivingVehicleType,step);
            } else if (item==savr::driving::MENU_MODEL_FORWARD&&step&&available) {
                savr::driving::AdjustCurrentModelSeatForwardCm(drivingVehicleType,step);
            } else if (item==savr::driving::MENU_MODEL_HEIGHT&&step&&available) {
                savr::driving::AdjustCurrentModelSeatHeightCm(drivingVehicleType,step);
            } else if (item==savr::driving::MENU_CONTROL_CALIBRATION&&enter&&available) {
                menuPage=PG_DRIVING_CALIB;
                drivingCalibSel=0;
                drivingCalibHand=0;
            } else if (item==savr::driving::MENU_HANDLE_HIGHLIGHTS&&enter) {
                savr::driving::ToggleHandleHighlights();
            } else if (item==savr::driving::MENU_BIKE_HAND_TILT&&enter) {
                savr::driving::ToggleBikeHandsFollowTilt();
            } else if (item==savr::driving::MENU_LOCAL_HORIZON&&enter) {
                savr::driving::ToggleBikeHorizonLock();
            } else if (item==savr::driving::MENU_BIKE_VISUAL_LEAN&&step) {
                savr::driving::AdjustBikeVisualLeanPercent(step);
            } else if (item==savr::driving::MENU_KEEP_RIDER_ON_FLIPS&&enter) {
                savr::driving::ToggleKeepRiderOnFlips();
            } else if (item==savr::driving::MENU_WHEEL_VISIBLE&&enter) {
                savr::driving::ToggleWheelVisible();
            } else if (item==savr::driving::MENU_CAR_CAMERA_TILT&&
                       (enter||step)) {
                savr::driving::ToggleCarCameraTilt();
            } else if (item==savr::driving::MENU_BOAT_CAMERA_TILT&&
                       (enter||step)) {
                savr::driving::ToggleBoatCameraTilt();
            } else if (item==savr::driving::MENU_INTERIOR_GLASS&&enter) {
                savr::driving::ToggleInteriorGlass();
            } else if (item==savr::driving::MENU_DRIVEBY_AIM&&enter) {
                savr::driving::ToggleDrivebyAimImmersive();
            } else if (item==savr::driving::MENU_RESET&&enter) {
                savr::driving::ResetVehiclePreset(drivingVehicleType);
            } else if (item==savr::driving::MENU_BACK&&enter) {
                menuPage=PG_MAIN;
            }
            if (back) menuPage = PG_MAIN;
            break;
        }
        case PG_DRIVING_CALIB: {
            // Layout must match BuildDrivingCalibrationMenu: for wheeled
            // types the WHEEL fields come first, then the grip block.
            const bool hasWheel =
                drivingVehicleType!=savr::driving::VEHICLE_BIKE;
            const int wheelRows =
                hasWheel?savr::driving::WHEEL_CAL_FIELD_COUNT:0;
            const int ROWS=wheelRows+savr::driving::CONTROL_FIELD_COUNT+2;
            if (!savr::driving::IsControlCalibrationAvailable(
                    drivingVehicleType)) {
                menuPage=PG_DRIVING;
                drivingSel=0;
                break;
            }
            if (navUp) drivingCalibSel=(drivingCalibSel-1+ROWS)%ROWS;
            if (navDown) drivingCalibSel=(drivingCalibSel+1)%ROWS;
            const int step=minus?-1:(plus?1:0);
            const int stepValue=step?step:0;
            if (drivingCalibSel<wheelRows) {
                if (stepValue)
                    savr::driving::AdjustWheelCalibrationValue(
                        drivingCalibSel,stepValue);
            } else if (drivingCalibSel==wheelRows&&(enter||step)) {
                drivingCalibHand=1-drivingCalibHand;
            } else if (drivingCalibSel>wheelRows&&
                       drivingCalibSel<=wheelRows+
                           savr::driving::CONTROL_FIELD_COUNT&&step) {
                savr::driving::AdjustControlCalibrationValue(
                    drivingCalibHand,drivingCalibSel-wheelRows-1,step);
            } else if (drivingCalibSel==ROWS-1&&enter) {
                menuPage=PG_DRIVING;
            }
            if (back) menuPage=PG_DRIVING;
            break;
        }
        case PG_VEHICLE_CAMERA: {
            constexpr int ROWS=5;
            if (navUp) vehicleCameraSel=(vehicleCameraSel-1+ROWS)%ROWS;
            if (navDown) vehicleCameraSel=(vehicleCameraSel+1)%ROWS;
            const int step=minus?-1:(plus?1:0);
            if (vehicleCameraSel==0&&(step||enter))
                savr::locomotion::ToggleFlightCameraTilt();
            else if (vehicleCameraSel==1&&(step||enter))
                savr::driving::ToggleCarCameraTilt();
            else if (vehicleCameraSel==2&&(step||enter))
                savr::driving::ToggleBikeHorizonLock();
            else if (vehicleCameraSel==3&&(step||enter))
                savr::driving::ToggleBoatCameraTilt();
            else if (vehicleCameraSel==4&&enter)
                menuPage=PG_LOCOMOTION;
            if (back) menuPage=PG_LOCOMOTION;
            break;
        }
        case PG_LOCOMOTION: {
            constexpr int ROWS=15;
            if (navUp) locomotionSel=(locomotionSel-1+ROWS)%ROWS;
            if (navDown) locomotionSel=(locomotionSel+1)%ROWS;
            const int step=minus?-1:(plus?1:0);
            if (locomotionSel==0&&(step||enter))
                savr::locomotion::CycleMovementMode(step?step:+1);
            else if (locomotionSel==1&&(step||enter))
                savr::locomotion::CycleTurnMode(step?step:+1);
            else if (locomotionSel==2&&step)
                savr::locomotion::AdjustTurnSensitivity(step);
            else if (locomotionSel==3&&step)
                savr::locomotion::AdjustSnapAngle(step);
            else if (locomotionSel==4&&enter)
                savr::locomotion::ToggleHeadBob();
            else if (locomotionSel==5&&(step||enter))
                savr::locomotion::ToggleGestureRun();
            else if (locomotionSel==6&&(step||enter))
                savr::locomotion::ToggleGestureSwim();
            else if (locomotionSel==7&&(step||enter))
                savr::locomotion::ToggleParachuteCameraFollow();
            else if (locomotionSel==8&&(step||enter))
                savr::locomotion::ToggleParachuteControl();
            else if (locomotionSel==9&&(step||enter))
                savr::locomotion::ToggleAutoParachute();
            else if (locomotionSel==10&&enter) {
                menuPage=PG_VEHICLE_CAMERA;
                vehicleCameraSel=0;
            }
            else if (locomotionSel==11&&(step||enter))
                savr::locomotion::CycleCutsceneMode(step<0?-1:+1);
            else if (locomotionSel==12&&(step||enter))
                savr::locomotion::CycleGameCutsceneMode(step<0?-1:+1);
            else if (locomotionSel==13&&enter)
                vrcam::RequestRecenter();
            else if (locomotionSel==14&&enter)
                menuPage=PG_MAIN;
            if (back) menuPage=PG_MAIN;
            break;
        }
        case PG_HUD: {
            // Root: presets, element pick, and the two calibration submenus.
            // Row indices must match BuildHudMenu page 0.
            constexpr int ROWS = 11;
            if (navUp)   hudSel = (hudSel - 1 + ROWS) % ROWS;
            if (navDown) hudSel = (hudSel + 1) % ROWS;
            const int step=minus?-1:(plus?1:0);
            const int element=savr::hud::CalibrationElement();
            const bool directText=savr::hud::IsDirectTextElement(element);
            if (hudSel == 0) {
                if (minus) savr::hud::SetPreset(savr::hud::IMMERSIVE);
                if (plus)  savr::hud::SetPreset(savr::hud::CLASSIC);
                if (enter) savr::hud::TogglePreset();
            } else if (hudSel == 1) {
                if (minus) savr::hud::SetGameplayHudEnabled(false);
                if (plus)  savr::hud::SetGameplayHudEnabled(true);
                if (enter) savr::hud::ToggleGameplayHud();
            } else if (hudSel == 2) {
                if (minus) savr::hud::SetGazeAutoHideEnabled(false);
                if (plus)  savr::hud::SetGazeAutoHideEnabled(true);
                if (enter) savr::hud::ToggleGazeAutoHide();
            } else if (hudSel==3&&(step||enter)) {
                savr::hud::CycleCalibrationElement(step?step:+1);
            } else if (hudSel==4&&(step||enter)) {
                if (step<0) savr::hud::SetElementEnabled(element,false);
                else if (step>0) savr::hud::SetElementEnabled(element,true);
                else {
                    const bool enabled=
                        savr::hud::GetElementSettings(element).enabled;
                    savr::hud::SetElementEnabled(element,!enabled);
                }
            } else if (hudSel==5&&enter) {
                menuPage=PG_HUD_CROP; hudCropSel=0;
            } else if (hudSel==6&&enter) {
                if (!directText) {
                    menuPage=PG_HUD_WRIST; hudWristSel=0; hudWristMode=0;
                }
            } else if (hudSel==7&&enter) {
                if (!directText) {
                    menuPage=PG_HUD_WRIST; hudWristSel=0; hudWristMode=4;
                }
            } else if (hudSel==8&&enter) {
                if (!directText) {
                    menuPage=PG_HUD_WRIST; hudWristSel=0; hudWristMode=5;
                }
            } else if (hudSel==9&&enter) {
                savr::hud::ResetElement(element);
            } else if (hudSel==10&&enter) {
                menuPage = PG_MAIN;
            }
            if (back) menuPage = PG_MAIN;
            break;
        }
        case PG_HUD_CROP: {
            // Sprite crop + screen placement. Rows match BuildHudMenu page 1.
            constexpr int ROWS = 11;
            if (navUp)   hudCropSel = (hudCropSel - 1 + ROWS) % ROWS;
            if (navDown) hudCropSel = (hudCropSel + 1) % ROWS;
            // Full acceleration magnitude reaches the value adjustments.
            const int step=plus>0?plus:(minus>0?-minus:0);
            const int element=savr::hud::CalibrationElement();
            const bool directText=savr::hud::IsDirectTextElement(element);
            if (hudCropSel<=8&&step) {
                // Rows 0..8 map to SOURCE_X..ELEMENT_SCALE (fields 1..9).
                // Direct text has no source image: source rows are no-ops.
                if (!directText||hudCropSel>=4)
                    savr::hud::AdjustElementField(element,hudCropSel+1,step);
            } else if (hudCropSel==9&&enter) {
                xr::StartHudSourceScan();
            } else if (hudCropSel==10&&enter) {
                menuPage=PG_HUD;
            }
            if (back) menuPage=PG_HUD;
            break;
        }
        case PG_HUD_WRIST: {
            // Wrist/dash placement (IMMERSIVE): the seven Vice City degrees
            // of freedom, live-previewed on the arm — or, while sitting in a
            // vehicle, on the wheel-centre dashboard slot. Rows match
            // BuildHudMenu pages 2/3.
            constexpr int ROWS = 8;
            if (navUp)   hudWristSel = (hudWristSel - 1 + ROWS) % ROWS;
            if (navDown) hudWristSel = (hudWristSel + 1) % ROWS;
            // Full acceleration magnitude reaches the value adjustments.
            const int step=plus>0?plus:(minus>0?-minus:0);
            const int element=savr::hud::CalibrationElement();
            const bool driving = g.FindPlayerVehicle &&
                g.FindPlayerVehicle(-1, false) != nullptr;
            const int slot =
                hudWristMode == 4 ? savr::hud::WRIST_SLOT_WEAPON :
                hudWristMode == 5 ? savr::hud::WRIST_SLOT_TWOHAND :
                driving ? savr::hud::WRIST_SLOT_VEHICLE
                        : savr::hud::WRIST_SLOT_HAND;
            const int dashModel =
                (hudWristMode == 0 && driving)
                    ? savr::driving::GetActiveVehicleModelId() : -1;
            if (hudWristSel<=6&&step) {
                savr::hud::AdjustWristField(element,hudWristSel,step,slot,
                                            dashModel);
            } else if (hudWristSel==7&&enter) {
                menuPage=PG_HUD;
            }
            if (back) menuPage=PG_HUD;
            break;
        }
        case PG_CHEATS: {
            const int itemCount = cheatCategory < 0
                ? savr::cheats::CATEGORY_COUNT
                : savr::cheats::CategoryCount(cheatCategory);
            const int rows = itemCount + 1; // explicit BACK
            if (navUp)   cheatSel = (cheatSel - 1 + rows) % rows;
            if (navDown) cheatSel = (cheatSel + 1) % rows;
            const int step=minus?-1:(plus?1:0);
            if (cheatCategory>=0&&cheatSel<itemCount&&step)
                savr::cheats::CycleCategoryItem(cheatCategory,cheatSel,step);
            if (enter) {
                if (cheatSel == itemCount) {
                    if (cheatCategory >= 0) { cheatCategory=-1; cheatSel=0; }
                    else menuPage=PG_MAIN;
                } else if (cheatCategory < 0) {
                    cheatCategory=cheatSel; cheatSel=0;
                } else {
                    savr::cheats::ActivateCategoryItem(cheatCategory,cheatSel);
                }
            }
            if (back) {
                if (cheatCategory >= 0) { cheatCategory=-1; cheatSel=0; }
                else menuPage=PG_MAIN;
            }
            break;
        }
        case PG_GRAPHICS: {
            // scale, CPU, GPU, world effects, neon, grading, dynamic shadows,
            // weapon models, distances, defaults, Back
            const int N = 11;
            if (navUp)   gfxSel = (gfxSel - 1 + N) % N;
            if (navDown) gfxSel = (gfxSel + 1) % N;
            const int step = minus ? -1 : (plus ? 1 : 0);
            if (step && gfxSel == 0) {
                vrcam::AdjustRenderScale(step);
            } else if (step && gfxSel <= 2) {
                int cpu = xr::GetCpuPerfIdx(), gpu = xr::GetGpuPerfIdx();
                if (gfxSel == 1) cpu = (cpu + step + 4) % 4;
                else             gpu = (gpu + step + 4) % 4;
                xr::SetPerfLevels(cpu, gpu);
            } else if (step && gfxSel == 3) {
                vrcam::SetWorldEffectsEnabled(step > 0);
            } else if (step && gfxSel == 4) {
                vrcam::SetNeonSignsEnabled(step > 0);
            } else if (step && gfxSel == 5) {
                vrcam::SetColorGradingEnabled(step > 0);
            } else if (step && gfxSel == 6) {
                vrcam::AdjustDynamicShadowMode(step);
            } else if (step && gfxSel == 7 && savr::hdweapons::Available()) {
                vrcam::SetHdWeaponsEnabled(step > 0);
            }
            if (enter && !step && gfxSel == 3) {
                vrcam::SetWorldEffectsEnabled(
                    !vrcam::AreWorldEffectsEnabled());
            } else if (enter && !step && gfxSel == 4) {
                vrcam::SetNeonSignsEnabled(!vrcam::AreNeonSignsEnabled());
            } else if (enter && !step && gfxSel == 5) {
                vrcam::SetColorGradingEnabled(!vrcam::IsColorGradingEnabled());
            } else if (enter && !step && gfxSel == 6) {
                vrcam::AdjustDynamicShadowMode(1);
            } else if (enter && !step && gfxSel == 7) {
                if (savr::hdweapons::Available())
                    vrcam::SetHdWeaponsEnabled(!vrcam::IsHdWeaponsEnabled());
            } else if (enter && gfxSel == 8) {
                menuPage = PG_GRAPHICS_DISTANCES;
                gfxDistanceSel = 0;
            } else if (enter && gfxSel == 9) {
                vrcam::ResetGraphicsDefaults();
                xr::SetPerfLevels(3, 3);
            } else if (enter && gfxSel == 10) {
                menuPage = PG_MAIN;
            }
            if (back) menuPage = PG_MAIN;
            break;
        }
        case PG_CONTROLS: {
            // Layout + five independent stick options + six button sources.
            // This page intentionally edits only the gameplay mapping; its own
            // navigation keeps consuming the raw left stick above.
            const int firstStick = 1;
            const int firstButton = firstStick + savr::locomotion::STICK_OPT_COUNT;
            const int tipsRow = firstButton + savr::locomotion::BIND_SRC_COUNT;
            const int resetRow = tipsRow + 1;
            const int backRow = resetRow + 1;
            const int NC = backRow + 1;
            if (navUp)   controlsSel = (controlsSel - 1 + NC) % NC;
            if (navDown) controlsSel = (controlsSel + 1) % NC;
            const int step = minus ? -1 : (plus ? 1 : 0);
            if (controlsSel == 0 && (step || enter)) {
                const int layout = savr::locomotion::ControlsLayout();
                savr::locomotion::ApplyControlsLayout(
                    layout == savr::locomotion::CONTROLS_LAYOUT_DEFAULT
                        ? savr::locomotion::CONTROLS_LAYOUT_SWAPPED_HANDS
                        : savr::locomotion::CONTROLS_LAYOUT_DEFAULT);
            } else if (controlsSel >= firstStick && controlsSel < firstButton &&
                       (step || enter)) {
                savr::locomotion::ToggleStickOption(controlsSel - firstStick);
            } else if (controlsSel >= firstButton && controlsSel < tipsRow && step) {
                savr::locomotion::CycleButtonBinding(controlsSel - firstButton, step);
            }
            if (enter && controlsSel == tipsRow) menuPage = PG_CONTROLS_TIPS;
            if (enter && controlsSel == resetRow) savr::locomotion::ResetControls();
            if (enter && controlsSel == backRow) menuPage = PG_MAIN;
            if (back) menuPage = PG_MAIN;
            break;
        }
        case PG_CONTROLS_TIPS: {
            if (enter || back) menuPage = PG_CONTROLS;
            break;
        }
        case PG_ABOUT: {
            // VC behaviour: any button closes, but only after every button
            // has first been seen released (so the opening press or the
            // movement stick cannot instantly dismiss it).
            const bool anyButton = in.a || in.b || in.x || in.y || in.menu;
            if (!anyButton) aboutArmed = true;
            else if (aboutArmed) {
                menuPage = PG_NONE;
                if (aboutFirstRun) {
                    savr::locomotion::MarkWelcomeSeen();
                    aboutFirstRun = false;
                }
            }
            break;
        }
        case PG_GRAPHICS_DISTANCES: {
            const int settings = vrcam::GetGraphicsDistanceSettingCount();
            const int rows = settings + 1; // explicit BACK
            if (navUp)   gfxDistanceSel = (gfxDistanceSel - 1 + rows) % rows;
            if (navDown) gfxDistanceSel = (gfxDistanceSel + 1) % rows;
            const int step = minus ? -1 : (plus ? 1 : 0);
            if (step && gfxDistanceSel < settings)
                vrcam::AdjustGraphicsDistanceSetting(gfxDistanceSel, step);
            if (enter && gfxDistanceSel == settings)
                menuPage = PG_GRAPHICS;
            if (back) menuPage = PG_GRAPHICS;
            break;
        }
        default: break;
        }

        if (menuPage != PG_HOLSTER_CALIB && savr::holster::CalibrationPreviewActive())
            savr::holster::EndCalibrationPreview();
        if (menuPage != PG_BASKETBALL_CALIB &&
            savr::basketball::HandCalibrationActive())
            savr::basketball::EndHandCalibration();

        xr::SetMainMenu    (menuPage == PG_MAIN,     mainSel);
        xr::SetCalibPage   (menuPage == PG_CALIB, calibSel, 1, calibWeaponType);
        xr::SetHolsterCalibMenu(menuPage == PG_HOLSTER_CALIB, holsterCalibSel);
        xr::SetHolsterMenu (menuPage == PG_HOLSTERS, holsterSel);
        savr::driving::SetMenuPreview(
            menuPage == PG_DRIVING || menuPage == PG_DRIVING_CALIB,
            menuPage == PG_DRIVING_CALIB ? drivingCalibHand : -1);
        xr::SetDrivingMenu(menuPage == PG_DRIVING,drivingSel,
                           drivingVehicleType);
        xr::SetDrivingCalibrationMenu(menuPage == PG_DRIVING_CALIB,
                                      drivingCalibSel,drivingCalibHand);
        xr::SetLocomotionMenu(menuPage == PG_LOCOMOTION,locomotionSel);
        xr::SetVehicleCameraMenu(menuPage == PG_VEHICLE_CAMERA,
                                 vehicleCameraSel);
        xr::SetBasketballMenu(menuPage == PG_BASKETBALL, basketballSel);
        xr::SetBasketballCalibMenu(menuPage == PG_BASKETBALL_CALIB,
                                   basketballCalibSel);
        {
            const bool hudOpen = menuPage == PG_HUD ||
                menuPage == PG_HUD_CROP || menuPage == PG_HUD_WRIST;
            const bool wristDriving = menuPage == PG_HUD_WRIST &&
                g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false);
            const int hudPage = menuPage == PG_HUD_CROP ? 1 :
                (menuPage == PG_HUD_WRIST
                     ? (hudWristMode ? hudWristMode : (wristDriving ? 3 : 2))
                     : 0);
            const int hudPageSel = menuPage == PG_HUD_CROP ? hudCropSel :
                (menuPage == PG_HUD_WRIST ? hudWristSel : hudSel);
            xr::SetHudMenu(hudOpen, hudPageSel, hudPage);
        }
        const int cheatRows = (cheatCategory < 0
            ? savr::cheats::CATEGORY_COUNT
            : savr::cheats::CategoryCount(cheatCategory)) + 1;
        xr::SetMenuState(menuPage == PG_CHEATS,cheatSel,cheatRows,cheatCategory);
        xr::SetGraphicsMenu(menuPage == PG_GRAPHICS, gfxSel);
        xr::SetControlsMenu(menuPage == PG_CONTROLS, controlsSel);
        xr::SetControlsTipsMenu(menuPage == PG_CONTROLS_TIPS);
        xr::SetAboutMenu(menuPage == PG_ABOUT, aboutFirstRun);
        xr::SetGraphicsDistanceMenu(
            menuPage == PG_GRAPHICS_DISTANCES, gfxDistanceSel);
    }
    const bool anyMenu = (menuPage != PG_NONE);

    // The Android port can leave its global pause state latched after the shell
    // loses focus (Meta menu), and the same stale state has been observed at the
    // end of the enter-car task. Rendering/XR continue normally in that state,
    // but simulation stops and OpenAL is attenuated, which looks like a frozen
    // image with missing sound. Only repair it after OpenXR is focused again and
    // while the in-world stereo path is active; genuine frontend/cutscene pause
    // screens therefore keep their normal behaviour.
    static bool wasXrFocused = false;
    const bool xrFocused = xr::IsSessionFocused();
    const bool focusRegained = xrFocused && !wasXrFocused;
    wasXrFocused = xrFocused;
    const bool playerInVehicleNow = g.FindPlayerVehicle &&
        g.FindPlayerVehicle(-1, false) != nullptr;
    if (xrFocused && inGameplay && !anyMenu&&
        (focusRegained || playerInVehicleNow)) {
        const bool userPause = g.CTimer_m_UserPause && *g.CTimer_m_UserPause;
        const bool codePause = g.CTimer_m_CodePause && *g.CTimer_m_CodePause;
        const bool androidPause = g.AndroidPaused && g.AndroidPaused();
        if (userPause || codePause || androidPause) {
            LOGW("[pause.fix] focused gameplay was paused: user=%d code=%d android=%d vehicle=%d",
                 userPause, codePause, androidPause, playerInVehicleNow);
            if (androidPause && g.SetAndroidPaused) g.SetAndroidPaused(0);
            if (userPause && g.CTimer_EndUserPause) g.CTimer_EndUserPause();
            if (g.CTimer_m_UserPause) *g.CTimer_m_UserPause = false;
            if (g.CTimer_m_CodePause) *g.CTimer_m_CodePause = false;
        }
    }

    // The left stick drives the menu, not the player, while a menu is up.
    vrcam::SetInputBlocked(anyMenu);

    // HD weapon model set: registers its image + textures once the engine is
    // ready, before any weapon streams in. Runs outside the gameplay gate so
    // load screens cannot outrun it.
    savr::hdweapons::Tick(vrcam::IsHdWeaponsEnabled());
    // Weapon grip/aim calibration follows the model set actually rendered, so
    // HD calibration lands in its own profile and never overwrites the original.
    savr::calib::SetModelSet(savr::hdweapons::Applied() ? 1 : 0);

    // Physical ownership keeps running while on foot so throws can expire and
    // respawn. Menus/open chords only block transitions and firing; they do not
    // silently return an already-held weapon to its socket.
    if (inGameplay) {
        const bool weaponInteractionsBlocked = anyMenu ||
            vrcam::ParachuteWeaponInteractionBlocked();
        savr::physicalweapon::Update(
            weaponInteractionsBlocked,
            menuPage == PG_HOLSTER_CALIB && savr::holster::GripMarkersEnabled(),
            menuPage == PG_CALIB);
        savr::melee::Update(weaponInteractionsBlocked);
        savr::scopeaim::Update(weaponInteractionsBlocked);
        savr::vrfire::Update(weaponInteractionsBlocked);
        savr::throwable::Update(weaponInteractionsBlocked);
        savr::physicalweapon::EnforceBikeWeaponLimit();
        savr::physicalweapon::AutoEquipParachute();
        savr::physicalweapon::AutoAssignGadgetPoint();
        savr::cheats::Tick();
        savr::pickups::Tick();
        savr::basketball::Update();
    }
    else {
        savr::physicalweapon::ResetTransient();
        savr::melee::Reset();
        savr::scopeaim::Reset();
        savr::throwable::Reset();
    }

    // The old CPU-geometry probe is deliberately disabled. Per-slot RenderWare
    // clumps are now authoritative; publishing the probe for a rare model that
    // retains morph vertices would draw a second orange gun in the GL hand pass.

    static int n = 0;
    static bool loggedA = false;
    static bool loggedB = false;
    static bool loggedX = false;
    static bool loggedY = false;
    static bool loggedMenu = false;
    static bool loggedGrips = false;
    static int loggedPage = -1;
    static bool loggedGameplay = false;
    static bool inputLogInitialised = false;
    const bool inputStateChanged = !inputLogInitialised ||
        in.a != loggedA || in.b != loggedB || in.x != loggedX ||
        in.y != loggedY || in.menu != loggedMenu || grips != loggedGrips ||
        menuPage != loggedPage || inGameplay != loggedGameplay;
    if (++n % 72 == 0 || inputStateChanged) {
        LOGI("input L(%.2f,%.2f) R(%.2f,%.2f) T(%.2f,%.2f) grip(%.2f,%.2f) a=%d b=%d x=%d y=%d menu=%d grips=%d page=%d game=%d",
             in.leftStick[0], in.leftStick[1], in.rightStick[0], in.rightStick[1],
             in.triggers[0], in.triggers[1], in.grip[0], in.grip[1],
             in.a, in.b, in.x, in.y, in.menu, grips, menuPage, inGameplay);
    }
    loggedA = in.a;
    loggedB = in.b;
    loggedX = in.x;
    loggedY = in.y;
    loggedMenu = in.menu;
    loggedGrips = grips;
    loggedPage = menuPage;
    loggedGameplay = inGameplay;
    inputLogInitialised = true;

    auto edge = [&](bool now, bool& prev, int code) {
        if (now && !prev) {
            g.implOnGamepadButtonDown(env, clazz, 0, code);
        } else if (!now && prev) {
            g.implOnGamepadButtonUp(env, clazz, 0, code);
        }
        prev = now;
    };

    if (inGameplay && !anyMenu && !grips) {
        // The engine's stick Y runs the opposite way to OpenXR's, so it is flipped.
        const bool driving = g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false);
        // On foot, whichever physical hand owns the selected gun drives the game's
        // single fire axis. An empty/dropped hand publishes zero. Vehicle controls
        // retain their original left/right trigger mapping.
        const float gameLT = driving ? in.triggers[0] : 0.0f;
        const float gameRT = driving ? in.triggers[1]
                                     : savr::physicalweapon::FireTrigger();
        float gameMoveX=0.0f, gameMoveY=0.0f;
        float gameTurnX=0.0f, gameTurnY=0.0f;
        savr::locomotion::MapGameplaySticks(
            in.leftStick[0], in.leftStick[1],
            in.rightStick[0], in.rightStick[1],
            &gameMoveX, &gameMoveY, &gameTurnX, &gameTurnY);
        // The mobile Java provider writes the pad after CPad::UpdatePads. Feed
        // it the same head-relative vector as the native hook, otherwise these
        // raw axes overwrite room-scale locomotion and movement follows CJ.
        if (!driving) {
            float localHeadYaw=0.0f;
            if (savr::vrcam::GetLocalHeadYaw(&localHeadYaw))
                savr::locomotion::TransformMoveStick(
                    localHeadYaw,&gameMoveX,&gameMoveY);
        }
        // Thumbrest D-pad mode: the provider must not see the raw right
        // stick while it acts as the D-pad.
        const bool dpadMode = (in.thumbrest[0] || in.thumbrest[1]) &&
                              !driving;
        g.implOnGamepadAxesChanged(env, clazz, 0,
                                   gameMoveX, -gameMoveY,
                                   dpadMode ? 0.0f : gameTurnX,
                                   dpadMode ? 0.0f : -gameTurnY,
                                   gameLT, gameRT);

        const bool sprintHeld = savr::locomotion::ActionHeld(
            savr::locomotion::BIND_ACT_SPRINT,
            in.a,in.b,in.x,in.y,in.l3,in.r3,!driving);
        const bool jumpHeld = savr::locomotion::ActionHeld(
            savr::locomotion::BIND_ACT_JUMP,
            in.a,in.b,in.x,in.y,in.l3,in.r3,!driving);
        const bool attackHeld = savr::locomotion::ActionHeld(
            savr::locomotion::BIND_ACT_ATTACK,
            in.a,in.b,in.x,in.y,in.l3,in.r3,!driving);
        static bool pa = false, pb = false, px = false;
        edge(sprintHeld, pa, 0);
        edge(driving ? false : attackHeld, pb, 1);
        // In a vehicle X is owned by Driving's exact NextStationJustUp hook;
        // keep Square released so it cannot compete with the L2 brake mapping.
        edge(driving ? false : jumpHeld, px, 2);
        // Y is handled once by vrcam's mobile ExitVehicleJustDown hook. Sending
        // it through the generic Java provider as well duplicates the enter task.

        // D-pad from the left stick, only in gameplay - or from the RIGHT
        // stick while a thumb rests on a thumbrest (UEVR-parity D-pad
        // mode: gang commands / conversation replies without a chord).
        constexpr float kThreshold = 0.6f;
        static bool pu = false, pd = false, pl = false, pr = false;
        const float dpx = dpadMode ? in.rightStick[0] : in.leftStick[0];
        const float dpy = dpadMode ? in.rightStick[1] : in.leftStick[1];
        edge(dpy >  kThreshold, pu, 8);   // up
        edge(dpy < -kThreshold, pd, 9);   // down
        edge(dpx < -kThreshold, pl, 10);  // left
        edge(dpx >  kThreshold, pr, 11);  // right
    }

    // Open the game's own pause menu. Neither gamepad Start (the provider
    // path) nor CPad::NewState ever pause the mobile build — the phone opens
    // its pause menu through two one-shot bytes polled by OS_ApplicationTick:
    // +1 = "run the resume tick" and +2 = "MobileMenu::InitForPause on that
    // tick" (both live directly after the exported SkipIntroCutscene byte;
    // the stock writers are OS_ApplicationEvent's pause/resume handlers).
    // Setting both from this same GameThread reproduces the exact suspend/
    // resume flow every mobile player sees, with the engine doing the state
    // switch itself on its next tick. Never leak the two-grip VR-menu chord
    // into the stock frontend.
    static bool gameMenuWasDown = false;
    const bool menuGripActive = in.grip[0] >= 0.25f || in.grip[1] >= 0.25f;
    const bool gameMenuDown=in.menu&&!menuGripActive&&!menuWasOpen&&!anyMenu;
    if (gameMenuDown&&!gameMenuWasDown&&inGameplay&&
        g.SkipIntroCutscene!=nullptr) {
        LOGI("[input] left Menu -> engine pause-menu request flags");
        g.SkipIntroCutscene[1]=1;   // resume-tick request
        g.SkipIntroCutscene[2]=1;   // InitForPause request
    }
    gameMenuWasDown=gameMenuDown;

    // B is the VR-menu open chord (grips+B) AND its "back/close", so it must not also
    // trigger the game's Back/pause. Grips are analog and ramp through the threshold,
    // so a B click can land a frame before both grips cross 0.75; gating Back on the
    // instantaneous `grips` would leak a stray pause on every menu-open. Instead defer
    // Back to B's RELEASE edge and cancel it the moment any grip squeeze or menu shows
    // this was a chord, not a lone Back press.
    static bool pb = false, bBackPending = false;
    const bool gripActive = in.grip[0] >= 0.25f || in.grip[1] >= 0.25f;
    if (in.b && !pb) bBackPending = !menuWasOpen;
    if (in.b && (grips || gripActive || menuWasOpen || anyMenu)) bBackPending = false;
    // In a vehicle B is the fire button (stock drive-by in DEFAULT, the held
    // physical weapon in IMMERSIVE) — it must never double as Back/pause.
    if (in.b && inGameplay && g.FindPlayerVehicle &&
        g.FindPlayerVehicle(-1, false) != nullptr)
        bBackPending = false;
    if (!in.b && pb && bBackPending && g.implOnBackButtonPressed != nullptr) {
        LOGI("back button -> implOnBackButtonPressed");
        g.implOnBackButtonPressed(env, clazz);
        bBackPending = false;
    }
    pb = in.b;

    // Pause-map zoom: feed the raw squeeze axes to the Menu_MapUpdate hook
    // (it only acts while the map page is on screen).
    savr::mapzoom::SetGrips(in.grip[0], in.grip[1]);

    // Laser pointer -> touch, menus only. Pointing at the theater screen and
    // clicking (A or the right trigger) is a tap there. In gameplay there is no
    // screen to point at, so leaving this on would fire stray touches, hence the
    // menu gate. A touch only becomes a drag once the pointer has actually moved a
    // real distance from where it went down — else tiny aim jitter turned every
    // tap into a drag that scrolled the screen.
    if (!inGameplay && g.implOnTouchStart != nullptr) {
        const float px = in.pointerU * static_cast<float>(g_gameWidth);
        const float py = in.pointerV * static_cast<float>(g_gameHeight);
        const bool  click = in.pointerValid && (in.a || in.pointerPressed);

        // A tap must read as a tap, not a nudge. Pointing a controller at a
        // screen a few metres away wobbles by a couple of percent just from hand
        // tremor, so a tight move threshold turned every tap into a scroll swipe
        // ("поддёргивание"). Only a deliberate, larger move counts as a drag, and
        // a tap releases exactly where it went down so the menu sees one point.
        static bool  touching = false, dragging = false;
        static float downU = 0.0f, downV = 0.0f;
        static float downPx = 0.0f, downPy = 0.0f;
        if (click && !touching) {
            g.implOnTouchStart(env, clazz, 0, px, py);
            touching = true; dragging = false;
            downU = in.pointerU; downV = in.pointerV;
            downPx = px; downPy = py;
        } else if (click && touching) {
            const float moved = std::abs(in.pointerU - downU) + std::abs(in.pointerV - downV);
            if (!dragging && moved > 0.09f) {
                dragging = true;
            }
            if (dragging) {
                g.implOnTouchMove(env, clazz, 0, px, py);
            }
        } else if (!click && touching) {
            // Clean tap -> up at the down point; a real drag -> up where it ended.
            if (dragging) g.implOnTouchEnd(env, clazz, 0, px, py);
            else          g.implOnTouchEnd(env, clazz, 0, downPx, downPy);
            LOGI("touch %s at (%.0f,%.0f)", dragging ? "drag-end" : "tap", downPx, downPy);
            touching = false;
        }
    }

    // Cutscene skipping is owned by the game's own on-screen arrow; a face
    // button here kept skipping scenes people were actually watching in the
    // first-person modes.
}

void OnDrawFrame(JNIEnv* env, jclass clazz, jfloat deltaTime) {
    // Let the engine render its frame on its own thread, into our SurfaceTexture.
    // Time it: this is the WHOLE game-frame work (logic + render) minus our pacing
    // sleep, so the profiler can split our stereo cost from the game's own cost.
    const int frameCpuCoreStart = sched_getcpu();
    const double frameT0 = perf::MonotonicMs();
    const double frameCpuT0 = perf::ThreadCpuMs();
    const std::uint64_t renderSceneBefore = perf::RenderSceneSerial();
    vrcam::BeginGameFrameTelemetry(frameT0, frameCpuT0);
    if (g_resetGameTiming.exchange(false, std::memory_order_acq_rel)) {
        g_prevGameFrameStartMs = 0.0;
        g_lastPacerFrameNs = 0;
        perf::ResetGameTelemetry();
    }
    const double periodMs = g_prevGameFrameStartMs
        ? (frameT0 - g_prevGameFrameStartMs) : 0.0;   // full game-frame period
    g_prevGameFrameStartMs = frameT0;
    ApplyVrWorldLodScale();
    g.implOnDrawFrame(env, clazz, deltaTime);
    // A normal stereo frame consumes the profile at the large RenderScene
    // entry.  Limiter/menu callbacks may never reach it, so close the scope
    // here to keep later pacing or JNI work out of engine_pre attribution.
    vrcam::EndGameFrameTelemetry();
    const std::uint64_t renderSceneAfter = perf::RenderSceneSerial();
    const int fadeStatus = g.CCamera_GetScreenFadeStatus && g.TheCamera
        ? g.CCamera_GetScreenFadeStatus(g.TheCamera) : -1;
    const int cutsceneRunning = g.CCutsceneMgr_ms_running
        ? (*g.CCutsceneMgr_ms_running ? 1 : 0) : -1;
    const int mobileMenuOpen = g.gMobileMenu
        ? (vrcam::IsMobileMenuOpen() ? 1 : 0) : -1;
    const double implMs = perf::MonotonicMs() - frameT0;                  // game logic + our 2x render
    const double implCpuMs = perf::ThreadCpuMs() - frameCpuT0;
    savr::xr::SetFrameMs(implMs);
    bool wrapperSwapCalled = false;
    bool wrapperSwapSucceeded = false;
    double swapMs = 0.0;
    double sleepRequestedMs = 0.0;
    double sleepActualMs = 0.0;

    auto submitTelemetry = [&] {
        const double doneMs = perf::MonotonicMs();
        const int rsFrameLimit = g.RsGlobal
            ? *reinterpret_cast<const std::int32_t*>(
                  reinterpret_cast<const std::uint8_t*>(g.RsGlobal) + 0x10)
            : 0;
        perf::SubmitGameFrame({
            .monoMs = frameT0,
            .implStartCpuMs = frameCpuT0,
            .callbackPeriodMs = periodMs,
            .javaDeltaSeconds = deltaTime,
            .implWallMs = implMs,
            .implCpuMs = implCpuMs,
            .wrapperSwapCalled = wrapperSwapCalled,
            .wrapperSwapSucceeded = wrapperSwapSucceeded,
            .wrapperSwapMs = swapMs,
            .sleepRequestedMs = sleepRequestedMs,
            .sleepActualMs = sleepActualMs,
            .totalWallMs = doneMs - frameT0,
            .totalCpuMs = perf::ThreadCpuMs() - frameCpuT0,
            .rsFrameLimit = rsFrameLimit,
            .gameFps = g.CTimer_game_FPS ? *g.CTimer_game_FPS : 0.0f,
            .timeStep = g.CTimer_ms_fTimeStep ? *g.CTimer_ms_fTimeStep : 0.0f,
            .timeStepNonClipped = g.CTimer_ms_fTimeStepNonClipped
                ? *g.CTimer_ms_fTimeStepNonClipped : 0.0f,
            .gameFrameCounter = g.CTimer_m_FrameCounter
                ? *g.CTimer_m_FrameCounter : 0u,
            .skipProcess = g.CTimer_bSkipProcessThisFrame
                ? (*g.CTimer_bSkipProcessThisFrame ? 1 : 0) : -1,
            .skipFrame = g.skipFrame ? static_cast<int>(*g.skipFrame) : -1,
            .renderSceneCalls = static_cast<int>(renderSceneAfter - renderSceneBefore),
            .fadeStatus = fadeStatus,
            .cutsceneRunning = cutsceneRunning,
            .mobileMenuOpen = mobileMenuOpen,
            .cpuCoreStart = frameCpuCoreStart,
            .cpuCore = sched_getcpu(),
            .stereoActive = vrcam::IsStereoActive(),
        });
    };

    if (!g_inlineReady) {
        submitTelemetry();
        return;
    }

    SendInputToGame(env, clazz);

    // The engine may have recreated its surface/context; adopt whatever it left
    // current — we are on its thread, so these handles are exactly right.
    g_display = eglGetCurrentDisplay();
    EGLContext c = eglGetCurrentContext();
    EGLSurface d = eglGetCurrentSurface(EGL_DRAW);
    if (c != EGL_NO_CONTEXT) g_context = c;
    if (d != EGL_NO_SURFACE) g_surface = d;

    // Deferred Social Club completions, fired between frames, not re-entrantly.
    if (g_pendingInitialComplete && g.OS_OnRockstarInitialComplete != nullptr) {
        g_pendingInitialComplete = false;
        LOGI("firing OS_OnRockstarInitialComplete");
        g.OS_OnRockstarInitialComplete();
    }
    if (g_pendingGateComplete && g.OS_OnRockstarGateComplete != nullptr) {
        g_pendingGateComplete = false;
        LOGI("firing OS_OnRockstarGateComplete(%d)", g_pendingGateId);
        g.OS_OnRockstarGateComplete(g_pendingGateId, true);
    }

    // Queue the engine's frame to the SurfaceTexture — but only if this thread
    // actually holds a live context. Once gameplay starts the engine renders on
    // its own thread and swaps there; GameThread then has no current display, and
    // swapping anyway just spams EGL_BAD_DISPLAY and freezes the picture.
    if (g_display != EGL_NO_DISPLAY && g_surface != EGL_NO_SURFACE &&
        eglGetCurrentContext() != EGL_NO_CONTEXT) {
        wrapperSwapCalled = true;
        const double swapT0 = perf::MonotonicMs();
        wrapperSwapSucceeded = eglSwapBuffers(g_display, g_surface) == EGL_TRUE;
        swapMs = perf::MonotonicMs() - swapT0;
    }

    // Pace the engine to display rate. Left unbounded it runs at tens of
    // thousands of frames a second, and GTA's boot sequence is frame-timed —
    // splash fades, the frontend hand-off — so at that speed the boot logic
    // never advances correctly. Capping each frame to the headset period is what
    // let the engine reach the menu when the old design happened to pace it.
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long long nowNs = now.tv_sec * 1000000000LL + now.tv_nsec;
    const long long outerPacerPeriodNs =
        g_outerPacerPeriodNs.load(std::memory_order_acquire);
    if (AbsoluteOuterPacerRequested()) {
        if (g_lastPacerFrameNs == 0) {
            g_lastPacerFrameNs = nowNs;
        } else {
            const long long deadlineNs =
                g_lastPacerFrameNs + outerPacerPeriodNs;
            if (nowNs < deadlineNs) {
                const long long requestedNs = deadlineNs - nowNs;
                sleepRequestedMs = static_cast<double>(requestedNs) / 1e6;
                timespec deadline{
                    static_cast<time_t>(deadlineNs / 1000000000LL),
                    static_cast<long>(deadlineNs % 1000000000LL)};
                const double sleepT0 = perf::MonotonicMs();
                int waitResult = 0;
                do {
                    waitResult = clock_nanosleep(
                        CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
                } while (waitResult == EINTR);
                sleepActualMs = perf::MonotonicMs() - sleepT0;
                clock_gettime(CLOCK_MONOTONIC, &now);
                const long long wokeNs =
                    now.tv_sec * 1000000000LL + now.tv_nsec;
                if (waitResult == 0) {
                    // Preserve the ideal deadline, not the slightly late wake.
                    // The next frame automatically repays scheduler overshoot.
                    g_lastPacerFrameNs = deadlineNs;
                } else {
                    static bool loggedFailure = false;
                    if (!loggedFailure) {
                        loggedFailure = true;
                        LOGW("[pacer] clock_nanosleep failed=%d; "
                             "rebasing without catch-up", waitResult);
                    }
                    g_lastPacerFrameNs = wokeNs;
                }
            } else {
                // Real work missed the deadline. Rebase here so one long frame
                // never causes a burst of short catch-up frames.
                g_lastPacerFrameNs = nowNs;
            }
        }
    } else {
        const long long elapsed = nowNs - g_lastPacerFrameNs;
        if (g_lastPacerFrameNs != 0 && elapsed < outerPacerPeriodNs) {
            const long long requestedNs = outerPacerPeriodNs - elapsed;
            sleepRequestedMs = static_cast<double>(requestedNs) / 1e6;
            timespec sleep{0, static_cast<long>(requestedNs)};
            const double sleepT0 = perf::MonotonicMs();
            nanosleep(&sleep, nullptr);
            sleepActualMs = perf::MonotonicMs() - sleepT0;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        g_lastPacerFrameNs = now.tv_sec * 1000000000LL + now.tv_nsec;
    }

    submitTelemetry();
}

// The present-only consumer thread.
//
// The engine keeps rendering into our SurfaceTexture on its own GameThread. This
// thread does nothing but pull each finished frame and hand it to the compositor,
// on its own private EGL context. Because the engine and the compositor never
// share a context or a thread — only the SurfaceTexture's BufferQueue links them
// — neither can stall the other, and the engine boots at full speed while VR
// presents at headset cadence.
void* RenderLoop(void*) {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("could not attach the consumer thread to the VM");
        return nullptr;
    }

    if (!CreateXrGl(g_display) ||
        eglMakeCurrent(g_display, g_pbuffer, g_pbuffer, g_xrContext) != EGL_TRUE) {
        LOGE("could not bring up the XR context: 0x%x", eglGetError());
        g_vm->DetachCurrentThread();
        return nullptr;
    }
    LOGI("consumer thread owns its private XR context");

    AttachGameTexture(env);      // OES texture + SurfaceTexture.attachToGLContext, on g_xrContext
    if (!xr::CreateSession()) {   // binds the session to g_xrContext
        LOGE("no OpenXR session - the game keeps running flat");
    }

    while (g_ownFrameLoop) {
        const double updateT0 = perf::MonotonicMs();
        const double updateCpuT0 = perf::ThreadCpuMs();
        PublishGameFrame(env);   // updateTexImage — newest engine frame into our texture
        xr::RenderFrame(perf::MonotonicMs() - updateT0,
                        perf::ThreadCpuMs() - updateCpuT0); // paced by xrWaitFrame
    }

    // The context is still current here. Complete any shared eye-texture reads
    // before releasing it so a future consumer epoch cannot inherit pinned ring
    // slots or destroy a texture still referenced by the GPU.
    xr::DrainStereoEyeReads();
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    g_vm->DetachCurrentThread();
    return nullptr;
}

// Start the present-only consumer thread.
//
// Runs on GameThread inside OnSurfaceChanged. The engine's context and surface
// are left exactly as they are — the engine keeps rendering on GameThread — and
// the consumer thread stands up its own separate context, so nothing here
// releases or disturbs the engine's EGL state.
void TakeOverFrameLoop() {
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT || g_surface == EGL_NO_SURFACE) {
        LOGE("no complete EGL state - leaving rendering to GameThread only");
        return;
    }

    pthread_t thread{};
    if (pthread_create(&thread, nullptr, RenderLoop, nullptr) != 0) {
        LOGE("failed to start the consumer thread");
        g_ownFrameLoop = false;
        return;
    }
    pthread_detach(thread);
}

bool InterceptGameNative(JNIEnv* env) {
    if (g_classLoader == nullptr) {
        LOGE("no class loader - Application never reported one");
        return false;
    }

    // FindClass on an attached native thread searches the system class loader,
    // which knows nothing about the app's classes. Going through the loader the
    // Application handed us is the only reliable route from here.
    jclass loaderClass = env->GetObjectClass(g_classLoader);
    jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    if (loadClass == nullptr) {
        LOGE("ClassLoader.loadClass not found");
        return false;
    }

    jstring name = env->NewStringUTF("com.rockstargames.oswrapper.GameNative");
    auto gameNative = static_cast<jclass>(env->CallObjectMethod(g_classLoader, loadClass, name));
    env->DeleteLocalRef(name);
    if (env->ExceptionCheck() || gameNative == nullptr) {
        env->ExceptionClear();
        LOGE("could not load com.rockstargames.oswrapper.GameNative");
        return false;
    }

    const JNINativeMethod methods[] = {
        {"implOnDrawFrame",      "(F)V",                        reinterpret_cast<void*>(OnDrawFrame)},
        {"implOnSurfaceChanged", "(Landroid/view/Surface;II)V", reinterpret_cast<void*>(OnSurfaceChanged)},
        {"implOnSurfaceDestroyed", "()V",                       reinterpret_cast<void*>(OnSurfaceDestroyed)},
        {"implOnSurfaceCreated", "()V",                       reinterpret_cast<void*>(OnSurfaceCreated)},
        {"implOnPause",          "()V",                        reinterpret_cast<void*>(OnPause)},
        {"implOnResume",         "()V",                        reinterpret_cast<void*>(OnResume)},
    };

    if (env->RegisterNatives(gameNative, methods, std::size(methods)) != JNI_OK) {
        env->ExceptionClear();
        LOGE("RegisterNatives failed");
        return false;
    }

    g_gameNativeClass = static_cast<jclass>(env->NewGlobalRef(gameNative));
    LOGI("intercepted implOnDrawFrame and implOnSurfaceChanged");
    return true;
}

void* WaitForGameLibrary(void*) {
    LOGI("waiting for libGame.so (pid %d)", getpid());

    void* handle = nullptr;
    for (int waited = 0; waited < kPollTimeoutMs; waited += kPollIntervalMs) {
        handle = dlopen("libGame.so", RTLD_NOLOAD | RTLD_NOW);
        if (handle != nullptr) {
            LOGI("libGame.so present after %d ms", waited);
            break;
        }
        SleepMs(kPollIntervalMs);
    }

    if (handle == nullptr) {
        LOGE("libGame.so never appeared - is this the right APK?");
        return nullptr;
    }

    if (!ResolveGameSymbols(handle)) {
        return nullptr;
    }
    const std::uintptr_t drawOffset = g.LoadBase
        ? reinterpret_cast<std::uintptr_t>(g.implOnDrawFrame) - g.LoadBase : 0u;
    const bool knownRetail211 = drawOffset == 0x7d28d4u;
    const auto capWord = knownRetail211
        ? *reinterpret_cast<const std::uint32_t*>(g.LoadBase + 0x3683d0) : 0u;
    const auto gateWord = knownRetail211
        ? *reinterpret_cast<const std::uint32_t*>(g.LoadBase + 0x3686b0) : 0u;
    LOGI("[perf.init] libGame=%p draw=+0x%zx known211=%d "
         "limiter pending cap=0x%08x gate=0x%08x outer_fallback=90Hz",
         reinterpret_cast<void*>(g.LoadBase), static_cast<std::size_t>(drawOffset),
         knownRetail211 ? 1 : 0, capWord, gateWord);
    savr::cheats::Init(handle);   // resolve the VR cheat-menu handlers
    savr::appearance::Init();     // load the persisted hand-skin choice
    savr::hud::Init();            // load CLASSIC/IMMERSIVE HUD preference
    savr::calib::Init();          // load saved per-weapon calibration profiles
    savr::driving::Init();        // load DEFAULT vehicle seat offsets
    savr::locomotion::Init();     // load movement/turning comfort settings
    savr::holster::Init();        // load the persistent Vice City-style loadout
    savr::physicalweapon::Init(); // reference Quest build grab/drop/transfer/catch ownership

    // StartUserPause is only four instructions in the exact 2.11 binary. Verify
    // all of them before replacing it; OnStartUserPause reproduces its one-byte
    // store outside focused stereo gameplay, so no trampoline is required.
    if (g.CTimer_StartUserPause != nullptr) {
        constexpr std::uint32_t kStartPause211[4] = {
            0xf0001b88u, 0x52800029u, 0xf941ed08u, 0x39000109u
        };
        std::uint32_t observed[4]{};
        std::memcpy(observed, reinterpret_cast<void*>(g.CTimer_StartUserPause),
                    sizeof(observed));
        if (std::memcmp(observed, kStartPause211, sizeof(observed)) == 0 &&
            InstallInlineHook(reinterpret_cast<void*>(g.CTimer_StartUserPause),
                              reinterpret_cast<void*>(OnStartUserPause))) {
            LOGI("[pause.fix] focused-gameplay StartUserPause guard installed");
        } else {
            LOGE("[pause.fix] StartUserPause prologue mismatch; guard not installed");
        }
    }

    // Skip both Social Club gates before the game ever asks for them: the
    // boot-time sign-in (ShowInitial) and the per-game gate (ShowGate).
    if (g.OS_RockstarShowInitial != nullptr &&
        InstallInlineHook(reinterpret_cast<void*>(g.OS_RockstarShowInitial),
                          reinterpret_cast<void*>(OnRockstarShowInitial))) {
        LOGI("rockstar initial hook installed");
    }
    if (g.OS_RockstarShowGate != nullptr &&
        InstallInlineHook(reinterpret_cast<void*>(g.OS_RockstarShowGate),
                          reinterpret_cast<void*>(OnRockstarShowGate))) {
        LOGI("rockstar gate hook installed");
    }
    // Audio: the game opens its .osw paks with fopen(RELATIVE, "rb"), resolved
    // against the process cwd (which is "/"), so the paks are looked for at
    // /AUDIO/STREAMS/... and never found -> null ZIPFile -> silence + a crash in
    // GetDataStream (ZIPFile::Find this=0). (CONFIG loads only because it goes
    // through CFileMgr, which prepends the data dir.) The fix is to point cwd at
    // the game's data dir so the relative fopen lands on the deployed packs; the
    // case-insensitive /sdcard FUSE maps AUDIO/ -> audio/. Fixes SFX and radio.
    const char* kDataDir = "/sdcard/Android/data/com.rockstargames.gtasa/files";
    const bool audioPacksPresent =
        access("/sdcard/Android/data/com.rockstargames.gtasa/files/audio/CONFIG/BankLkup.dat", F_OK) == 0;
    if (audioPacksPresent && chdir(kDataDir) == 0) {
        // Packs will now open through the real loaders — do NOT stub them, or the
        // stub's return-null keeps radio silent.
        LOGI("audio: cwd -> data dir; real SFX + streams enabled (loaders not stubbed)");
    } else {
        // No packs (or chdir failed): keep the stubs so the null-ZIPFile path can't
        // crash the game; it just runs silent.
        LOGW("audio: packs %s, chdir %s -> stubbing stream loader (silent, crash-safe)",
             audioPacksPresent ? "present" : "absent", audioPacksPresent ? "failed" : "skipped");
        if (g.CAEMP3_GetDataStream != nullptr &&
            InstallInlineHook(reinterpret_cast<void*>(g.CAEMP3_GetDataStream),
                              reinterpret_cast<void*>(OnGetDataStream))) {
            LOGI("mp3 stream loader stubbed");
        }
        if (g.CAudioEngine_PreloadCutscene != nullptr &&
            InstallInlineHook(reinterpret_cast<void*>(g.CAudioEngine_PreloadCutscene),
                              reinterpret_cast<void*>(OnPreloadCutsceneTrack))) {
            LOGI("cutscene track preload stubbed");
        }
    }

    // Head-tracked stereo camera: hook the scene camera so gameplay is rendered
    // from the headset's pose. Menus/cutscenes stay on the flat theater screen.
    vrcam::Install();
    vrfire::SetPhysicalFireQuery(&QueryPhysicalFireHand);
    vrfire::Install();
    throwable::Install();
    savr::pickups::Install(handle);  // physical world-pickup rise/grab
    savr::braindiag::Install(handle);  // basketball brain-chain diagnostics
    savr::basketball::Install(handle);  // custom immersive basketball
    savr::mapzoom::Install(handle);  // pause-map zoom on the grips

    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("could not attach the resolver thread to the VM");
        return nullptr;
    }
    InterceptGameNative(env);
    g_vm->DetachCurrentThread();
    return nullptr;
}

} // namespace
} // namespace savr

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    savr::g_vm = vm;
    LOGI("libsavr.so loaded, JavaVM %p", static_cast<void*>(vm));
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_savr_SavrApplication_nativeOnApplicationCreate(JNIEnv* env, jclass, jobject classLoader) {
    savr::g_classLoader = env->NewGlobalRef(classLoader);
    LOGI("application created");
}

JNIEXPORT void JNICALL
Java_com_savr_SavrApplication_nativeOnActivityCreated(JNIEnv* env, jclass, jobject activity) {
    if (savr::g_activity != nullptr) {
        return;
    }
    savr::g_activity = env->NewGlobalRef(activity);
    LOGI("game activity created");

    // The loader needs the VM and the Activity, but no GL context, so the
    // instance can be built here and the session left until the render thread.
    savr::xr::Initialize(savr::g_vm, savr::g_activity);

    // Waiting starts here rather than at Application.onCreate. The process can
    // be brought up long before anyone launches the game — a background service
    // is enough — and libGame.so is only pulled in when this activity starts.
    // Waiting from process start therefore timed out against an idle process and
    // left the whole layer uninstalled.
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, savr::WaitForGameLibrary, nullptr) != 0) {
        LOGE("failed to start the resolver thread");
    } else {
        pthread_detach(thread);
    }
}

} // extern "C"
