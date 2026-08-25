#include "GestureMove.h"

#include "Locomotion.h"
#include "Log.h"
#include "Symbols.h"
#include "Xr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace savr::gesturemove {
namespace {

// Disasm-verified arm64 offsets, shared with Melee.cpp: entity matrix pointer
// at +0x18 (forward row at matrix+0x10), CPhysical mass at +0xB0.
constexpr int kEntityMatrixOffset = 0x18;
constexpr int kMatrixForwardOffset = 0x10;
constexpr int kPhysicalMassOffset = 0xB0;

struct V3 {
    float x{}, y{}, z{};
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float Length(V3 v) {
    return std::sqrt(std::max(0.0f, v.x * v.x + v.y * v.y + v.z * v.z));
}
bool Finite(V3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
               Clock::now().time_since_epoch()).count();
}

// Per-hand motion tracker in OpenXR LOCAL space (y is up). LOCAL space is
// stage-fixed, so the ped moving through the world can never feed back into
// the gesture the way world-space hands would.
struct HandTrack {
    bool valid{};
    V3 previousPos{};
    double previousAt{};
    V3 velocity{};   // low-passed, m/s
};

HandTrack gTracks[2]{};
float gRunEnergy = 0.0f;
float gPaddleEnergy = 0.0f;
double gSurgeUntil = 0.0;
double gSurgeCooldownUntil = 0.0;
bool gRunActive = false;   // hysteresis latch for the run state

void ResetAll() {
    gTracks[0] = HandTrack{};
    gTracks[1] = HandTrack{};
    gRunEnergy = 0.0f;
    gPaddleEnergy = 0.0f;
    gSurgeUntil = 0.0;
    gSurgeCooldownUntil = 0.0;
    gRunActive = false;
}

// Advance one hand's velocity estimate. Returns false (and re-baselines)
// on tracking loss, stale samples or recenter-style jumps.
bool Track(int hand, const xr::HandPose& pose, double now, float dt) {
    HandTrack& track = gTracks[hand];
    if (!pose.valid) {
        track = HandTrack{};
        return false;
    }
    const V3 pos{pose.gripPos[0], pose.gripPos[1], pose.gripPos[2]};
    if (!Finite(pos)) {
        track = HandTrack{};
        return false;
    }
    const bool fresh = track.valid && dt > 0.0f && dt <= 0.1f &&
                       Length(pos - track.previousPos) <= 0.5f;
    if (fresh) {
        const V3 raw = (pos - track.previousPos) * (1.0f / dt);
        // ~half-life of one 60 Hz frame: raw hand samples are jittery.
        track.velocity.x += (raw.x - track.velocity.x) * 0.5f;
        track.velocity.y += (raw.y - track.velocity.y) * 0.5f;
        track.velocity.z += (raw.z - track.velocity.z) * 0.5f;
    } else {
        track.velocity = V3{};
    }
    track.valid = true;
    track.previousPos = pos;
    track.previousAt = now;
    return fresh;
}

// One physical stroke's forward shove through the native physics. The swim
// task's own sprint handles sustained speed; this is the "thrown forward"
// impulse at the moment of a big two-hand pull.
void ApplySwimLunge(void* ped, float strokeSpeed) {
    if (!ped || !g.CPhysical_ApplyMoveForce) return;
    const auto* bytes = static_cast<const std::uint8_t*>(ped);
    void* matrix = nullptr;
    std::memcpy(&matrix, bytes + kEntityMatrixOffset, sizeof(matrix));
    if (!matrix) return;
    V3 forward{};
    std::memcpy(&forward,
                static_cast<const std::uint8_t*>(matrix) +
                    kMatrixForwardOffset,
                sizeof(forward));
    const float length = Length(forward);
    if (!std::isfinite(length) || length < 0.5f) return;
    forward = forward * (1.0f / length);
    float mass = 70.0f;
    std::memcpy(&mass, bytes + kPhysicalMassOffset, sizeof(mass));
    if (!std::isfinite(mass) || mass < 1.0f || mass > 1000.0f) mass = 70.0f;
    // moveSpeed gain of 0.030-0.055 game units/frame per stroke: a clear
    // lunge on top of SA's ~0.05-0.1 swim speeds without launching CJ.
    const float gain = std::clamp(0.018f * strokeSpeed, 0.030f, 0.055f);
    const V3 force = forward * (mass * gain);
    g.CPhysical_ApplyMoveForce(ped, force.x, force.y, force.z);
    LOGI("[vr.gesture] swim lunge speed=%.2f gain=%.3f", strokeSpeed, gain);
}

} // namespace

void Update(void* ped, bool swimming, bool blocked, Result& out) {
    out = Result{};
    if (blocked) {
        ResetAll();
        return;
    }
    const bool runEnabled = locomotion::GestureRunEnabled();
    const bool swimEnabled = locomotion::GestureSwimEnabled();
    if (!runEnabled && !swimEnabled) {
        ResetAll();
        return;
    }

    xr::HandPose poses[2]{};
    xr::GetHandPoses(poses);

    const double now = NowSeconds();
    // dt from the shared previous sample time (both hands are sampled
    // together every pad update).
    const double previousAt =
        std::max(gTracks[0].previousAt, gTracks[1].previousAt);
    const float dt = previousAt > 0.0
        ? static_cast<float>(now - previousAt) : 0.0f;

    const bool fresh0 = Track(0, poses[0], now, dt);
    const bool fresh1 = Track(1, poses[1], now, dt);
    const bool bothFresh = fresh0 && fresh1 && dt > 0.0f;

    if (swimming) {
        gRunEnergy = 0.0f;
        gRunActive = false;
        if (!swimEnabled) return;

        const float speed0 = Length(gTracks[0].velocity);
        const float speed1 = Length(gTracks[1].velocity);
        if (bothFresh) {
            // Dog paddle: any sustained two-hand motion keeps a slow crawl.
            const float average = 0.5f * (speed0 + speed1);
            if (average > 0.35f) {
                gPaddleEnergy += (average - 0.35f) * dt * 2.5f;
            }
            gPaddleEnergy = std::clamp(gPaddleEnergy - dt * 1.5f, 0.0f, 1.2f);

            // Big stroke: both hands fast at once -> surge + physical lunge.
            const float strokeSpeed = std::min(speed0, speed1);
            if (strokeSpeed >= 1.30f && now >= gSurgeCooldownUntil) {
                gSurgeUntil = now + 0.80;
                gSurgeCooldownUntil = now + 1.20;
                ApplySwimLunge(ped, strokeSpeed);
            }
        } else {
            gPaddleEnergy = std::max(0.0f, gPaddleEnergy - dt * 1.5f);
        }

        if (now < gSurgeUntil) {
            out.forward = 1.0f;
            out.sprint = true;   // Cross = the swim task's fast stroke
        } else if (gPaddleEnergy > 0.25f) {
            out.forward = 0.65f; // plain forward = the slow breaststroke
        }
        return;
    }

    // --- on land: arm-swing running -------------------------------------
    gPaddleEnergy = 0.0f;
    gSurgeUntil = 0.0;
    if (!runEnabled) return;

    // A hand with its trigger squeezed is punching or firing, never pumping.
    const bool combat0 = poses[0].trigger >= 0.40f;
    const bool combat1 = poses[1].trigger >= 0.40f;
    if (bothFresh && !combat0 && !combat1) {
        const float vertical0 = std::abs(gTracks[0].velocity.y);
        const float vertical1 = std::abs(gTracks[1].velocity.y);
        const float speed0 = Length(gTracks[0].velocity);
        const float speed1 = Length(gTracks[1].velocity);
        // Both arms pumping with a mostly-VERTICAL motion. The dominance
        // test keeps reaching, pointing and weapon handling from walking
        // the player.
        const bool vertical = vertical0 >= 0.55f * speed0 &&
                              vertical1 >= 0.55f * speed1;
        const float pump = std::min(vertical0, vertical1);
        if (vertical && pump > 0.55f) {
            gRunEnergy += (pump - 0.55f) * dt * 3.0f;
        }
    }
    gRunEnergy = std::clamp(gRunEnergy - dt * 2.0f, 0.0f, 1.4f);

    // Hysteresis: swings ease through zero at every arm turnaround, so the
    // latch starts high and only releases when the energy actually drains.
    if (gRunActive) {
        if (gRunEnergy < 0.10f) gRunActive = false;
    } else if (gRunEnergy > 0.30f) {
        gRunActive = true;
    }
    if (gRunActive) {
        out.forward = 1.0f;
        out.sprint = gRunEnergy > 0.80f;

        static double lastLog = 0.0;
        if (now - lastLog > 2.0) {
            lastLog = now;
            LOGI("[vr.gesture] run energy=%.2f sprint=%d",
                 gRunEnergy, out.sprint ? 1 : 0);
        }
    }
}

} // namespace savr::gesturemove
