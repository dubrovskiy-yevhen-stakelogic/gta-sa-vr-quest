#include "PhysicalWeapon.h"

#include "Calib.h"
#include "Driving.h"
#include "Holster.h"
#include "Locomotion.h"
#include "Log.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace savr::physicalweapon {
namespace {

constexpr int   kWeaponSlotCount = 13;
constexpr int   kOffWeapons = 0x730;
constexpr int   kWeaponStride = 0x20;
constexpr int   kOffActiveWeaponSlot = 0x8DC;

constexpr float kGripClose = 0.65f;
constexpr float kGripCatch = 0.55f;
constexpr float kGripOpen = 0.30f;
constexpr float kGripBlockedLatch = 0.45f;
constexpr float kHolsterGrabRadius = 0.27f;
constexpr float kHolsterReturnRadius = 0.24f;
constexpr float kSupportGrabRadius = 0.18f;
constexpr float kSupportHintRadius = 0.45f;
constexpr float kTransferRadius = 0.20f;
constexpr float kCatchRadius = 0.22f;
constexpr float kDropLifetimeSeconds = 1.8f;
constexpr float kMaximumThrowSpeed = 6.0f;
// A release is a short toss rather than a dead drop.  The tracked velocity is
// still authoritative (including a deliberate downward throw); this modest
// +Y impulse only gives a stationary release a visible, catchable arc.
constexpr float kReleaseUpwardImpulse = 1.0f;
// reference Quest build uses p = p0 + v*t - up*(2.1*t*t), i.e. 4.2 m/s^2 acceleration.
constexpr float kDropGravityTerm = 2.1f;

struct V3 {
    float x{}, y{}, z{};
};

struct Q {
    float x{}, y{}, z{}, w{1.0f};
};

struct DroppedWeapon {
    int slot{-1};
    TrackingPose startPose{};
    V3 velocity{};
    V3 angularAxis{0.0f, 1.0f, 0.0f};
    float angularSpeed{};
    double startedAt{};
};

std::mutex gMutex;
int gHeldSlot[kHandCount] = {-1, -1};
// Lock-free mirror of "which hands hold a weapon", refreshed at the end of
// every Update(). Driving reads it inside its own state lock to keep a
// weapon hand off the steering wheel; taking gMutex there would invert the
// gMutex -> driving-state order this file already uses.
std::atomic<unsigned int> gHeldMaskRelaxed{0};
// A support hand does not own a second copy of the primary's inventory slot.
// gSupportHand[primary] is the other controller or -1.
int gSupportHand[kHandCount] = {-1, -1};
bool gGripLatched[kHandCount]{};
int gPreferredHand = -1;
// Visual-only support-hand preview selected while the weapon calibration menu
// blocks interactions. It deliberately does not mutate gSupportHand/ownership.
int gSupportCalibrationPrimary = -1;
bool gInteractionsBlocked = true;
bool gGripLock = false;
bool gDualHoldEnabled = true;

xr::HandPose gHands[kHandCount]{};
V3 gHandVelocity[kHandCount]{};
V3 gHandAngularVelocity[kHandCount]{};
V3 gPreviousHandPosition[kHandCount]{};
Q gPreviousHandOrientation[kHandCount]{};
double gPreviousPoseTime[kHandCount]{};
bool gPreviousPoseValid[kHandCount]{};

TrackingPose gHeldVisualPose[kHandCount]{};
int gHeldVisualSlot[kHandCount] = {-1, -1};
bool gHeldVisualValid[kHandCount]{};
// One-hand model pose, captured before the support transform. Never overwrite
// this with a supported/final pose or shortest-arc rotation accumulates each frame.
TrackingPose gHeldBasePose[kHandCount]{};
int gHeldBaseSlot[kHandCount] = {-1, -1};
bool gHeldBaseValid[kHandCount]{};

DroppedWeapon gDropped[kHandCount]{};

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
float LengthSquared(V3 a) { return Dot(a, a); }
float Length(V3 a) { return std::sqrt(LengthSquared(a)); }
V3 Normalized(V3 a, V3 fallback = {0.0f, 1.0f, 0.0f}) {
    const float length = Length(a);
    return length > 1.0e-6f ? Scale(a, 1.0f / length) : fallback;
}
float Distance(V3 a, V3 b) { return Length(Sub(a, b)); }

Q NormalizeQ(Q q) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length <= 1.0e-6f) return {};
    const float inv = 1.0f / length;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Q MultiplyQ(Q a,Q b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}

Q ConjugateQ(Q q) { return {-q.x,-q.y,-q.z,q.w}; }

V3 Rotate(Q q, V3 v) {
    q = NormalizeQ(q);
    const V3 u{q.x, q.y, q.z};
    const float s = q.w;
    return Add(Add(Scale(u, 2.0f * Dot(u, v)),
                   Scale(v, s * s - Dot(u, u))),
               Scale(Cross(u, v), 2.0f * s));
}

V3 RotateAroundAxis(V3 value, V3 axis, float angle) {
    axis = Normalized(axis);
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return Add(Add(Scale(value, cosine), Scale(Cross(axis, value), sine)),
               Scale(axis, Dot(axis, value) * (1.0f - cosine)));
}

V3 PositionOf(const TrackingPose& pose) {
    return {pose.position[0], pose.position[1], pose.position[2]};
}

V3 GripPosition(const xr::HandPose& hand) {
    return {hand.gripPos[0], hand.gripPos[1], hand.gripPos[2]};
}

Q GripOrientation(const xr::HandPose& hand) {
    return NormalizeQ({hand.gripOri[0], hand.gripOri[1],
                       hand.gripOri[2], hand.gripOri[3]});
}

TrackingPose RawGripFallbackPose(const xr::HandPose& hand) {
    TrackingPose pose{};
    std::memcpy(pose.position, hand.gripPos, sizeof(pose.position));
    const Q q = GripOrientation(hand);
    const V3 right = Rotate(q, {1.0f, 0.0f, 0.0f});
    const V3 forward = Rotate(q, {0.0f, 0.0f, -1.0f});
    const V3 up = Rotate(q, {0.0f, 1.0f, 0.0f});
    pose.right[0] = right.x; pose.right[1] = right.y; pose.right[2] = right.z;
    pose.forward[0] = forward.x; pose.forward[1] = forward.y; pose.forward[2] = forward.z;
    pose.up[0] = up.x; pose.up[1] = up.y; pose.up[2] = up.z;
    return pose;
}

bool ValidHand(int hand) { return hand >= 0 && hand < kHandCount; }
bool ValidSlot(int slot) { return slot > 0 && slot < kWeaponSlotCount; }

bool IsTwoHandedWeaponTypeInternal(int weaponType) {
    // SA equivalents of reference Quest build's current Quest long-gun list. Compact SMGs and
    // pistols intentionally stay one-handed; MP5 and every full-size long gun
    // use a calibrated foregrip.
    switch (weaponType) {
        case 25:  // shotgun
        case 26:  // sawnoff
        case 27:  // combat shotgun
        case 29:  // MP5
        case 30:  // AK47
        case 31:  // M4
        case 33:  // country rifle
        case 34:  // sniper
        case 35:  // RPG
        case 36:  // heat-seeking RPG
        case 37:  // flamethrower
        case 38:  // minigun
            return true;
        default:
            return false;
    }
}

int WeaponTypeInSlot(void* ped, int slot) {
    if (ped == nullptr || !ValidSlot(slot)) return 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(ped);
    return *reinterpret_cast<const std::int32_t*>(
        bytes + kOffWeapons + slot * kWeaponStride);
}

bool OwnedConfiguredSlot(void* ped, int slot) {
    return ValidSlot(slot) && holster::FindPointForSlot(slot) >= 0 &&
           WeaponTypeInSlot(ped, slot) != 0;
}

int CurrentWeaponSlot(void* ped) {
    if (ped == nullptr) return -1;
    return *reinterpret_cast<const std::int8_t*>(
        reinterpret_cast<const std::uint8_t*>(ped) + kOffActiveWeaponSlot);
}

void SelectCurrentSlot(void* ped, int slot) {
    if (ped == nullptr || !ValidSlot(slot) || g.CPed_SetCurrentWeaponSlot == nullptr) return;
    if (CurrentWeaponSlot(ped) != slot) g.CPed_SetCurrentWeaponSlot(ped, slot);
}

int HeldHandForSlotLocked(int slot) {
    for (int hand = 0; hand < kHandCount; ++hand)
        if (gHeldSlot[hand] == slot) return hand;
    return -1;
}

int PrimaryHandForSupportLocked(int supportHand) {
    if (!ValidHand(supportHand)) return -1;
    for (int primary = 0; primary < kHandCount; ++primary)
        if (gSupportHand[primary] == supportHand) return primary;
    return -1;
}

bool IsSupportHandLocked(int hand) {
    return PrimaryHandForSupportLocked(hand) >= 0;
}

void ClearSupportForHandLocked(int hand) {
    if (!ValidHand(hand)) return;
    // `hand` may itself be a primary and/or the support side of a stale relation.
    gSupportHand[hand] = -1;
    for (int primary = 0; primary < kHandCount; ++primary)
        if (gSupportHand[primary] == hand) gSupportHand[primary] = -1;
}

int DroppedSourceForSlotLocked(int slot) {
    for (int source = 0; source < kHandCount; ++source)
        if (gDropped[source].slot == slot) return source;
    return -1;
}

std::uint32_t OccupiedSlotMaskLocked() {
    std::uint32_t mask = 0;
    for (int hand = 0; hand < kHandCount; ++hand) {
        if (ValidSlot(gHeldSlot[hand])) mask |= 1u << gHeldSlot[hand];
        if (ValidSlot(gDropped[hand].slot)) mask |= 1u << gDropped[hand].slot;
    }
    return mask;
}

int AnyHeldHandLocked() {
    if (ValidHand(gPreferredHand) && ValidSlot(gHeldSlot[gPreferredHand]))
        return gPreferredHand;
    for (int hand = 0; hand < kHandCount; ++hand)
        if (ValidSlot(gHeldSlot[hand])) return hand;
    return -1;
}

void ClearVisualPoseLocked(int hand) {
    if (!ValidHand(hand)) return;
    gHeldVisualValid[hand] = false;
    gHeldVisualSlot[hand] = -1;
    gHeldBaseValid[hand] = false;
    gHeldBaseSlot[hand] = -1;
}

void ClearHeldLocked(int hand) {
    if (!ValidHand(hand)) return;
    ClearSupportForHandLocked(hand);
    gHeldSlot[hand] = -1;
    ClearVisualPoseLocked(hand);
    if (gPreferredHand == hand) gPreferredHand = AnyHeldHandLocked();
}

void ResetRuntimeLocked() {
    for (int hand = 0; hand < kHandCount; ++hand) {
        gHeldSlot[hand] = -1;
        gSupportHand[hand] = -1;
        gGripLatched[hand] = false;
        gHands[hand] = xr::HandPose{};
        gHandVelocity[hand] = {};
        gHandAngularVelocity[hand] = {};
        gPreviousHandPosition[hand] = {};
        gPreviousHandOrientation[hand] = {};
        gPreviousPoseTime[hand] = 0.0;
        gPreviousPoseValid[hand] = false;
        ClearVisualPoseLocked(hand);
        gDropped[hand] = DroppedWeapon{};
    }
    gPreferredHand = -1;
    gSupportCalibrationPrimary = -1;
    gInteractionsBlocked = true;
}

bool DroppedPoseAtLocked(int sourceHand, double now, TrackingPose* out) {
    if (!ValidHand(sourceHand) || out == nullptr) return false;
    const DroppedWeapon& dropped = gDropped[sourceHand];
    if (!ValidSlot(dropped.slot)) return false;
    const float elapsed = static_cast<float>(now - dropped.startedAt);
    if (elapsed < 0.0f || elapsed > kDropLifetimeSeconds) return false;

    const V3 start = PositionOf(dropped.startPose);
    V3 position = Add(start, Scale(dropped.velocity, elapsed));
    // OpenXR LOCAL is +Y up.
    position.y -= kDropGravityTerm * elapsed * elapsed;
    out->position[0] = position.x;
    out->position[1] = position.y;
    out->position[2] = position.z;

    // Rotate every FINAL model axis in world/tracking space. Never reconstruct an
    // axis with a cross product: doing so would erase det<0 and change cull winding.
    const float angle = dropped.angularSpeed * elapsed;
    const V3 right = RotateAroundAxis(
        {dropped.startPose.right[0], dropped.startPose.right[1], dropped.startPose.right[2]},
        dropped.angularAxis, angle);
    const V3 forward = RotateAroundAxis(
        {dropped.startPose.forward[0], dropped.startPose.forward[1], dropped.startPose.forward[2]},
        dropped.angularAxis, angle);
    const V3 up = RotateAroundAxis(
        {dropped.startPose.up[0], dropped.startPose.up[1], dropped.startPose.up[2]},
        dropped.angularAxis, angle);
    out->right[0] = right.x; out->right[1] = right.y; out->right[2] = right.z;
    out->forward[0] = forward.x; out->forward[1] = forward.y; out->forward[2] = forward.z;
    out->up[0] = up.x; out->up[1] = up.y; out->up[2] = up.z;
    return true;
}

void ExpireDroppedLocked(double now) {
    for (int source = 0; source < kHandCount; ++source) {
        DroppedWeapon& dropped = gDropped[source];
        if (!ValidSlot(dropped.slot) || now - dropped.startedAt <= kDropLifetimeSeconds)
            continue;
        LOGI("[physwpn] slot %d respawned to holster", dropped.slot);
        dropped = DroppedWeapon{};
    }
}

void UpdateHandVelocityLocked(double now) {
    for (int hand = 0; hand < kHandCount; ++hand) {
        if (!gHands[hand].valid) {
            gHandVelocity[hand] = {};
            gHandAngularVelocity[hand] = {};
            gPreviousPoseValid[hand] = false;
            continue;
        }
        const V3 position = GripPosition(gHands[hand]);
        const Q orientation=GripOrientation(gHands[hand]);
        if (gPreviousPoseValid[hand]) {
            const double delta = now - gPreviousPoseTime[hand];
            if (delta >= 0.001 && delta <= 0.25) {
                V3 measured = Scale(
                    Sub(position, gPreviousHandPosition[hand]),
                    static_cast<float>(1.0 / delta));
                // OpenXR and the GameThread are not guaranteed to publish/read at
                // the same cadence.  A one-frame derivative therefore often
                // becomes zero exactly when grip release is observed.  Keep a
                // short low-pass history, as a physical controller naturally has
                // inertia, while still converging to rest within a few frames.
                if (LengthSquared(measured) < 0.0009f) measured = {};
                constexpr float kMeasuredWeight = 0.62f;
                gHandVelocity[hand] = Add(
                    Scale(gHandVelocity[hand], 1.0f - kMeasuredWeight),
                    Scale(measured, kMeasuredWeight));

                // Convert the controller's real orientation delta into a
                // world/tracking-space angular velocity. This preserves the
                // weapon pose at release and spins only when the wrist actually
                // supplied angular momentum.
                Q deltaQ=NormalizeQ(MultiplyQ(
                    orientation,ConjugateQ(gPreviousHandOrientation[hand])));
                if (deltaQ.w<0.0f)
                    deltaQ={-deltaQ.x,-deltaQ.y,-deltaQ.z,-deltaQ.w};
                const float vectorLength=std::sqrt(deltaQ.x*deltaQ.x+
                    deltaQ.y*deltaQ.y+deltaQ.z*deltaQ.z);
                V3 measuredAngular{};
                if (vectorLength>1.0e-5f) {
                    const float angle=2.0f*std::atan2(
                        vectorLength,std::clamp(deltaQ.w,-1.0f,1.0f));
                    measuredAngular=Scale(
                        {deltaQ.x/vectorLength,deltaQ.y/vectorLength,
                         deltaQ.z/vectorLength},
                        angle/static_cast<float>(delta));
                }
                gHandAngularVelocity[hand]=Add(
                    Scale(gHandAngularVelocity[hand],1.0f-kMeasuredWeight),
                    Scale(measuredAngular,kMeasuredWeight));
            } else {
                gHandVelocity[hand] = {};
                gHandAngularVelocity[hand] = {};
            }
        } else {
            gHandVelocity[hand] = {};
            gHandAngularVelocity[hand] = {};
        }
        gPreviousHandPosition[hand] = position;
        gPreviousHandOrientation[hand] = orientation;
        gPreviousPoseTime[hand] = now;
        gPreviousPoseValid[hand] = true;
    }
}

void GiveToHandLocked(void* ped, int hand, int slot, const char* reason) {
    if (!ValidHand(hand) || !ValidSlot(slot)) return;
    ClearSupportForHandLocked(hand);
    gHeldSlot[hand] = slot;
    gPreferredHand = hand;
    ClearVisualPoseLocked(hand);
    SelectCurrentSlot(ped, slot);
    const int weaponType = WeaponTypeInSlot(ped, slot);
    holster::RememberActiveWeapon(slot, weaponType);
    calib::SetActiveWeapon(weaponType);
    LOGI("[physwpn] %s slot=%d -> %s hand", reason, slot,
         hand == 0 ? "LEFT" : "RIGHT");
}

void StartDropLocked(int hand, int slot, double now) {
    if (!ValidHand(hand) || !ValidSlot(slot)) return;
    DroppedWeapon dropped{};
    dropped.slot = slot;
    dropped.startedAt = now;
    if (gHeldVisualValid[hand] && gHeldVisualSlot[hand] == slot)
        dropped.startPose = gHeldVisualPose[hand];
    else
        dropped.startPose = RawGripFallbackPose(gHands[hand]);

    const V3 trackedVelocity = gHandVelocity[hand];
    dropped.velocity = trackedVelocity;
    dropped.velocity.y += kReleaseUpwardImpulse;
    const float speed = Length(dropped.velocity);
    if (speed > kMaximumThrowSpeed)
        dropped.velocity = Scale(dropped.velocity, kMaximumThrowSpeed / speed);

    const V3 angularVelocity=gHandAngularVelocity[hand];
    dropped.angularSpeed=std::clamp(Length(angularVelocity),0.0f,10.0f);
    if (dropped.angularSpeed<0.35f) {
        dropped.angularSpeed=0.0f;
        dropped.angularAxis={0.0f,1.0f,0.0f};
    } else {
        dropped.angularAxis=Scale(angularVelocity,1.0f/dropped.angularSpeed);
    }

    // reference Quest build has one flying record per source hand; replacing this record makes
    // an older throw by the same hand respawn immediately rather than duplicate.
    gDropped[hand] = dropped;
    LOGI("[physwpn] dropped slot=%d hand=%d tracked=(%.2f %.2f %.2f) launch=(%.2f %.2f %.2f) spin=%.2f",
         slot, hand,
         trackedVelocity.x, trackedVelocity.y, trackedVelocity.z,
         dropped.velocity.x, dropped.velocity.y, dropped.velocity.z,
         dropped.angularSpeed);
}

void ValidateInventoryLocked(void* ped) {
    for (int hand = 0; hand < kHandCount; ++hand) {
        // A held item only needs to still exist in the ped inventory. Requiring
        // a configured holster point here silently knocked cheat-given gadgets
        // (spraycan/camera/goggles: slots with no body point) out of the hand
        // on the very next tick.
        if (ValidSlot(gHeldSlot[hand]) &&
            WeaponTypeInSlot(ped, gHeldSlot[hand]) == 0) {
            LOGI("[physwpn] held slot %d no longer owned", gHeldSlot[hand]);
            ClearHeldLocked(hand);
        }
        if (ValidSlot(gDropped[hand].slot) &&
            !OwnedConfiguredSlot(ped, gDropped[hand].slot)) {
            LOGI("[physwpn] dropped slot %d no longer owned/configured",
                 gDropped[hand].slot);
            gDropped[hand] = DroppedWeapon{};
        }
    }
}

void ChooseFiringHandLocked(void* ped) {
    int selected = AnyHeldHandLocked();
    for (int hand = 0; hand < kHandCount; ++hand) {
        if (!ValidSlot(gHeldSlot[hand]) || !gHands[hand].valid) continue;
        if (selected < 0 || !gHands[selected].valid ||
            gHands[hand].trigger > gHands[selected].trigger + 0.05f)
            selected = hand;
    }
    if (selected >= 0 && gHands[selected].trigger >= 0.10f)
        gPreferredHand = selected;
    else if (!ValidHand(gPreferredHand) || !ValidSlot(gHeldSlot[gPreferredHand]))
        gPreferredHand = selected;

    const int firing = AnyHeldHandLocked();
    if (firing >= 0) SelectCurrentSlot(ped, gHeldSlot[firing]);
}

// One-handed guns a seated driver can realistically draw and aim — the Vice
// City "immersive vehicle sidearm" rule. Two-handed longarms, melee and
// throwables stay holstered until back on foot.
bool VehicleSidearmWeaponType(int weaponType) {
    switch (weaponType) {
        case 22: // pistol
        case 23: // silenced
        case 24: // deagle
        case 26: // sawn-off
        case 28: // micro uzi
        case 32: // tec9
            return true;
        default:
            return false;
    }
}

int ClosestAvailableHolsterSlotLocked(
    void* ped, int hand,
    const float (*anchors)[3], const bool* anchorValid,
    float* distanceOut = nullptr) {
    if (!ValidHand(hand) || !gHands[hand].valid || anchors == nullptr ||
        anchorValid == nullptr)
        return -1;

    const bool inVehicle = g.FindPlayerVehicle &&
                           g.FindPlayerVehicle(-1, false) != nullptr;
    int closestSlot = -1;
    float closestDistance = holster::GrabRadiusMetres();
    const std::uint32_t occupied = OccupiedSlotMaskLocked();
    for (int point = 0; point < holster::PointCount(); ++point) {
        const int slot = holster::PointSlot(point);
        if (!anchorValid[point] || !OwnedConfiguredSlot(ped, slot) ||
            (occupied & (1u << slot)) != 0)
            continue;
        if (inVehicle &&
            !VehicleSidearmWeaponType(WeaponTypeInSlot(ped, slot)))
            continue;
        const float distance = Distance(
            GripPosition(gHands[hand]),
            {anchors[point][0], anchors[point][1], anchors[point][2]});
        if (distance < closestDistance) {
            closestDistance = distance;
            closestSlot = slot;
        }
    }
    if (distanceOut != nullptr) *distanceOut = closestDistance;
    return closestSlot;
}

bool BuildSupportAnchorLocked(int primaryHand, TrackingPose* out) {
    if (!ValidHand(primaryHand) || out == nullptr) return false;
    const int slot = gHeldSlot[primaryHand];
    if (!ValidSlot(slot) || !gHeldBaseValid[primaryHand] ||
        gHeldBaseSlot[primaryHand] != slot)
        return false;

    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    const int weaponType = WeaponTypeInSlot(ped, slot);
    if (!IsTwoHandedWeaponTypeInternal(weaponType)) return false;

    // reference Quest build v5's model-bound support contract: model RIGHT is the barrel,
    // model FORWARD is the top candidate, and their canonical cross product is
    // the lateral socket axis. Rebuilding this proper frame also deliberately
    // removes the reflected RenderWare winding from the wrist pose.
    const TrackingPose& model = gHeldBasePose[primaryHand];
    V3 barrel{model.right[0], model.right[1], model.right[2]};
    V3 top{model.forward[0], model.forward[1], model.forward[2]};
    if (LengthSquared(barrel) < 0.0001f || LengthSquared(top) < 0.0001f)
        return false;
    barrel = Normalized(barrel);
    top = Sub(top, Scale(barrel, Dot(top, barrel)));
    if (LengthSquared(top) < 0.0001f) return false;
    top = Normalized(top);
    V3 lateral = Cross(barrel, top);
    if (LengthSquared(lateral) < 0.0001f) return false;
    lateral = Normalized(lateral);
    top = Normalized(Cross(lateral, barrel), top);

    const calib::WeaponCalib calibration = calib::Snapshot(1, weaponType);
    int offsetX = calibration.supOffX;
    int offsetY = calibration.supOffY;
    int offsetZ = calibration.supOffZ;
    // RIGHT is the one saved/master profile. Match reference Quest build's derived LEFT
    // support calibration without asking the user to tune the same weapon
    // twice: lateral offset and the two handedness-sensitive rotations mirror.
    if (primaryHand == 0) offsetX = -offsetX;
    constexpr float kCalibUnitToMetres = 0.005f;
    const V3 origin = PositionOf(model);
    const V3 anchor = Add(
        Add(Add(origin, Scale(lateral, offsetX * kCalibUnitToMetres)),
            Scale(barrel, offsetY * kCalibUnitToMetres)),
        Scale(top, offsetZ * kCalibUnitToMetres));

    // reference Quest build's current Quest wrist contract is style-dependent. Build the
    // conventional visual hand frame here (right/forward/up), then let Xr apply
    // only the shared two-hand shortest arc. Keeping this in one place avoids
    // distributing palm mirroring across PhysicalWeapon, VrCamera and Xr.
    const int supportHand = ValidHand(gSupportHand[primaryHand])
        ? gSupportHand[primaryHand] : 1 - primaryHand;
    const float palmSign = supportHand == 0 ? -1.0f : 1.0f;
    const V3 weaponRight = barrel;
    const V3 weaponUp = Scale(lateral, -1.0f);
    const V3 weaponForward = Scale(top, -1.0f);
    V3 encodedPalmAxis = calibration.supStyle == calib::SUPPORT_FROM_BELOW
        ? Scale(weaponUp, -palmSign) : weaponRight;
    encodedPalmAxis = Normalized(encodedPalmAxis);
    V3 handUp = Scale(encodedPalmAxis, palmSign);
    V3 handForward = Normalized(weaponForward);
    V3 handRight = Cross(handUp, handForward);
    if (LengthSquared(handRight) < 0.0001f) return false;
    handRight = Normalized(handRight);
    constexpr float kHalfDegreeToRadians =
        0.5f * 3.14159265358979323846f / 180.0f;
    int rotationRawX = calibration.supRotX;
    int rotationRawY = calibration.supRotY;
    int rotationRawZ = calibration.supRotZ;
    if (primaryHand == 0) {
        rotationRawX = -rotationRawX;
        rotationRawZ = -rotationRawZ;
    }
    const float rotationX = rotationRawX * kHalfDegreeToRadians;
    const float rotationY = rotationRawY * kHalfDegreeToRadians;
    const float rotationZ = rotationRawZ * kHalfDegreeToRadians;
    if (rotationX != 0.0f) {
        handUp = RotateAroundAxis(handUp, handRight, rotationX);
        handForward = RotateAroundAxis(handForward, handRight, rotationX);
    }
    if (rotationY != 0.0f) {
        handRight = RotateAroundAxis(handRight, handUp, rotationY);
        handForward = RotateAroundAxis(handForward, handUp, rotationY);
    }
    if (rotationZ != 0.0f) {
        handRight = RotateAroundAxis(handRight, handForward, rotationZ);
        handUp = RotateAroundAxis(handUp, handForward, rotationZ);
    }
    handForward = Normalized(handForward);
    handUp = Sub(handUp, Scale(handForward, Dot(handUp, handForward)));
    if (LengthSquared(handUp) < 0.0001f) return false;
    handUp = Normalized(handUp);
    handRight = Normalized(Cross(handUp, handForward));
    if (supportHand == 0) {
        handUp = Scale(handUp, -1.0f);
        handRight = Scale(handRight, -1.0f);
    }

    out->position[0] = anchor.x;
    out->position[1] = anchor.y;
    out->position[2] = anchor.z;
    out->right[0] = handRight.x;
    out->right[1] = handRight.y;
    out->right[2] = handRight.z;
    out->forward[0] = handForward.x;
    out->forward[1] = handForward.y;
    out->forward[2] = handForward.z;
    out->up[0] = handUp.x;
    out->up[1] = handUp.y;
    out->up[2] = handUp.z;
    return true;
}

bool BuildTwoHandTransformLocked(int primaryHand, V3& pivot, V3& axis,
                                 float& angle) {
    if (!ValidHand(primaryHand) || !gHands[primaryHand].valid) return false;
    const int support = gSupportHand[primaryHand];
    if (!ValidHand(support) || !gHands[support].valid) return false;

    TrackingPose anchor{};
    if (!BuildSupportAnchorLocked(primaryHand, &anchor)) return false;
    pivot = GripPosition(gHands[primaryHand]);
    V3 expected = Sub(PositionOf(anchor), pivot);
    V3 actual = Sub(GripPosition(gHands[support]), pivot);
    if (LengthSquared(expected) < 0.0001f || LengthSquared(actual) < 0.0001f)
        return false;
    expected = Normalized(expected);
    actual = Normalized(actual);
    const float cosine = std::clamp(Dot(expected, actual), -1.0f, 1.0f);
    axis = Cross(expected, actual);
    const float sine = Length(axis);
    angle = 0.0f;
    if (sine < 0.000001f) {
        if (cosine < 0.0f) {
            const V3 reference = std::fabs(expected.y) < 0.9f
                ? V3{0.0f, 1.0f, 0.0f} : V3{1.0f, 0.0f, 0.0f};
            axis = Normalized(Cross(expected, reference));
            angle = 0.5f * 3.14159265358979323846f;
        } else {
            axis = {0.0f, 1.0f, 0.0f};
        }
    } else {
        axis = Scale(axis, 1.0f / sine);
        angle = std::min(std::atan2(sine, cosine),
                         0.5f * 3.14159265358979323846f);
    }
    return true;
}

TrackingPose NormalizedTrackingPose(const TrackingPose& pose) {
    TrackingPose normalized = pose;
    const V3 right = Normalized(
        {pose.right[0], pose.right[1], pose.right[2]}, {1.0f, 0.0f, 0.0f});
    const V3 forward = Normalized(
        {pose.forward[0], pose.forward[1], pose.forward[2]}, {0.0f, 0.0f, -1.0f});
    const V3 up = Normalized(
        {pose.up[0], pose.up[1], pose.up[2]}, {0.0f, 1.0f, 0.0f});
    normalized.right[0] = right.x;
    normalized.right[1] = right.y;
    normalized.right[2] = right.z;
    normalized.forward[0] = forward.x;
    normalized.forward[1] = forward.y;
    normalized.forward[2] = forward.z;
    normalized.up[0] = up.x;
    normalized.up[1] = up.y;
    normalized.up[2] = up.z;
    return normalized;
}

}  // namespace

void Init() {
    holster::Init();
    std::lock_guard<std::mutex> lock(gMutex);
    ResetRuntimeLocked();
    LOGI("[physwpn] initialised (reference Quest build dual-hand mode)");
}

void Update(bool interactionsBlocked, bool showHolsterMarkersWhileBlocked,
            bool showSupportMarkerWhileBlocked) {
    holster::Init();
    xr::HandPose latest[kHandCount]{};
    xr::GetHandPoses(latest);
    const double now = NowSeconds();

    std::lock_guard<std::mutex> lock(gMutex);
    std::memcpy(gHands, latest, sizeof(gHands));
    UpdateHandVelocityLocked(now);
    ExpireDroppedLocked(now);
    gInteractionsBlocked = interactionsBlocked;
    // The menu-persisted VC grip lock owns this flag now; SetGripLock stays as
    // a programmatic override between ticks only.
    gGripLock = holster::GripLockEnabled();

    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    const bool inVehicle = ped != nullptr && g.FindPlayerVehicle &&
                           g.FindPlayerVehicle(-1, false) != nullptr;
    // Vice City parity: IMMERSIVE driving keeps the physical weapon system
    // live in the seat — the body holsters stay reachable for sidearms and a
    // held gun keeps tracking. The DEFAULT-driving DRIVE-BY AIM = IMMERSIVE
    // option opts into the same weapon system without changing driving input.
    const bool vehicleImmersive =
        inVehicle && driving::VehicleWeaponsImmersive();
    // A hand holding the wheel/handlebar must not simultaneously draw from a
    // chest holster that happens to overlap the rim in space.
    driving::WheelVisualState seatWheel{};
    if (vehicleImmersive) driving::GetWheelVisualState(&seatWheel);
    if (ped == nullptr || g.CPed_SetCurrentWeaponSlot == nullptr ||
        (inVehicle && !vehicleImmersive)) {
        const bool hadTransient = OccupiedSlotMaskLocked() != 0;
        ResetRuntimeLocked();
        gHeldMaskRelaxed.store(0, std::memory_order_relaxed);
        xr::SetHolsterMarkers(nullptr, 0);
        if (hadTransient) LOGI("[physwpn] transient state reset (no on-foot player)");
        return;
    }

    ValidateInventoryLocked(ped);

    // PG_CALIB previews the free hand directly on the calibrated support socket.
    // Keep the preferred/latched held gun authoritative, and never manufacture a
    // physical support relation: menus must not grab, transfer or change ownership.
    gSupportCalibrationPrimary = -1;
    if (interactionsBlocked && showSupportMarkerWhileBlocked) {
        const int primary = AnyHeldHandLocked();
        const int support = ValidHand(primary) ? 1 - primary : -1;
        const int slot = ValidHand(primary) ? gHeldSlot[primary] : -1;
        if (ValidSlot(slot) && ValidHand(support) &&
            IsTwoHandedWeaponTypeInternal(WeaponTypeInSlot(ped, slot)) &&
            (!ValidSlot(gHeldSlot[support]) || gSupportHand[primary] == support)) {
            gSupportCalibrationPrimary = primary;
        }
    }

    float anchors[holster::POINT_COUNT][3]{};
    bool anchorValid[holster::POINT_COUNT]{};
    for (int point = 0; point < holster::PointCount(); ++point)
        anchorValid[point] = vrcam::GetHolsterAnchorTracking(point, anchors[point]);

    if (!interactionsBlocked) {
        std::uint32_t handledHands = 0;

        // Keep/release an established support grip first. Releasing only the
        // support side detaches it. Releasing the primary while support remains
        // squeezed naturally promotes that controller instead of throwing the
        // weapon -- the newer Quest reference Quest build hand-off contract.
        for (int primary = 0; primary < kHandCount; ++primary) {
            const int support = gSupportHand[primary];
            if (!ValidHand(support)) continue;
            const int slot = gHeldSlot[primary];
            const int weaponType = WeaponTypeInSlot(ped, slot);
            if (!ValidSlot(slot) || ValidSlot(gHeldSlot[support]) ||
                !IsTwoHandedWeaponTypeInternal(weaponType) ||
                !gHands[primary].valid || !gHands[support].valid) {
                LOGI("[physwpn] support relation cleared primary=%d support=%d",
                     primary, support);
                gSupportHand[primary] = -1;
                continue;
            }
            if (gHands[support].grip <= kGripOpen) {
                LOGI("[physwpn] support released slot=%d hand=%d", slot, support);
                gSupportHand[primary] = -1;
                continue;
            }

            handledHands |= 1u << support;
            if (gHands[primary].grip <= kGripOpen && !gGripLock) {
                const bool haveFinalPose =
                    gHeldVisualValid[primary] && gHeldVisualSlot[primary] == slot;
                const TrackingPose finalPose = haveFinalPose
                    ? gHeldVisualPose[primary] : RawGripFallbackPose(gHands[primary]);
                ClearHeldLocked(primary);
                GiveToHandLocked(ped, support, slot, "promoted support");
                gHeldVisualPose[support] = finalPose;
                gHeldVisualSlot[support] = slot;
                gHeldVisualValid[support] = true;
                gGripLatched[support] = true;
                handledHands |= (1u << primary) | (1u << support);
            }
        }

        // A fresh squeeze at the calibrated foregrip creates a real support
        // hand, not an inventory transfer. A nearby available holster wins first
        // so dual independent weapons remain easy to acquire.
        for (int primary = 0; primary < kHandCount; ++primary) {
            const int support = 1 - primary;
            const int slot = gHeldSlot[primary];
            const int weaponType = WeaponTypeInSlot(ped, slot);
            if (!ValidSlot(slot) || ValidHand(gSupportHand[primary]) ||
                ValidSlot(gHeldSlot[support]) || IsSupportHandLocked(support) ||
                (handledHands & (1u << support)) != 0 ||
                !IsTwoHandedWeaponTypeInternal(weaponType) ||
                !gHands[support].valid || gHands[support].grip < kGripClose ||
                gGripLatched[support])
                continue;
            if (ClosestAvailableHolsterSlotLocked(
                    ped, support, anchors, anchorValid) >= 0)
                continue;

            TrackingPose anchor{};
            if (!BuildSupportAnchorLocked(primary, &anchor)) continue;
            const float reach = Distance(
                GripPosition(gHands[support]), PositionOf(anchor));
            if (reach > kSupportGrabRadius) continue;

            gSupportHand[primary] = support;
            gGripLatched[support] = true;
            handledHands |= 1u << support;
            LOGI("[physwpn] support grabbed slot=%d %s supports %s reach=%.3f",
                 slot, support == 0 ? "LEFT" : "RIGHT",
                 primary == 0 ? "LEFT" : "RIGHT", reach);

            // Primary release and support close may arrive in one XR sample.
            // Promote immediately so the generic drop pass below cannot destroy
            // this deliberate hand-off.
            if (gHands[primary].grip <= kGripOpen && !gGripLock) {
                const TrackingPose finalPose =
                    gHeldVisualValid[primary] && gHeldVisualSlot[primary] == slot
                        ? gHeldVisualPose[primary]
                        : RawGripFallbackPose(gHands[primary]);
                ClearHeldLocked(primary);
                GiveToHandLocked(ped, support, slot, "promoted fresh support");
                gHeldVisualPose[support] = finalPose;
                gHeldVisualSlot[support] = slot;
                gHeldVisualValid[support] = true;
                handledHands |= (1u << primary) | (1u << support);
            }
        }

        // Explicit hand-to-hand transfer at the final rendered gun body.
        for (int receiver = 0; receiver < kHandCount; ++receiver) {
            const int source = 1 - receiver;
            if (ValidSlot(gHeldSlot[receiver]) || !ValidSlot(gHeldSlot[source]) ||
                IsSupportHandLocked(receiver) ||
                (handledHands & (1u << receiver)) != 0 ||
                !gHands[receiver].valid || gHands[receiver].grip < kGripClose ||
                gGripLatched[receiver])
                continue;
            // A free hand intentionally reaching an available body socket wants
            // that second weapon.  Do not steal the already-held gun merely
            // because both hands happen to be within the transfer radius; the
            // single holster-grab path below will perform the acquisition.
            if (ClosestAvailableHolsterSlotLocked(
                    ped, receiver, anchors, anchorValid) >= 0)
                continue;
            const bool haveFinalPose =
                gHeldVisualValid[source] && gHeldVisualSlot[source] == gHeldSlot[source];
            const TrackingPose transferredPose = haveFinalPose
                ? gHeldVisualPose[source] : RawGripFallbackPose(gHands[source]);
            const V3 weaponPosition = PositionOf(transferredPose);
            const V3 receiverPosition = GripPosition(gHands[receiver]);
            // Some SA weapon clumps have their object origin away from the grip.
            // Accept either the final rendered origin (reference Quest build behaviour) or the
            // primary controller/handle itself, so a natural hand-over at the
            // visible holding hand cannot miss solely because of model pivots.
            const float modelDistance = Distance(receiverPosition, weaponPosition);
            const float handleDistance = Distance(
                receiverPosition, GripPosition(gHands[source]));
            if (std::min(modelDistance, handleDistance) > kTransferRadius)
                continue;
            const int slot = gHeldSlot[source];
            ClearHeldLocked(source);
            GiveToHandLocked(ped, receiver, slot, "transferred");
            // Preserve the exact reflected final basis until the renderer publishes
            // the receiver's newly calibrated model pose on the next render pass.
            gHeldVisualPose[receiver] = transferredPose;
            gHeldVisualSlot[receiver] = slot;
            gHeldVisualValid[receiver] = true;
            gGripLatched[receiver] = true;
            handledHands |= (1u << receiver) | (1u << source);
            LOGI("[physwpn] transfer reach model=%.3f handle=%.3f",
                 modelDistance, handleDistance);
        }

        // A flying weapon can be caught by either free, closed hand.
        for (int receiver = 0; receiver < kHandCount; ++receiver) {
            if (ValidSlot(gHeldSlot[receiver]) || !gHands[receiver].valid ||
                IsSupportHandLocked(receiver) ||
                (handledHands & (1u << receiver)) != 0 ||
                gHands[receiver].grip < kGripCatch ||
                (!gDualHoldEnabled && AnyHeldHandLocked() >= 0))
                continue;
            for (int source = 0; source < kHandCount; ++source) {
                TrackingPose flying{};
                if (!DroppedPoseAtLocked(source, now, &flying) ||
                    Distance(GripPosition(gHands[receiver]), PositionOf(flying)) > kCatchRadius)
                    continue;
                const int slot = gDropped[source].slot;
                gDropped[source] = DroppedWeapon{};
                GiveToHandLocked(ped, receiver, slot, "caught");
                gHeldVisualPose[receiver] = flying;
                gHeldVisualSlot[receiver] = slot;
                gHeldVisualValid[receiver] = true;
                gGripLatched[receiver] = true;
                handledHands |= 1u << receiver;
                break;
            }
        }

        // Release returns at the matching socket, otherwise creates a flying copy.
        for (int hand = 0; hand < kHandCount; ++hand) {
            if ((handledHands & (1u << hand)) != 0 ||
                !ValidSlot(gHeldSlot[hand]) || !gHands[hand].valid ||
                gHands[hand].grip > kGripOpen)
                continue;
            const int slot = gHeldSlot[hand];
            const int point = holster::FindPointForSlot(slot);
            const bool returnToHolster = point >= 0 && anchorValid[point] &&
                Distance(GripPosition(gHands[hand]),
                         {anchors[point][0], anchors[point][1], anchors[point][2]}) <=
                    kHolsterReturnRadius;
            if (gGripLock && !returnToHolster) continue;

            if (!returnToHolster) StartDropLocked(hand, slot, now);
            ClearHeldLocked(hand);
            if (returnToHolster)
                LOGI("[physwpn] returned slot=%d to %s", slot, holster::PointName(point));
        }

        // Fresh close near the closest available configured socket grabs that slot.
        for (int hand = 0; hand < kHandCount; ++hand) {
            if (ValidSlot(gHeldSlot[hand]) || !gHands[hand].valid ||
                IsSupportHandLocked(hand) ||
                (handledHands & (1u << hand)) != 0 ||
                gHands[hand].grip < kGripClose || gGripLatched[hand] ||
                (!gDualHoldEnabled && AnyHeldHandLocked() >= 0) ||
                (vehicleImmersive && seatWheel.grabbed[hand]))
                continue;
            float closestDistance = holster::GrabRadiusMetres();
            const int closestSlot = ClosestAvailableHolsterSlotLocked(
                ped, hand, anchors, anchorValid, &closestDistance);
            if (closestSlot >= 0) {
                GiveToHandLocked(ped, hand, closestSlot, "grabbed");
                gGripLatched[hand] = true;
                LOGI("[physwpn] holster grab reach=%.3f", closestDistance);
            }
        }
    }

    for (int hand = 0; hand < kHandCount; ++hand) {
        if (interactionsBlocked)
            gGripLatched[hand] = gHands[hand].valid &&
                                 gHands[hand].grip >= kGripBlockedLatch;
        else if (!gHands[hand].valid || gHands[hand].grip <= kGripOpen)
            gGripLatched[hand] = false;
        else if (gHands[hand].grip >= kGripClose)
            gGripLatched[hand] = true;
    }

    // L2/R2 edit calibration values while a VR menu is open. Do not let those
    // same axes silently switch the preferred held gun/profile mid-calibration.
    if (!interactionsBlocked) {
        ChooseFiringHandLocked(ped);
    } else {
        const int held = AnyHeldHandLocked();
        if (held >= 0) SelectCurrentSlot(ped, gHeldSlot[held]);
    }

    float markers[holster::POINT_COUNT + 1][3]{};
    int markerCount = 0;
    const bool gameplayMarkers = !interactionsBlocked &&
                                 holster::GripMarkersEnabled();
    const bool holsterCalibrationMarkers = interactionsBlocked &&
        showHolsterMarkersWhileBlocked && holster::GripMarkersEnabled();
    const bool supportCalibrationMarker = interactionsBlocked &&
                                          showSupportMarkerWhileBlocked;
    if (gameplayMarkers || holsterCalibrationMarkers ||
        supportCalibrationMarker) {
        const std::uint32_t occupied = OccupiedSlotMaskLocked();
        if (gameplayMarkers || holsterCalibrationMarkers) {
            for (int point = 0; point < holster::PointCount(); ++point) {
                const int slot = holster::PointSlot(point);
                if (!anchorValid[point]) continue;
                // Holster calibration is spatial: every valid socket (including
                // EMPTY and the occupied preview socket) must remain visible.
                // Normal gameplay retains the owned + unoccupied reach hints.
                if (!holsterCalibrationMarkers &&
                    (!OwnedConfiguredSlot(ped, slot) ||
                     (occupied & (1u << slot)) != 0))
                        continue;
                std::memcpy(markers[markerCount], anchors[point],
                            sizeof(markers[markerCount]));
                ++markerCount;
            }
        }

        // Current reference Quest build exposes the model-bound foregrip with the same marker
        // toggle as body holsters during gameplay. Weapon calibration instead
        // forces this one socket, ignores proximity, and suppresses body markers:
        // changing SUPPORT OFFSET/ROT therefore has immediate, unambiguous
        // spatial feedback even when normal grip hints are disabled. Menu
        // blocking still prevents this marker from becoming an interactive grab.
        if ((gameplayMarkers || supportCalibrationMarker) &&
            markerCount < holster::POINT_COUNT + 1) {
            for (int primary = 0; primary < kHandCount; ++primary) {
                if (supportCalibrationMarker &&
                    primary != gSupportCalibrationPrimary)
                    continue;
                const int slot = gHeldSlot[primary];
                const int support = 1 - primary;
                if (!ValidSlot(slot) ||
                    !IsTwoHandedWeaponTypeInternal(WeaponTypeInSlot(ped, slot)) ||
                    (!supportCalibrationMarker && !gHands[support].valid) ||
                    (ValidSlot(gHeldSlot[support]) && gSupportHand[primary] != support))
                    continue;
                TrackingPose anchor{};
                if (!BuildSupportAnchorLocked(primary, &anchor)) continue;
                // With an established support grip the rendered weapon has
                // already received the shortest-arc correction. Put the forced
                // calibration marker through that same transform so it remains
                // on the visible foregrip instead of showing the stale BASE
                // socket when the menu was opened with both grips held.
                if (ValidHand(gSupportHand[primary])) {
                    V3 pivot{}, axis{};
                    float angle = 0.0f;
                    if (BuildTwoHandTransformLocked(primary, pivot, axis, angle)) {
                        const V3 finalAnchor = Add(
                            pivot, RotateAroundAxis(
                                Sub(PositionOf(anchor), pivot), axis, angle));
                        anchor.position[0] = finalAnchor.x;
                        anchor.position[1] = finalAnchor.y;
                        anchor.position[2] = finalAnchor.z;
                    }
                }
                if (!supportCalibrationMarker &&
                    (gSupportHand[primary] == support ||
                     Distance(GripPosition(gHands[support]),
                              PositionOf(anchor)) > kSupportHintRadius))
                    continue;
                std::memcpy(markers[markerCount], anchor.position,
                            sizeof(markers[markerCount]));
                ++markerCount;
                break; // two controllers can form at most one support pair
            }
        }
    }
    xr::SetHolsterMarkers(markerCount > 0 ? markers : nullptr, markerCount);

    unsigned int heldMask = 0;
    for (int hand = 0; hand < kHandCount; ++hand)
        if (ValidSlot(gHeldSlot[hand])) heldMask |= 1u << hand;
    gHeldMaskRelaxed.store(heldMask, std::memory_order_relaxed);
}

void ResetTransient() {
    std::lock_guard<std::mutex> lock(gMutex);
    ResetRuntimeLocked();
    gHeldMaskRelaxed.store(0, std::memory_order_relaxed);
    xr::SetHolsterMarkers(nullptr, 0);
}

bool GetHandPosesSnapshot(xr::HandPose out[kHandCount]) {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(gMutex);
    std::memcpy(out, gHands, sizeof(gHands));
    return gHands[0].valid || gHands[1].valid;
}

bool IsVehicleSidearmWeaponType(int weaponType) {
    return VehicleSidearmWeaponType(weaponType);
}

bool ForceHold(int hand, int slot) {
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (ped == nullptr || !ValidHand(hand) || !ValidSlot(slot)) return false;
    std::lock_guard<std::mutex> lock(gMutex);
    if (WeaponTypeInSlot(ped, slot) == 0) return false;
    const int holder = HeldHandForSlotLocked(slot);
    if (holder >= 0 && holder != hand) ClearHeldLocked(holder);
    // A flying copy of the same slot must not respawn as a duplicate.
    for (int h = 0; h < kHandCount; ++h)
        if (gDropped[h].slot == slot) gDropped[h] = DroppedWeapon{};
    GiveToHandLocked(ped, hand, slot, "cheat give");
    gGripLatched[hand] = true;
    return true;
}

// Mission gadgets (spray can in Tagging Up Turf, camera, extinguisher) arrive
// in slot 9, which shipped with no holster point — the script said "spray the
// wall" while the player had nothing to grab. When the ped owns a gadget and
// no point offers slot 9 yet, auto-assign the first EMPTY body point so the
// item simply appears on the body. Players can still move it in the loadout.
void AutoAssignGadgetPoint() {
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (ped == nullptr) return;
    if (WeaponTypeInSlot(ped, 9) == 0) return;
    if (holster::FindPointForSlot(9) >= 0) return;
    for (int point = 0; point < holster::PointCount(); ++point) {
        if (holster::IsPointFixed(point)) continue;
        if (holster::PointSlot(point) >= 0) continue;
        if (holster::SetPointSlot(point, 9)) {
            LOGI("[physwpn] gadget slot auto-assigned to %s",
                 holster::PointName(point));
        }
        return;
    }
}

void AutoEquipParachute() {
    // No wearing ritual: after ~0.7s of genuine freefall with a parachute in
    // the inventory and empty tracked hands, the chute becomes the active
    // weapon by itself. The pad hook then maps the deploy button, so falling
    // + press = open canopy.
    static double airborneSince = 0.0;
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (ped == nullptr) { airborneSince = 0.0; return; }
    // AUTO PARACHUTE (locomotion menu): keep one in the inventory at all
    // times so no cheat-menu trip is needed before a jump.
    if (locomotion::AutoParachuteEnabled() && g.CPed_GiveWeapon != nullptr &&
        WeaponTypeInSlot(ped, 11) != 46) {
        g.CPed_GiveWeapon(ped, 46, 1u, false);
        LOGI("[physwpn] auto parachute re-issued");
    }
    if (g.CPed_SetCurrentWeaponSlot == nullptr ||
        g.CPedGeometryAnalyser_IsInAir == nullptr ||
        (g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false) != nullptr) ||
        WeaponTypeInSlot(ped, 11) != 46) {
        airborneSince = 0.0;
        return;
    }
    // IsInAir raycasts the world collision at the ped's position. A ped whose
    // matrix is missing or holds a non-finite/absurd position (transient during
    // a weapon switch or streaming churn) makes the vertical-line test index a
    // bad sector and fault inside CCollision::ProcessLineTriangle. Validate the
    // position first and skip the raycast that frame instead of crashing; the
    // check simply retries next frame once the ped is settled.
    const std::uintptr_t pedMtx =
        *reinterpret_cast<std::uintptr_t*>(static_cast<char*>(ped) + 0x18);
    if (pedMtx == 0) { airborneSince = 0.0; return; }
    const float px = *reinterpret_cast<float*>(pedMtx + 0x30);
    const float py = *reinterpret_cast<float*>(pedMtx + 0x34);
    const float pz = *reinterpret_cast<float*>(pedMtx + 0x38);
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
        std::abs(px) > 20000.0f || std::abs(py) > 20000.0f ||
        pz < -1000.0f || pz > 5000.0f) {
        airborneSince = 0.0;
        return;
    }
    if (!g.CPedGeometryAnalyser_IsInAir(ped)) {
        airborneSince = 0.0;
        return;
    }
    const double now = NowSeconds();
    if (airborneSince == 0.0) { airborneSince = now; return; }
    if (now - airborneSince < 0.7) return;
    if (gHeldMaskRelaxed.load(std::memory_order_relaxed) != 0) return;
    if (CurrentWeaponSlot(ped) != 11) {
        g.CPed_SetCurrentWeaponSlot(ped, 11);
        LOGI("[physwpn] freefall: parachute auto-equipped");
    }
}

void EnforceBikeWeaponLimit() {
    // A rider can mount the bike with a two-handed longarm already selected —
    // the holster grab filter never sees that path. Kick the game's active
    // slot back to fists whenever a seated bike rider holds anything that is
    // not a one-handed sidearm.
    if (g.FindPlayerVehicle == nullptr || g.FindPlayerPed == nullptr ||
        g.CVehicle_GetVehicleAppearance == nullptr)
        return;
    void* vehicle = g.FindPlayerVehicle(-1, false);
    if (vehicle == nullptr ||
        g.CVehicle_GetVehicleAppearance(vehicle) != 2)
        return;
    void* ped = g.FindPlayerPed(-1);
    if (ped == nullptr) return;
    const int slot = CurrentWeaponSlot(ped);
    if (slot <= 0) return;
    const int type = WeaponTypeInSlot(ped, slot);
    if (type != 0 && !VehicleSidearmWeaponType(type)) {
        SelectCurrentSlot(ped, 0);
        LOGI("[physicalweapon] bike weapon limit: slot %d type %d -> fists",
             slot, type);
    }
}

unsigned int HeldHandMaskRelaxed() {
    return gHeldMaskRelaxed.load(std::memory_order_relaxed);
}

int HeldSlot(int hand) {
    std::lock_guard<std::mutex> lock(gMutex);
    return ValidHand(hand) ? gHeldSlot[hand] : -1;
}

int HeldHandForSlot(int slot) {
    std::lock_guard<std::mutex> lock(gMutex);
    return HeldHandForSlotLocked(slot);
}

int PreferredHeldHand() {
    std::lock_guard<std::mutex> lock(gMutex);
    return AnyHeldHandLocked();
}

int ActiveHeldSlot() {
    std::lock_guard<std::mutex> lock(gMutex);
    const int hand = AnyHeldHandLocked();
    return hand >= 0 ? gHeldSlot[hand] : -1;
}

bool IsTwoHandedWeaponType(int weaponType) {
    return IsTwoHandedWeaponTypeInternal(weaponType);
}

int SupportHand(int primaryHand) {
    std::lock_guard<std::mutex> lock(gMutex);
    return ValidHand(primaryHand) && ValidHand(gSupportHand[primaryHand])
        ? gSupportHand[primaryHand] : -1;
}

int PrimaryHand(int supportHand) {
    std::lock_guard<std::mutex> lock(gMutex);
    return PrimaryHandForSupportLocked(supportHand);
}

bool IsSupportHand(int hand) {
    std::lock_guard<std::mutex> lock(gMutex);
    return IsSupportHandLocked(hand);
}

float SupportWeight(int primaryHand) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!ValidHand(primaryHand)) return 0.0f;
    const int support = gSupportHand[primaryHand];
    if (!ValidHand(support) || !gHands[support].valid) return 0.0f;
    return std::clamp(gHands[support].grip, 0.0f, 1.0f);
}

bool GetSupportHandPoseTracking(int primaryHand, TrackingPose* out) {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!ValidHand(primaryHand)) return false;
    const int support = gSupportHand[primaryHand];
    if (!ValidHand(support) || !gHands[support].valid) return false;
    *out = RawGripFallbackPose(gHands[support]);
    return true;
}

bool GetSupportAnchorTracking(int primaryHand, TrackingPose* out) {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(gMutex);
    return BuildSupportAnchorLocked(primaryHand, out);
}

int SupportCalibrationPrimaryHand() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gInteractionsBlocked && ValidHand(gSupportCalibrationPrimary)
        ? gSupportCalibrationPrimary : -1;
}

bool GetTwoHandTransformTracking(int primaryHand, float pivotOut[3],
                                 float axisOut[3], float* angleRadiansOut) {
    if (pivotOut == nullptr || axisOut == nullptr || angleRadiansOut == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(gMutex);
    V3 pivot{}, axis{};
    float angle = 0.0f;
    if (!BuildTwoHandTransformLocked(primaryHand, pivot, axis, angle))
        return false;

    pivotOut[0] = pivot.x;
    pivotOut[1] = pivot.y;
    pivotOut[2] = pivot.z;
    axisOut[0] = axis.x;
    axisOut[1] = axis.y;
    axisOut[2] = axis.z;
    *angleRadiansOut = angle;
    return true;
}

int DroppedSlot(int sourceHand) {
    std::lock_guard<std::mutex> lock(gMutex);
    return ValidHand(sourceHand) ? gDropped[sourceHand].slot : -1;
}

bool GetDroppedPoseTracking(int sourceHand, TrackingPose* out) {
    std::lock_guard<std::mutex> lock(gMutex);
    return DroppedPoseAtLocked(sourceHand, NowSeconds(), out);
}

bool GetDroppedPoseForSlotTracking(int slot, TrackingPose* out) {
    std::lock_guard<std::mutex> lock(gMutex);
    return DroppedPoseAtLocked(DroppedSourceForSlotLocked(slot), NowSeconds(), out);
}

std::uint32_t OccupiedSlotMask() {
    std::lock_guard<std::mutex> lock(gMutex);
    return OccupiedSlotMaskLocked();
}

bool IsSlotHeld(int slot) {
    std::lock_guard<std::mutex> lock(gMutex);
    return HeldHandForSlotLocked(slot) >= 0;
}

bool IsSlotDropped(int slot) {
    std::lock_guard<std::mutex> lock(gMutex);
    return DroppedSourceForSlotLocked(slot) >= 0;
}

void PublishHeldVisualPoseTracking(int hand, int slot, const TrackingPose* pose) {
    if (pose == nullptr) return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!ValidHand(hand) || gHeldSlot[hand] != slot) return;
    const TrackingPose normalized = NormalizedTrackingPose(*pose);
    gHeldVisualPose[hand] = normalized;
    gHeldVisualSlot[hand] = slot;
    gHeldVisualValid[hand] = true;
    // Compatibility with the old single-publish renderer: while unsupported the
    // final pose is also the one-hand base. Once support engages, freeze that
    // base and never feed the rotated final pose back into the next transform.
    if (!ValidHand(gSupportHand[hand])) {
        gHeldBasePose[hand] = normalized;
        gHeldBaseSlot[hand] = slot;
        gHeldBaseValid[hand] = true;
    }
}

bool GetHeldVisualPoseTracking(int hand, int slot, TrackingPose* out) {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!ValidHand(hand) || !ValidSlot(slot) || gHeldSlot[hand] != slot ||
        !gHeldVisualValid[hand] || gHeldVisualSlot[hand] != slot) {
        return false;
    }
    *out = gHeldVisualPose[hand];
    return true;
}

void PublishHeldBasePoseTracking(int hand, int slot, const TrackingPose* pose) {
    if (pose == nullptr) return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!ValidHand(hand) || gHeldSlot[hand] != slot) return;
    gHeldBasePose[hand] = NormalizedTrackingPose(*pose);
    gHeldBaseSlot[hand] = slot;
    gHeldBaseValid[hand] = true;
}

int FiringHand() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gInteractionsBlocked ? -1 : AnyHeldHandLocked();
}

float FireTrigger() {
    std::lock_guard<std::mutex> lock(gMutex);
    const int hand = gInteractionsBlocked ? -1 : AnyHeldHandLocked();
    if (hand < 0 || !gHands[hand].valid) return 0.0f;
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    const int weaponType = WeaponTypeInSlot(ped, gHeldSlot[hand]);
    // Types 1..15 are driven by Melee's controller-velocity sweep. Forwarding
    // the trigger too would run SA's animated fight task and double-hit targets.
    if (weaponType >= 1 && weaponType <= 15) return 0.0f;
    // Physical throwables have their own R2 hold-preview-release owner. Sending
    // this axis to CPad would also start CTaskSimpleThrowProjectile and could
    // manufacture a second grenade after our direct native release.
    if (weaponType == 16 || weaponType == 17 || weaponType == 18 ||
        weaponType == 39) return 0.0f;
    // Launchers are fired on a fresh physical-trigger edge by Throwable's
    // direct CWeapon::Fire owner. Forwarding the same axis into CPad would run
    // the legacy rocket-camera task as a second competing fire path.
    if (weaponType == 35 || weaponType == 36) return 0.0f;
    return std::clamp(gHands[hand].trigger, 0.0f, 1.0f);
}

void ReleaseAfterUse(int hand, int slot) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!ValidHand(hand) || !ValidSlot(slot) || gHeldSlot[hand] != slot)
        return;
    ClearHeldLocked(hand);
    LOGI("[physwpn] consumed slot=%d released from %s hand", slot,
         hand == 0 ? "LEFT" : "RIGHT");
}

void SetGripLock(bool enabled) {
    std::lock_guard<std::mutex> lock(gMutex);
    gGripLock = enabled;
}

bool GripLock() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gGripLock;
}

void SetDualHoldEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(gMutex);
    gDualHoldEnabled = enabled;
}

bool DualHoldEnabled() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gDualHoldEnabled;
}

}  // namespace savr::physicalweapon
