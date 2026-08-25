#include "ScopeAim.h"

#include "Log.h"
#include "PhysicalWeapon.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace savr::scopeaim {
namespace {

using GameVec3 = GameSymbols::Vec3;

constexpr int kSniperType = 34;
constexpr int kRpgType = 35;
constexpr int kHeatSeekerType = 36;
constexpr int kCameraType = 43;
constexpr int kWeaponSlots = 13;
constexpr int kOffWeapons = 0x730;
constexpr int kWeaponStride = 0x20;
constexpr int kColPointSize = 0x2c;
constexpr float kSniperZoom = 2.5f;
constexpr float kLauncherZoom = 1.8f;
constexpr float kCameraZoom = 1.8f;
constexpr float kCentreRayDistance = 150.0f;
constexpr std::uint64_t kEnterDwellMs = 120;
constexpr std::uint64_t kReleaseGraceMs = 180;

struct V3 {
    float x{}, y{}, z{};
};

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float LengthSquared(V3 v) { return Dot(v, v); }
float Length(V3 v) { return std::sqrt(std::max(0.0f, LengthSquared(v))); }
bool Finite(V3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
V3 Normalized(V3 v) {
    const float lengthSq = LengthSquared(v);
    if (!std::isfinite(lengthSq) || lengthSq < 1.0e-8f) return {};
    return v * (1.0f / std::sqrt(lengthSq));
}

V3 RotateQuaternion(const float q[4], V3 v) {
    const V3 u{q[0], q[1], q[2]};
    const float s = q[3];
    const float uv = Dot(u, v);
    const float uu = Dot(u, u);
    const V3 cross{u.y * v.z - u.z * v.y,
                   u.z * v.x - u.x * v.z,
                   u.x * v.y - u.y * v.x};
    return u * (2.0f * uv) + v * (s * s - uu) + cross * (2.0f * s);
}

std::uint64_t NowMs() {
    using Clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

struct ColPoint {
    alignas(4) std::uint8_t bytes[kColPointSize]{};
    V3 Point() const {
        V3 point{};
        std::memcpy(&point, bytes, sizeof(point));
        return point;
    }
};
static_assert(sizeof(ColPoint) == kColPointSize);

// One atomic publishes hand+type as a coherent compositor/fire snapshot.
// 0 means inactive; otherwise low byte is hand+1 and upper bits are type.
std::atomic<int> gActivePacked{0};
std::atomic<bool> gSniperFireAvailable{false};
std::atomic<bool> gProjectileFireAvailable{false};
int gCandidateHand = -1;
int gCandidateType = -1;
std::uint64_t gCandidateSince = 0;
std::uint64_t gReleaseSince = 0;
void* gOwner = nullptr;
bool gMissingSymbolsLogged = false;
bool gMissingFireHookLogged = false;

int Pack(int hand, int type) {
    return hand < 0 || type < 0 ? 0 : (type << 8) | (hand + 1);
}
int PackedHand(int packed) { return packed == 0 ? -1 : (packed & 0xff) - 1; }
int PackedType(int packed) { return packed == 0 ? -1 : packed >> 8; }

bool IsScopeWeapon(int type) {
    // Weapon 33 (the country rifle/Mosin replacement in WEAPONS SET ONE) has no
    // optic. It always follows the calibrated barrel/laser. The real sniper,
    // the two launchers and the photo camera (its viewfinder is the Vice City
    // bring-to-face optic) may enter the HMD-centred scope view.
    return type == kSniperType ||
           type == kRpgType || type == kHeatSeekerType ||
           type == kCameraType;
}

float ZoomForWeapon(int type) {
    return type == kSniperType ? kSniperZoom
         : type == kRpgType || type == kHeatSeekerType
                              ? kLauncherZoom
         : type == kCameraType ? kCameraZoom
                               : 1.0f;
}

bool FireRouteAvailable(int type) {
    if (type == kRpgType || type == kHeatSeekerType)
        return gProjectileFireAvailable.load(std::memory_order_acquire);
    // The camera shutter is an independent direct CWeapon::Fire call in
    // vrfire::Update; it has no hook prerequisite.
    if (type == kCameraType) return true;
    return gSniperFireAvailable.load(std::memory_order_acquire);
}

bool CoreReady() {
    return g.FindPlayerPed && g.FindPlayerVehicle && g.CPed_IsAlive &&
           g.CWorld_ProcessLineOfSight && g.CWorld_pIgnoreEntity;
}

int WeaponTypeInSlot(void* ped, int slot) {
    if (!ped || slot <= 0 || slot >= kWeaponSlots) return -1;
    const auto* bytes = static_cast<const std::uint8_t*>(ped);
    return *reinterpret_cast<const std::int32_t*>(
        bytes + kOffWeapons + slot * kWeaponStride);
}

void ResetCandidate() {
    gCandidateHand = -1;
    gCandidateType = -1;
    gCandidateSince = 0;
}

void SetActive(int hand, int type) {
    const int next = Pack(hand, type);
    const int previous = gActivePacked.exchange(next, std::memory_order_acq_rel);
    if (previous == next) return;
    if (next) {
        LOGI("[scope] active hand=%s type=%d zoom=%.1fx",
             hand == 0 ? "LEFT" : "RIGHT", type, ZoomForWeapon(type));
    } else if (previous) {
        LOGI("[scope] released");
    }
}

bool HeadFrame(V3& position, V3& right, V3& up, V3& forward) {
    float p[3]{}, q[4]{};
    if (!xr::GetHeadPose(p, q)) return false;
    position = {p[0], p[1], p[2]};
    right = Normalized(RotateQuaternion(q, {1.0f, 0.0f, 0.0f}));
    up = Normalized(RotateQuaternion(q, {0.0f, 1.0f, 0.0f}));
    forward = Normalized(RotateQuaternion(q, {0.0f, 0.0f, -1.0f}));
    return Finite(position) && LengthSquared(right) > 0.9f &&
           LengthSquared(up) > 0.9f && LengthSquared(forward) > 0.9f;
}

bool TrackingDirectionToWorld(V3 trackingOrigin, V3 trackingDirection,
                              V3& worldOrigin, V3& worldDirection) {
    const V3 trackingEnd = trackingOrigin + trackingDirection;
    float p0[3]{trackingOrigin.x, trackingOrigin.y, trackingOrigin.z};
    float p1[3]{trackingEnd.x, trackingEnd.y, trackingEnd.z};
    float w0[3]{}, w1[3]{};
    if (!vrcam::TrackingPointToWorld(p0, w0) ||
        !vrcam::TrackingPointToWorld(p1, w1)) {
        return false;
    }
    worldOrigin = {w0[0], w0[1], w0[2]};
    worldDirection = Normalized(V3{w1[0], w1[1], w1[2]} - worldOrigin);
    return Finite(worldOrigin) && LengthSquared(worldDirection) > 0.9f;
}

bool CandidateMetrics(void* ped, int hand, V3 head, V3 headRight,
                      V3 headUp, V3 headForward, int& candidateType,
                      float& score) {
    const int slot = physicalweapon::HeldSlot(hand);
    const int type = WeaponTypeInSlot(ped, slot);
    if (!IsScopeWeapon(type) || !FireRouteAvailable(type)) return false;
    // Quest Vice City activates an optic only after the long gun is physically
    // shouldered: the opposite controller must own its support socket.  This
    // prevents one-hand movement near the face from unexpectedly zooming the
    // entire view. The photo camera is one-handed by design — raising it to
    // the eye alone opens the viewfinder (VC behaviour).
    if (type != kCameraType && physicalweapon::SupportHand(hand) < 0)
        return false;

    xr::HandPose hands[physicalweapon::kHandCount]{};
    if (!physicalweapon::GetHandPosesSnapshot(hands) ||
        !hands[hand].valid || !hands[hand].aimValid) {
        return false;
    }
    // Keep the controller position for the shoulder/eye proximity envelope;
    // orientation is checked separately against the calibrated barrel below.
    const V3 weaponPosition{hands[hand].aimPos[0], hands[hand].aimPos[1],
                            hands[hand].aimPos[2]};
    const V3 relative = weaponPosition - head;
    const float distance = Length(relative);
    const float forward = Dot(relative, headForward);
    const float height = Dot(relative, headUp);
    const float lateral = Dot(relative, headRight);

    // Match current Quest VC: the alignment gate follows the same calibrated,
    // model-bound barrel ray as the laser and physical shot. The old raw AIM
    // controller axis let an upward-pointing gun enter scope mode merely
    // because the controller itself remained near the player's face.
    float rayOriginValues[3]{},rayDirectionValues[3]{};
    if (!vrcam::GetWeaponFireRay(hand,type,
                                rayOriginValues,rayDirectionValues)) {
        return false;
    }
    V3 worldHead{},worldHeadForward{};
    if (!TrackingDirectionToWorld(head,headForward,
                                  worldHead,worldHeadForward)) {
        return false;
    }
    const V3 aim=Normalized({rayDirectionValues[0],rayDirectionValues[1],
                             rayDirectionValues[2]});
    const float alignment=Dot(worldHeadForward,aim);

    // Keep the current Quest-VC eye envelope after the support grip has latched.
    if (distance < 0.04f || distance > 0.65f ||
        forward < -0.03f || forward > 0.58f ||
        height < -0.30f || height > 0.14f ||
        std::abs(lateral) > 0.30f || alignment < 0.70f) {
        return false;
    }
    score = distance + std::abs(lateral) * 1.5f +
            (1.0f - alignment) * 0.8f;
    candidateType = type;
    return std::isfinite(score);
}

bool BuildCentreTarget(V3& target) {
    if (!CoreReady() || !vrcam::IsStereoActive()) return false;
    void* const ped = g.FindPlayerPed(-1);
    if (!ped || !g.CPed_IsAlive(ped) ||
        g.FindPlayerVehicle(-1, false)) {
        return false;
    }

    V3 head{}, right{}, up{}, forward{};
    if (!HeadFrame(head, right, up, forward)) return false;
    V3 worldOrigin{}, worldDirection{};
    if (!TrackingDirectionToWorld(head, forward,
                                  worldOrigin, worldDirection)) {
        return false;
    }
    const V3 farTarget = worldOrigin + worldDirection * kCentreRayDistance;
    target = farTarget;

    ColPoint point{};
    void* hitEntity = nullptr;
    void* const savedIgnore = *g.CWorld_pIgnoreEntity;
    *g.CWorld_pIgnoreEntity = ped;
    const GameVec3 origin{worldOrigin.x, worldOrigin.y, worldOrigin.z};
    const GameVec3 end{farTarget.x, farTarget.y, farTarget.z};
    const bool hit = g.CWorld_ProcessLineOfSight(
        &origin, &end, &point, &hitEntity,
        true, true, true, true, true, false, false, false);
    *g.CWorld_pIgnoreEntity = savedIgnore;
    if (hit) target = point.Point();
    return Finite(target);
}

} // namespace

void Reset() {
    SetActive(-1, -1);
    ResetCandidate();
    gReleaseSince = 0;
    gOwner = nullptr;
}

void SetSniperFireAvailable(bool available) {
    gSniperFireAvailable.store(available, std::memory_order_release);
    if (available) gMissingFireHookLogged = false;
    if (!available) Reset();
}

void SetProjectileFireAvailable(bool available) {
    gProjectileFireAvailable.store(available, std::memory_order_release);
    if (available) gMissingFireHookLogged = false;
    if (!available) Reset();
}

void Update(bool interactionsBlocked) {
    // A zoomed centre reticle is only safe when FireSniper is routed through
    // ApplyReticleAim.  If the verified hook is unavailable, fail closed rather
    // than displaying an optic whose native shot can still follow the desktop
    // camera direction.
    if (!gSniperFireAvailable.load(std::memory_order_acquire) &&
        !gProjectileFireAvailable.load(std::memory_order_acquire)) {
        if (!gMissingFireHookLogged) {
            gMissingFireHookLogged = true;
            LOGW("[scope] disabled: no verified physical fire route available");
        }
        Reset();
        return;
    }
    if (!CoreReady()) {
        if (!gMissingSymbolsLogged) {
            gMissingSymbolsLogged = true;
            LOGW("[scope] disabled: required SA world/player symbols missing");
        }
        Reset();
        return;
    }

    void* const ped = g.FindPlayerPed(-1);
    if (interactionsBlocked || !vrcam::IsStereoActive() || !ped ||
        !g.CPed_IsAlive(ped) || g.FindPlayerVehicle(-1, false)) {
        Reset();
        return;
    }
    if (gOwner != ped) {
        Reset();
        gOwner = ped;
    }

    V3 head{}, headRight{}, headUp{}, headForward{};
    if (!HeadFrame(head, headRight, headUp, headForward)) {
        Reset();
        return;
    }

    int candidateHand = -1;
    int candidateType = -1;
    float candidateScore = 1000.0f;
    for (int hand = 0; hand < physicalweapon::kHandCount; ++hand) {
        int type = -1;
        float score = 0.0f;
        if (CandidateMetrics(ped, hand, head, headRight, headUp,
                             headForward, type, score) &&
            score < candidateScore) {
            candidateScore = score;
            candidateHand = hand;
            candidateType = type;
        }
    }

    const std::uint64_t now = NowMs();
    const int active = gActivePacked.load(std::memory_order_acquire);
    const int activeHand = PackedHand(active);
    const int activeType = PackedType(active);
    if (candidateHand >= 0) {
        if (activeHand == candidateHand && activeType == candidateType) {
            gReleaseSince = 0;
            ResetCandidate();
            return;
        }
        if (gCandidateHand != candidateHand ||
            gCandidateType != candidateType) {
            gCandidateHand = candidateHand;
            gCandidateType = candidateType;
            gCandidateSince = now;
        }
        if (gCandidateSince == 0) gCandidateSince = now;
        if (now - gCandidateSince >= kEnterDwellMs) {
            SetActive(candidateHand, candidateType);
            gReleaseSince = 0;
            ResetCandidate();
        }
        return;
    }

    ResetCandidate();
    if (activeHand >= 0) {
        if (gReleaseSince == 0) gReleaseSince = now;
        if (now - gReleaseSince >= kReleaseGraceMs) {
            SetActive(-1, -1);
            gReleaseSince = 0;
        }
    } else {
        gReleaseSince = 0;
    }
}

VisualState Snapshot() {
    const int packed = gActivePacked.load(std::memory_order_acquire);
    const int hand = PackedHand(packed);
    const int type = PackedType(packed);
    return {hand >= 0 && IsScopeWeapon(type), hand, type,
            ZoomForWeapon(type)};
}

bool IsActive() { return Snapshot().active; }

bool IsActiveFor(int hand, int weaponType) {
    const VisualState state = Snapshot();
    return state.active && state.hand == hand && state.weaponType == weaponType;
}

float RenderFovX(float baseFovXDegrees) {
    const VisualState state = Snapshot();
    if (!state.active || !std::isfinite(baseFovXDegrees) ||
        baseFovXDegrees <= 1.0f || baseFovXDegrees >= 179.0f) {
        return baseFovXDegrees;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float halfRadians = baseFovXDegrees * kPi / 360.0f;
    return 2.0f * std::atan(std::tan(halfRadians) / state.zoom) *
           180.0f / kPi;
}

bool ApplyReticleAim(int hand, int weaponType,
                     const float muzzleOrigin[3], float direction[3]) {
    if (!IsActiveFor(hand, weaponType)) return true;
    if (!muzzleOrigin || !direction) return false;

    V3 target{};
    if (!BuildCentreTarget(target)) return false;
    const V3 muzzle{muzzleOrigin[0], muzzleOrigin[1], muzzleOrigin[2]};
    const V3 converged = Normalized(target - muzzle);
    if (!Finite(converged) || LengthSquared(converged) < 0.9f) return false;
    direction[0] = converged.x;
    direction[1] = converged.y;
    direction[2] = converged.z;
    return true;
}

} // namespace savr::scopeaim
