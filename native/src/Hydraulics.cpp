#include "Hydraulics.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

#include "Log.h"
#include "Symbols.h"

// Lowrider hydraulics on the right stick (GTA:SA Android 2.11.311 arm64).
//
// WHY A HOOK AND NOT A PAD WRITE. CAutomobile::HydraulicControl (@0x6a1f8c) does
// read the stick exactly like the PC build — it calls
//   CPad::GetCarGunLeftRight(pad, true, true)              @0x49ce54
//   CPad::GetCarGunUpDown(pad, true, nullptr, 2500.0f, true) @0x49cb98
// and turns the pair into per-wheel suspension. BUT on this mobile port those
// two accessors only fall through to NewState.RightStickX/Y when
// `CHID::GetInputType() == 1`; otherwise they read the on-screen touch widgets
// (CTouchInterface::IsTouched, and there are CWidgetRegionHydraulics /
// CWidgetButtonHydraulics classes for exactly that). On the Quest the input type
// is not that value, so writing NewState.RightStick* was simply ignored — which
// is why the first attempt did nothing. Hooking the two accessors bypasses the
// whole input-type/touch-widget maze and is immune to it.
//
// SIGNS (worked through against the decomp formula in
// CAutomobile::HydraulicControl: angle = atan2(leftRight, upDown) - 45deg, then
// the four corners come from +/-cos/sin):
//   upDown  < 0 -> FRONT lifts,  upDown  > 0 -> REAR lifts
//   leftRight < 0 -> LEFT lifts, leftRight > 0 -> RIGHT lifts
// The VR stick convention is "up/right positive", so we return
//   leftRight = +x * 127   and   upDown = -y * 127
// giving the intuitive mapping the player asked for.
//
// NOTE the mobile accessors are NOT the PC ones: GetCarGunUpDown takes
// (bool, CAutomobile*, float, bool) and internally returns NEGATED RightStickY,
// while GetCarGunLeftRight takes (bool, bool). We return the final value
// ourselves, so those internal differences do not matter.
namespace savr::hydraulics {
namespace {

std::atomic<bool>  g_active{false};
std::atomic<float> g_stickX{0.0f};
std::atomic<float> g_stickY{0.0f};

// Rhythm minigame beat table. CAEAudioHardware keeps the live tBeatInfo at
// AEAudioHardware+0xCE8: tBeat BeatWindow[20] {u32 timeMs; u32 key;} then
// IsBeatInfoPresent at +0xA0. BeatWindow[10] is the current beat, [10+i] is i
// beats ahead. We READ it directly and never call CAudioEngine::GetBeatInfo -
// that call mutates BeatNumber/BeatTypeThisFrame and would consume the beat
// edge the game itself is waiting on (the same class of bug as the per-eye
// consumption issues elsewhere in this project).
const std::uint8_t* g_beatInfo = nullptr;
constexpr int kBeatWindowMid   = 0x50;   // &BeatWindow[10]
constexpr int kBeatPresent     = 0xA0;
constexpr unsigned kBeatMaxMs  = 4500u;  // the script ignores beats beyond this
constexpr int kBeatIndexOff    = 0xA8;   // tBeatInfo::BeatNumber (the graded beat id)
// LOWGAME grades a beat as PERFECT only within 126 ms of it; anything later in
// the beat period is "Early" (the countdown never goes negative), which is what
// made every beat BAD while the stick was simply held.
constexpr int kHitWindowMs     = 126;

// Latched flick: a beat is answered by ONE fresh neutral->deflected flick, and
// that answer is only handed to the game inside the hit window.
int  g_pendingDir   = 0;      // 0 none, 1 UP, 2 DOWN, 3 LEFT, 4 RIGHT
int  g_lastBeatIdx  = -1;
bool g_stickNeutral = true;

// Snapshot of BeatWindow[10] taken on the GAME thread. CAEAudioHardware::
// GetBeatInfo zeroes the whole table before refilling it, so reading it from
// another thread can catch it blank.
std::atomic<int> g_curBeatKey{0};
std::atomic<int> g_curBeatMs{0};

// LOWGAME script globals, decoded from mainV1.scm. CTheScripts::ScriptSpace is
// already resolved in Symbols; globals live at ScriptSpace + index*4.
//   $1025 running flag  +0x1004
//   $1026 player score   +0x1008
//   $1027 opponent score +0x100C
constexpr int kOffLowGameRunning = 0x1004;
constexpr int kOffPlayerScore    = 0x1008;
constexpr int kOffOpponentScore  = 0x100C;

// True only while the LOWGAME dance minigame is actually running ($1025).
// CRITICAL: do NOT use "a beat track is playing" for this - the RADIO carries
// beat info too, so that gate silently killed ordinary hydraulics whenever
// music was on.
bool DanceActive() {
    if (g.CTheScripts_ScriptSpace == nullptr) return false;
    const auto* space = static_cast<const std::uint8_t*>(g.CTheScripts_ScriptSpace);
    const std::int32_t running =
        *reinterpret_cast<const std::int32_t*>(space + kOffLowGameRunning);
    static std::int32_t logged = -12345;
    if (running != logged) {
        logged = running;
        LOGI("[hydraulics] LOWGAME $1025=%d -> dance=%s (space=%p)", running,
             running != 0 ? "ACTIVE (live stick suppressed)" : "off",
             g.CTheScripts_ScriptSpace);
    }
    return running != 0;
}

int SnapDirection(float x, float y) {
    if (std::fabs(x) >= std::fabs(y)) return x > 0.f ? 4 : 3;   // RIGHT / LEFT
    return y > 0.f ? 1 : 2;                                      // UP / DOWN
}
bool CardinalKey(int key) {
    return key == 9 || key == 10 || key == 13 || key == 14 ||
           key == 1 || key == 2  || key == 3  || key == 4;
}

std::int16_t Axis(float v) {
    return static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 127.0f);
}

// NOTE: the CPad car-gun accessors are hooked ONCE, in Driving.cpp (it needs
// them for the Hydra nozzles and the tank gun). Installing a second trampoline
// over the first is impossible - it observed our own LDR X17/BR X17 pattern as
// the "prologue" and refused - so Driving's handlers call into us instead.

}  // namespace

void Install(void* handle) {
    if (handle) {
        const auto* hw = reinterpret_cast<const std::uint8_t*>(
            dlsym(handle, "AEAudioHardware"));
        if (hw) g_beatInfo = hw + 0xCE8;
    }
    LOGI("[hydraulics] ready (beatInfo=%p; CPad car-gun hooks owned by Driving)",
         static_cast<const void*>(g_beatInfo));
}

bool GetDanceScore(int* playerOut, int* opponentOut) {
    if (g.CTheScripts_ScriptSpace == nullptr) return false;
    const auto* space = static_cast<const std::uint8_t*>(g.CTheScripts_ScriptSpace);
    const std::int32_t running =
        *reinterpret_cast<const std::int32_t*>(space + kOffLowGameRunning);
    if (running == 0) return false;          // minigame not active
    const std::int32_t player =
        *reinterpret_cast<const std::int32_t*>(space + kOffPlayerScore);
    const std::int32_t rival =
        *reinterpret_cast<const std::int32_t*>(space + kOffOpponentScore);
    // Defensive: the script caps at 999999; anything wild means we are reading
    // a slot the minigame does not own this session.
    if (player < 0 || player > 999999 || rival < 0 || rival > 999999)
        return false;
    if (playerOut) *playerOut = player;
    if (opponentOut) *opponentOut = rival;
    return true;
}

int PollBeat(int* msUntilOut) {
    if (msUntilOut) *msUntilOut = 0;
    if (!DanceActive()) return 0;   // never prompt over ordinary radio music
    // BeatWindow[10] ONLY: that is the beat LOWGAME is grading right now.
    // Scanning ahead showed a beat several periods away, so its countdown told
    // the player to press far too early. (Values are snapshotted on the game
    // thread; the audio code blanks the table while refilling it.)
    const int key = g_curBeatKey.load(std::memory_order_relaxed);
    const int ms  = g_curBeatMs.load(std::memory_order_relaxed);
    if (key <= 0 || ms < 0 || ms >= static_cast<int>(kBeatMaxMs)) return 0;
    int dir = 0;
    switch (key) {              // decoded from R*'s own rhythm widget
        case 13: case 3: dir = 1; break;   // UP
        case 14: case 1: dir = 2; break;   // DOWN
        case  9: case 2: dir = 3; break;   // LEFT
        case 10: case 4: dir = 4; break;   // RIGHT
        default: return 0;                 // diagonals are never graded
    }
    if (msUntilOut) *msUntilOut = ms;
    return dir;
}

void SetStick(bool active, float x, float y) {
    g_stickX.store(active ? x : 0.0f, std::memory_order_relaxed);
    g_stickY.store(active ? y : 0.0f, std::memory_order_relaxed);

    // --- beat snapshot (game thread) -------------------------------------
    int beatKey = 0, beatMs = 0, beatIdx = -1;
    if (g_beatInfo != nullptr &&
        *reinterpret_cast<const std::int32_t*>(g_beatInfo + kBeatPresent) != 0) {
        const std::uint8_t* cur = g_beatInfo + kBeatWindowMid;   // BeatWindow[10]
        beatMs  = static_cast<int>(*reinterpret_cast<const std::uint32_t*>(cur));
        beatKey = *reinterpret_cast<const std::int32_t*>(cur + 4);
        beatIdx = *reinterpret_cast<const std::int32_t*>(g_beatInfo + kBeatIndexOff);
    }
    g_curBeatKey.store(beatKey, std::memory_order_relaxed);
    g_curBeatMs.store(beatMs, std::memory_order_relaxed);

    // --- one fresh flick answers one beat --------------------------------
    if (beatIdx != g_lastBeatIdx) {   // beat advanced: a new answer is needed
        g_lastBeatIdx = beatIdx;
        g_pendingDir = 0;
    }
    const float mag = std::sqrt(x * x + y * y);
    if (!active || mag < 0.35f) {
        g_stickNeutral = true;
    } else if (g_stickNeutral && mag >= 0.50f) {
        g_stickNeutral = false;                 // neutral -> deflected EDGE
        g_pendingDir = SnapDirection(x, y);     // latch what the player asked for
    }

    const bool was = g_active.exchange(active, std::memory_order_acq_rel);
    if (was != active) {
        LOGI("[hydraulics] %s", active ? "ENGAGED" : "released");
        if (!active) { g_pendingDir = 0; g_stickNeutral = true; }
    }
}

// The value handed to the game for THIS frame: a snapped cardinal, and only
// inside the hit window of a cardinal beat. Returning the raw held stick made
// the script grade every beat "Early" (grade 4) -> BAD, and an off-axis push
// decoded as a diagonal (11/12/15/16) -> "wrong direction" (grade 5).
int ScoringDirection() {
    if (!g_active.load(std::memory_order_acquire)) return 0;
    if (!DanceActive()) return 0;
    if (g_pendingDir == 0) return 0;
    const int key = g_curBeatKey.load(std::memory_order_relaxed);
    const int ms  = g_curBeatMs.load(std::memory_order_relaxed);
    if (!CardinalKey(key)) return 0;
    if (ms < 0 || ms >= kHitWindowMs) return 0;
    return g_pendingDir;
}

bool IsActive() { return g_active.load(std::memory_order_acquire); }

// Two consumers, two needs:
//  - CAutomobile::HydraulicControl wants the LIVE stick so the car bounces while
//    you hold it (that already worked and must keep working);
//  - LOWGAME's scoring wants a SNAPPED cardinal delivered only on the beat.
// The dance minigame takes priority whenever a beat track is playing, because a
// held raw stick is exactly what the script mis-grades.
int AxisLeftRight() {
    if (DanceActive()) {
        const int dir = ScoringDirection();
        return dir == 3 ? -127 : (dir == 4 ? 127 : 0);
    }
    return Axis(g_stickX.load(std::memory_order_relaxed));
}
// Negated: the VR stick is up-positive, and upDown<0 is what lifts the FRONT
// (and is also what the script decodes as UP against its (0,-1) reference).
int AxisUpDown() {
    if (DanceActive()) {
        const int dir = ScoringDirection();
        return dir == 1 ? -127 : (dir == 2 ? 127 : 0);
    }
    return Axis(-g_stickY.load(std::memory_order_relaxed));
}

}  // namespace savr::hydraulics
