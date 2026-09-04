#include "Jetpack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include "Log.h"
#include "Symbols.h"

// CTaskSimpleJetPack control model (gta-reversed decomp + arm64 disasm of
// 2.11.311). ProcessControlInput @0x680c1c reads the flat pad and sets the
// task's thrust fields; ProcessThrust turns them into force. Pad->control
// (Mode 0): climb=GetAccelerate=ButtonCross(ns16); descend=GetBrake=
// ButtonSquare(ns14); fwd/back tilt=GetPedWalkUpDown=LeftStickY(ns1, forward
// is NEGATIVE); turn=GetPedWalkLeftRight=LeftStickX(ns0). Feeding these flies
// the belt with no task hook. Detection: CPedIntelligence::GetTaskJetPack()
// (@0x5a9d44); m_pIntelligence @ped+0x538. Drop: CTaskSimpleJetPack::
// DropJetPack (@0x681990) removes the belt and spawns a ground pickup.
namespace savr::jetpack {
namespace {

const char* const kIniPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_jetpack.ini";

std::atomic<Mode> g_mode{Mode::Reliable};   // rock-solid default (SIMPLE)
std::atomic<bool> g_lockInAir{true};        // block B-drop while airborne (default ON)
std::atomic<int>  g_turbinePower{100};      // vectored-thrust scale, percent
std::atomic<int>  g_fogDistance{100};       // in-flight fog push-out, percent
std::mutex        g_saveMutex;
// True once the ini has been read successfully (or confirmed absent). Save()
// is REFUSED before that so start-up defaults can never overwrite the file —
// which is exactly how the user's saved 110% got clobbered back to 100.
std::atomic<bool> g_loaded{false};
int               g_loadAttempts = 0;
long long         g_lastLoadAttemptMs = 0;

using GetTaskJetPackFn = void* (*)(void* pedIntelligence);
using DropJetPackFn     = void  (*)(void* task, void* ped);
GetTaskJetPackFn g_getTaskJetPack = nullptr;
DropJetPackFn    g_dropJetPack    = nullptr;   // hooked entry (goes via OnDropJetPack)
DropJetPackFn    g_origDropJetPack = nullptr;  // trampoline to the real DropJetPack

constexpr int kOffIntelligence = 0x538;   // CPed::m_pIntelligence
constexpr int kOffMoveSpeed    = 0x68;    // CPhysical::m_vecMoveSpeed (arm64 2.11.311)
constexpr std::uintptr_t kDropJetPackRVA = 0x681990u;   // CTaskSimpleJetPack::DropJetPack
// Expected DropJetPack prologue (must match before we patch; all 4 relocatable):
//   sub sp,sp,#0x50 ; stp x29,x30,[sp,#0x20] ; str x21,[sp,#0x30] ; stp x20,x19,[sp,#0x40]
constexpr std::uint32_t kDropPrologue[4] = {
    0xd10143ffu, 0xa9027bfdu, 0xf9001bf5u, 0xa9044ff4u};

// NewState (CControllerState) int16 field indices, matching OnUpdatePads.
constexpr int NS_LEFT_STICK_X = 0;   // GetPedWalkLeftRight -> turn
constexpr int NS_LEFT_STICK_Y = 1;   // GetPedWalkUpDown    -> fwd/back tilt
constexpr int NS_SQUARE       = 14;  // GetBrake            -> descend
constexpr int NS_CROSS        = 16;  // GetAccelerate       -> climb

constexpr float kTriggerOn   = 0.5f;   // reliable-mode throttle threshold
constexpr float kStickDead   = 0.15f;
constexpr float kFireOn      = 0.12f;  // realistic: any thruster considered lit
                                       // (matches VrCamera::ApplyJetpackThrust)
// A drop is never allowed while any VR menu page is open, nor for this long
// after one closes: closing/navigating a menu must never strip the belt.
constexpr long kMenuDropCooldownMs = 300;

bool g_prevExit = false;
bool g_bLatched = false;   // B is "claimed" by a menu; ignore for drop until released
std::atomic<bool> g_anyMenuOpen{false};
std::atomic<long long> g_menuLastOpenMs{0};   // steady_clock ms, last frame a menu was open
std::atomic<bool> g_intentionalDrop{false};    // our own B-drop passing through the hook

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// True while a menu is open or closed less than kMenuDropCooldownMs ago.
bool MenuDropGuard() {
    if (g_anyMenuOpen.load(std::memory_order_relaxed)) return true;
    return NowMs() - g_menuLastOpenMs.load(std::memory_order_relaxed)
           < kMenuDropCooldownMs;
}

// The player is "airborne" (jetpack lifted off) while the ped has real move
// speed. Grounded-idle with the belt settles to ~0 (task hover < gravity, ground
// clamps it). A short debounce keeps a hover apex from reading as landed.
bool PlayerAirborne(void* ped) {
    if (ped == nullptr) return false;
    const float* v = reinterpret_cast<const float*>(
        static_cast<std::uint8_t*>(ped) + kOffMoveSpeed);
    const float speedSq = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    static long long lastMoving = 0;
    const long long now = NowMs();
    if (speedSq > 0.0009f) lastMoving = now;   // ~0.03 units/frame
    return now - lastMoving < 400;              // 0.4 s land debounce
}

// Returns the active jetpack task (or null) and, when non-null, the player ped.
void* ActiveTaskAndPed(void** pedOut) {
    if (pedOut) *pedOut = nullptr;
    if (g_getTaskJetPack == nullptr || g.FindPlayerPed == nullptr) return nullptr;
    void* ped = g.FindPlayerPed(-1);
    if (ped == nullptr) return nullptr;
    void* intel = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(ped) + kOffIntelligence);
    if (intel == nullptr) return nullptr;
    void* task = g_getTaskJetPack(intel);
    if (task != nullptr && pedOut) *pedOut = ped;
    return task;
}

// ---- persistence ----------------------------------------------------------
void Save() {
    if (!g_loaded.load(std::memory_order_acquire)) {
        LOGW("[jetpack] Save() before a successful Load() - refused");
        return;
    }
    std::lock_guard<std::mutex> lock(g_saveMutex);
    // Temp file + rename, never reopen the target with "w": a settings file
    // deployed with `adb push` is owned by `shell` and mode 0644, so the app
    // cannot write it and every change is silently lost. We own the directory,
    // so rename() succeeds and also replaces it with an app-owned file.
    char tmpPath[256];
    std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", kIniPath);
    FILE* f = std::fopen(tmpPath, "w");
    if (!f) { LOGW("[jetpack] could not save %s", tmpPath); return; }
    std::fprintf(f, "Mode=%d\n", static_cast<int>(g_mode.load()));
    std::fprintf(f, "LockInAir=%d\n", g_lockInAir.load() ? 1 : 0);
    std::fprintf(f, "TurbinePower=%d\n", g_turbinePower.load());
    std::fprintf(f, "FogDistance=%d\n", g_fogDistance.load());
    const bool wrote = std::fflush(f) == 0;
    std::fclose(f);
    if (!wrote) { std::remove(tmpPath); return; }
    if (std::rename(tmpPath, kIniPath) != 0) {
        std::remove(kIniPath);
        if (std::rename(tmpPath, kIniPath) != 0) {
            LOGW("[jetpack] could not replace %s", kIniPath);
            std::remove(tmpPath);
        }
    }
}

void Load() {
    if (g_loaded.load(std::memory_order_acquire)) return;
    // The menu draw calls this from the PRESENT thread while NoteMenuInput calls
    // it from the GAME thread. Serialise with the same lock Save() uses so a read
    // can never straddle Save()'s truncate, and rate-limit the retry so an
    // unreadable file cannot fopen() once per frame forever. (The early return
    // above runs before the lock, so the LOGI below cannot re-enter it.)
    std::lock_guard<std::mutex> guard(g_saveMutex);   // 'lock' is taken below
    if (g_loaded.load(std::memory_order_acquire)) return;
    const long long nowMs = NowMs();
    if (g_loadAttempts > 0 && nowMs - g_lastLoadAttemptMs < 1000) return;
    g_lastLoadAttemptMs = nowMs;
    ++g_loadAttempts;
    int mode = static_cast<int>(Mode::Reliable), lock = 1, power = 100, fog = 100;
    FILE* f = std::fopen(kIniPath, "r");
    if (f == nullptr) {
        const int err = errno;
        if (err == ENOENT) {
            // No file yet: defaults are the truth; allow Save() from now on.
            g_loaded.store(true, std::memory_order_release);
            LOGI("[jetpack] no %s yet - using defaults", kIniPath);
        } else if (g_loadAttempts <= 3 || (g_loadAttempts % 60) == 0) {
            // Transient (storage not ready?) - keep the defaults for now but
            // do NOT mark loaded, so Save() stays refused and we retry lazily.
            LOGW("[jetpack] Load() fopen failed errno=%d (%s), attempt %d - will retry",
                 err, std::strerror(err), g_loadAttempts);
        }
        return;
    }
    char line[96];
    while (std::fgets(line, sizeof(line), f)) {
        int v = 0;
        if      (std::sscanf(line, "Mode=%d", &v) == 1)         mode = v;
        else if (std::sscanf(line, "LockInAir=%d", &v) == 1)    lock = v;
        else if (std::sscanf(line, "TurbinePower=%d", &v) == 1) power = v;
        else if (std::sscanf(line, "FogDistance=%d", &v) == 1)  fog = v;
    }
    std::fclose(f);
    // Store the VALUES first and publish g_loaded only after. g_loaded is what
    // unblocks Save(); publishing it first let the other thread save stale
    // defaults over the file we had just read - exactly the clobber that reset
    // the user's saved settings on every launch.
    g_mode.store(mode == static_cast<int>(Mode::Realistic) ? Mode::Realistic
                                                            : Mode::Reliable);
    g_lockInAir.store(lock != 0);
    g_turbinePower.store(std::clamp(power, 50, 200));
    g_fogDistance.store(std::clamp(fog, 100, 300));
    g_loaded.store(true, std::memory_order_release);
    // Logged last: ModeName() -> GetMode() -> Load() must find g_loaded already
    // true so it returns before re-entering this non-recursive lock.
    LOGI("[jetpack] settings loaded: mode=%s lockInAir=%d power=%d%% fog=%d%%",
         ModeName(), g_lockInAir.load() ? 1 : 0, g_turbinePower.load(),
         g_fogDistance.load());
}

// ---- DropJetPack chokepoint hook ------------------------------------------
// EVERY belt drop (the task's own Triangle/exit-vehicle path, our B, anything)
// funnels through CTaskSimpleJetPack::DropJetPack. Guarding it here is what
// finally makes "no drop around menus" airtight regardless of which input
// path fired it.
void OnDropJetPack(void* task, void* ped) {
    const bool intentional = g_intentionalDrop.load(std::memory_order_acquire);
    if (!intentional && MenuDropGuard()) {
        LOGI("[jetpack] DropJetPack BLOCKED (menu open/just closed)");
        return;
    }
    if (g_origDropJetPack) g_origDropJetPack(task, ped);
}

bool InstallDropHook(void* target) {
    if (g_origDropJetPack != nullptr) return true;
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    std::uint32_t observed[4]{};
    std::memcpy(observed, code, sizeof(observed));
    if (std::memcmp(observed, kDropPrologue, sizeof(observed)) != 0) {
        LOGE("[jetpack] DropJetPack prologue mismatch: %08x %08x %08x %08x",
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
    const std::uintptr_t start =
        reinterpret_cast<std::uintptr_t>(code) & ~(pageSize - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(code) + 16 + pageSize - 1) &
        ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(tramp, pageSize);
        return false;
    }
    // Publish the trampoline BEFORE redirecting the target, otherwise a call
    // landing in that window reaches OnDropJetPack with a null original and is
    // silently swallowed.
    g_origDropJetPack = reinterpret_cast<DropJetPackFn>(tramp);
    code[0] = 0x58000051u;   // LDR X17, #8 (replacement literal)
    code[1] = 0xD61F0220u;   // BR X17
    *reinterpret_cast<void**>(code + 2) = reinterpret_cast<void*>(&OnDropJetPack);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    return true;
}

}  // namespace

void Init(void* handle) {
    if (handle == nullptr) return;
    g_getTaskJetPack = reinterpret_cast<GetTaskJetPackFn>(
        dlsym(handle, "_ZNK16CPedIntelligence14GetTaskJetPackEv"));
    g_dropJetPack = reinterpret_cast<DropJetPackFn>(
        dlsym(handle, "_ZN18CTaskSimpleJetPack11DropJetPackEP4CPed"));
    // Hook the drop chokepoint at its resolved address (== base+RVA).
    void* dropTarget = g_dropJetPack
        ? reinterpret_cast<void*>(g_dropJetPack)
        : (g.LoadBase ? reinterpret_cast<void*>(g.LoadBase + kDropJetPackRVA)
                      : nullptr);
    const bool hooked = dropTarget && InstallDropHook(dropTarget);
    if (dropTarget && !g_dropJetPack)
        g_dropJetPack = reinterpret_cast<DropJetPackFn>(dropTarget);
    Load();
    LOGI("[jetpack] GetTaskJetPack=%p DropJetPack=%p dropHook=%s",
         reinterpret_cast<void*>(g_getTaskJetPack),
         reinterpret_cast<void*>(g_dropJetPack), hooked ? "OK" : "FAILED");
}

void NoteMenuInput(bool anyMenuOpen, bool bDown) {
    Load();   // lazy retry until the ini is readable
    // Runs from the menu loop EVERY frame (even while WritePad is skipped because
    // game input is blocked). Records menu-open state + time for the drop
    // guard, and arms the B latch while any page is open; disarms only when B
    // is fully released.
    g_anyMenuOpen.store(anyMenuOpen, std::memory_order_relaxed);
    if (anyMenuOpen) {
        g_menuLastOpenMs.store(NowMs(), std::memory_order_relaxed);
        g_bLatched = true;
    }
    if (!bDown) g_bLatched = false;
}

bool IsActive() { return ActiveTaskAndPed(nullptr) != nullptr; }

void* ActiveTask() { return ActiveTaskAndPed(nullptr); }

Mode GetMode() { Load(); return g_mode.load(std::memory_order_acquire); }

void CycleMode() {
    const Mode next = GetMode() == Mode::Realistic ? Mode::Reliable
                                                   : Mode::Realistic;
    g_mode.store(next, std::memory_order_release);
    LOGI("[jetpack] control mode -> %s", ModeName());
    Save();
}

const char* ModeName() {
    // Display names: turbines = EXPERIMENTAL, stick control = SIMPLE.
    return GetMode() == Mode::Realistic ? "EXPERIMENTAL" : "SIMPLE";
}

bool IsVectoredMode() { return GetMode() == Mode::Realistic; }

bool LockInAir() { Load(); return g_lockInAir.load(std::memory_order_acquire); }
void SetLockInAir(bool on) {
    g_lockInAir.store(on, std::memory_order_release);
    Save();
}
void ToggleLockInAir() { SetLockInAir(!LockInAir()); }

int TurbinePowerPercent() { Load(); return g_turbinePower.load(std::memory_order_acquire); }
float TurbinePower() { return static_cast<float>(TurbinePowerPercent()) / 100.f; }
void AdjustTurbinePower(int step) {
    const int p = std::clamp(TurbinePowerPercent() + step * 10, 50, 200);
    g_turbinePower.store(p, std::memory_order_release);
    LOGI("[jetpack] turbine power -> %d%%", p);
    Save();
}

int FogDistancePercent() {
    Load();
    return g_fogDistance.load(std::memory_order_acquire);
}
float FogDistanceScale() {
    return static_cast<float>(FogDistancePercent()) / 100.f;
}
void AdjustFogDistance(int step) {
    const int p = std::clamp(FogDistancePercent() + step * 10, 100, 300);
    g_fogDistance.store(p, std::memory_order_release);
    LOGI("[jetpack] fog distance -> %d%%", p);
    Save();
}

void WritePad(std::int16_t* ns, const xr::InputState& in, float headYaw) {
    (void)headYaw;
    void* ped = nullptr;
    void* task = ActiveTaskAndPed(&ped);
    if (task == nullptr) return;

    const bool bothGrips = in.grip[0] >= 0.75f && in.grip[1] >= 0.75f;

    // EXIT: tap B to take the belt off (ground pickup + fists). Guards:
    //  - not during the two-grip menu chord;
    //  - not while any menu is open nor within the cooldown after it closes
    //    (MenuDropGuard), and B must be fully RELEASED once after menu use
    //    (g_bLatched, armed from the menu loop);
    //  - not while airborne when LOCK IN AIR is on (drop only when standing).
    // The DropJetPack hook enforces the menu guard for EVERY drop path too.
    if (!in.b) g_bLatched = false;
    const bool blockAir = LockInAir() && PlayerAirborne(ped);
    const bool exitNow  = in.b && !bothGrips && !MenuDropGuard() &&
                          !g_bLatched && !blockAir;
    if (exitNow && !g_prevExit && g_dropJetPack != nullptr && ped != nullptr) {
        g_intentionalDrop.store(true, std::memory_order_release);
        g_dropJetPack(task, ped);
        g_intentionalDrop.store(false, std::memory_order_release);
        LOGI("[jetpack] dropped via B");
    }
    g_prevExit = exitNow;

    // Left stick Y ONLY = forward/back cruise (ground/air mobility). Left X is
    // deliberately NOT fed: the stock task turns the ped with it, and turning
    // belongs to the RIGHT stick (the VR snap/smooth turn) — never fight it.
    float my = in.leftStick[1];
    if (std::fabs(my) < kStickDead) my = 0.f;
    ns[NS_LEFT_STICK_X] = 0;
    ns[NS_LEFT_STICK_Y] = static_cast<std::int16_t>(std::clamp(my,-1.f,1.f)*-127.f);

    if (GetMode() == Mode::Reliable) {
        // SIMPLE: right trigger climbs, left descends.
        if (in.triggers[1] > kTriggerOn)      ns[NS_CROSS]  = 255;
        else if (in.triggers[0] > kTriggerOn) ns[NS_SQUARE] = 255;
    } else {
        // EXPERIMENTAL: the true 3D vectored thrust is applied as real force in
        // VrCamera::ApplyJetpackThrust from the two nozzles. The stock task only
        // lets the ped leave the ground when it reads "accelerate" (m_ThrustFwd>0
        // clears bIsStanding), so while either thruster fires feed Cross to
        // unstick the ped (its gentle hover baseline is then dominated by ours).
        const bool firing = in.triggers[0] > kFireOn || in.triggers[1] > kFireOn;
        if (firing) ns[NS_CROSS] = 255;
    }
}

}  // namespace savr::jetpack
