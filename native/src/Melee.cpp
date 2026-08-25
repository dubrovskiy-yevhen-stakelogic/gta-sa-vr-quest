#include "Melee.h"

#include "Log.h"
#include "PhysicalWeapon.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace savr::melee {
namespace {

using GameVec3 = GameSymbols::Vec3;

constexpr int kWeaponSlots = 13;
constexpr int kOffWeapons = 0x730;
constexpr int kWeaponStride = 0x20;
constexpr int kEntityTypeOffset = 0x5A;
constexpr int kEntityVehicle = 2;
constexpr int kEntityPed = 3;
constexpr int kEntityObject = 4;
// 64-bit CEntity flag word (disasm: CEntity::SetIsStatic @0x3aa72c writes
// bit 2 of the qword at +0x28; ObjectDamage tests bUsesCollision as bit 0
// of the same byte).
constexpr int kEntityFlagsOffset = 0x28;
constexpr std::uint64_t kEntityFlagIsStatic = 0x4;
// CPlaceable: matrix pointer at +0x18, inline position at +0x08 when the
// matrix is absent (both already used by Throwable/VrCamera).
constexpr int kEntityMatrixOffset = 0x18;
constexpr int kPlaceablePositionOffset = 0x08;
constexpr int kMatrixPositionOffset = 0x30;
// CObject (disasm: ObjectDamage @0x53a4f4): m_nColDamageEffect byte at
// +0x1A8, CObjectData* at +0x1C4. CObjectData is all scalars, so the PC
// layout holds: m_fUprootLimit at +0x14.
constexpr int kObjectColDamageEffectOffset = 0x1A8;
constexpr int kObjectInfoOffset = 0x1C4;
constexpr int kObjectInfoUprootLimitOffset = 0x14;
constexpr int kColPointSize = 0x2C;
constexpr int kColNormal = 0x10;
constexpr int kColSurfaceB = 0x23;
constexpr int kColPieceB = 0x24;
constexpr int kWeaponInfoDamage = 0x22;
constexpr int kWeaponInfoRadius = 0x60;

struct V3 {
    float x{}, y{}, z{};
};

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

float LengthSquared(V3 v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
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
    const float uv = u.x * v.x + u.y * v.y + u.z * v.z;
    const float uu = LengthSquared(u);
    const V3 cross{u.y * v.z - u.z * v.y,
                   u.z * v.x - u.x * v.z,
                   u.x * v.y - u.y * v.x};
    return u * (2.0f * uv) + v * (s * s - uu) + cross * (2.0f * s);
}

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

struct ColPoint {
    alignas(4) std::uint8_t bytes[kColPointSize]{};

    V3 Point() const {
        V3 point{};
        std::memcpy(&point, bytes, sizeof(point));
        return point;
    }
    V3 Normal() const {
        V3 normal{};
        std::memcpy(&normal, bytes + kColNormal, sizeof(normal));
        return normal;
    }
    unsigned char SurfaceB() const { return bytes[kColSurfaceB]; }
    int PieceB() const { return bytes[kColPieceB]; }
};
static_assert(sizeof(ColPoint) == kColPointSize);

struct Segment {
    int slot{-1};
    int weaponType{-1};
    bool enabled{};
    bool modelBound{};
    V3 root{};
    V3 tip{};
};

struct Motion {
    bool valid{};
    bool armed{};
    int slot{-1};
    int weaponType{-1};
    bool modelBound{};
    V3 previousRoot{};
    V3 previousTip{};
    double previousAt{};
    double calmSince{};
    double lastStrikeAt{};
    bool strikeInProgress{};
    float strikePeakSpeed{};
    double strikeContinueUntil{};
};

Motion gMotion[physicalweapon::kHandCount]{};
bool gMissingSymbolsLogged = false;
void* gMotionOwner = nullptr;

bool CoreReady() {
    return g.FindPlayerPed && g.FindPlayerVehicle &&
           g.CPed_IsAlive &&
           g.CWeaponInfo_GetWeaponInfo && g.CWorld_ProcessLineOfSight &&
           g.CWorld_TestSphereAgainstWorld && g.gaTempSphereColPoints &&
           g.CWorld_pIgnoreEntity && g.CWeapon_GenerateDamageEvent &&
           g.CVehicle_InflictDamage && g.CPed_GetLocalDirection &&
           g.CEntity_GetBoundCentre;
}

int WeaponTypeInSlot(void* ped, int slot) {
    if (!ped || slot < 0 || slot >= kWeaponSlots) return -1;
    const auto* bytes = static_cast<const std::uint8_t*>(ped);
    return *reinterpret_cast<const std::int32_t*>(
        bytes + kOffWeapons + slot * kWeaponStride);
}

bool IsMeleeType(int type) { return type >= 0 && type <= 15; }

int EntityType(void* entity) {
    if (!entity) return 0;
    return *(reinterpret_cast<const std::uint8_t*>(entity) +
             kEntityTypeOffset) & 7;
}

std::uint64_t EntityFlags(void* entity) {
    std::uint64_t flags{};
    std::memcpy(&flags,
                reinterpret_cast<const std::uint8_t*>(entity) +
                    kEntityFlagsOffset,
                sizeof(flags));
    return flags;
}

V3 EntityPosition(void* entity) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(entity);
    void* matrix = nullptr;
    std::memcpy(&matrix, bytes + kEntityMatrixOffset, sizeof(matrix));
    V3 position{};
    if (matrix) {
        std::memcpy(&position,
                    reinterpret_cast<const std::uint8_t*>(matrix) +
                        kMatrixPositionOffset,
                    sizeof(position));
    } else {
        std::memcpy(&position, bytes + kPlaceablePositionOffset,
                    sizeof(position));
    }
    return position;
}

V3 PosePoint(const physicalweapon::TrackingPose& pose, V3 local) {
    const V3 position{pose.position[0], pose.position[1], pose.position[2]};
    const V3 right{pose.right[0], pose.right[1], pose.right[2]};
    const V3 forward{pose.forward[0], pose.forward[1], pose.forward[2]};
    const V3 up{pose.up[0], pose.up[1], pose.up[2]};
    return position + right * local.x + forward * local.y + up * local.z;
}

void MeleeModelSegment(int type, V3& root, V3& tip) {
    // SA's own weapon-streak offsets provide exact visible endpoints for the
    // three long weapons below. The remaining classic models share the same
    // +Z authored shaft convention; chainsaw is the one +X exception.
    root = {0.020f, 0.050f, 0.070f};
    switch (type) {
    case 2:  tip = {-0.054f, 0.0325f, 0.796f}; break; // golf club
    case 3:  tip = { 0.050f, 0.0300f, 0.540f}; break; // nightstick
    case 4:  tip = { 0.100f, 0.0200f, 0.360f}; break; // knife
    case 5:  tip = { 0.246f, 0.0325f, 0.796f}; break; // baseball bat
    case 6:  tip = { 0.050f, 0.0200f, 0.950f}; break; // shovel
    case 7:  tip = { 0.020f, 0.0300f, 1.050f}; break; // pool cue
    case 8:  tip = { 0.096f,-0.0175f, 1.096f}; break; // katana
    case 9:  root = {0.485f, 0.038f, 0.160f};
             tip  = {0.865f, 0.033f, 0.335f}; break; // chainsaw
    case 10: tip = { 0.020f, 0.0300f, 0.430f}; break; // dildo 1
    case 11: tip = { 0.020f, 0.0300f, 0.300f}; break; // dildo 2
    case 12:
    case 13: tip = { 0.020f, 0.0300f, 0.350f}; break; // vibrators
    case 14: tip = { 0.020f, 0.0300f, 0.380f}; break; // flowers
    case 15: tip = { 0.020f, 0.0300f, 0.850f}; break; // cane
    default: tip = { 0.000f, 0.0000f, 0.110f}; break;
    }
}

bool BuildSegment(int hand, void* ped, const xr::HandPose hands[2],
                  Segment& out) {
    if (hand < 0 || hand >= physicalweapon::kHandCount ||
        !hands[hand].valid) {
        return false;
    }

    const int heldSlot = physicalweapon::HeldSlot(hand);
    if (heldSlot > 0) {
        const int type = WeaponTypeInSlot(ped, heldSlot);
        if (!IsMeleeType(type)) return false;
        physicalweapon::TrackingPose pose{};
        if (!physicalweapon::GetHeldVisualPoseTracking(
                hand, heldSlot, &pose)) {
            return false;
        }
        V3 localRoot{}, localTip{};
        MeleeModelSegment(type, localRoot, localTip);
        out.slot = heldSlot;
        out.weaponType = type;
        out.enabled = true;
        out.modelBound = true;
        out.root = PosePoint(pose, localRoot);
        out.tip = PosePoint(pose, localTip);
        return Finite(out.root) && Finite(out.tip);
    }

    // A support controller belongs to the long gun and is never a spare fist.
    if (physicalweapon::IsSupportHand(hand)) return false;

    int fistType = WeaponTypeInSlot(ped, 0);
    if (fistType != 1) fistType = 0; // brass knuckle or plain unarmed
    const xr::HandPose& pose = hands[hand];
    const float* orientation = pose.aimValid ? pose.aimOri : pose.gripOri;
    V3 direction = Normalized(RotateQuaternion(orientation, {0.0f, 0.0f, -1.0f}));
    if (LengthSquared(direction) < 0.5f) return false;
    out.slot = 0;
    out.weaponType = fistType;
    out.enabled = pose.grip >= 0.65f && pose.trigger >= 0.45f;
    out.modelBound = false;
    out.root = {pose.gripPos[0], pose.gripPos[1], pose.gripPos[2]};
    out.tip = out.root + direction * 0.11f;
    return Finite(out.root) && Finite(out.tip);
}

bool ToWorld(V3 tracking, V3& world) {
    const float in[3]{tracking.x, tracking.y, tracking.z};
    float out[3]{};
    if (!vrcam::TrackingPointToWorld(in, out)) return false;
    world = {out[0], out[1], out[2]};
    return Finite(world);
}

GameVec3 ToGame(V3 v) { return {v.x, v.y, v.z}; }

bool ProcessLine(V3 from, V3 to, ColPoint& contact, void*& entity) {
    const GameVec3 a = ToGame(from);
    const GameVec3 b = ToGame(to);
    entity = nullptr;
    return g.CWorld_ProcessLineOfSight(
        &a, &b, contact.bytes, &entity,
        true, true, true, true, true, false, false, false);
}

bool Damageable(void* entity, void* player) {
    if (!entity || entity == player) return false;
    const int type = EntityType(entity);
    if (type == kEntityObject) {
        // Props (gym punching bags first of all) only become melee targets
        // when the object response symbols resolved; otherwise a swing near
        // a bin would consume the strike and visibly do nothing.
        return g.CObject_ObjectDamage && g.CPhysical_ApplyForce;
    }
    return type == kEntityPed || type == kEntityVehicle;
}

bool VisibleFromHandle(V3 root, void* target) {
    GameVec3 centre{};
    g.CEntity_GetBoundCentre(target, &centre);
    ColPoint visibility{};
    void* first = nullptr;
    const bool hit = ProcessLine(root, {centre.x, centre.y, centre.z},
                                 visibility, first);
    return !hit || first == target;
}

float ProbeRadius(int weaponType) {
    if (weaponType <= 1) return 0.10f;
    if (weaponType == 8) return 0.08f;
    float radius = 0.14f;
    if (void* info = g.CWeaponInfo_GetWeaponInfo(
            weaponType, static_cast<signed char>(1))) {
        const float nativeRadius = *reinterpret_cast<const float*>(
            reinterpret_cast<const std::uint8_t*>(info) + kWeaponInfoRadius);
        if (std::isfinite(nativeRadius) && nativeRadius > 0.0f)
            radius = std::clamp(nativeRadius * 0.23f, 0.12f, 0.18f);
    }
    return radius;
}

bool FindContact(void* player, int weaponType,
                 V3 rootStart, V3 rootEnd, V3 tipStart, V3 tipEnd,
                 void*& target, ColPoint& targetContact) {
    target = nullptr;
    const V3 rootTravel = rootEnd - rootStart;
    const V3 tipTravel = tipEnd - tipStart;
    const float travel = std::max(Length(rootTravel), Length(tipTravel));
    const int steps = std::clamp(static_cast<int>(std::ceil(travel / 0.035f)),
                                 1, 24);
    const bool fist = weaponType <= 1;
    const bool katana = weaponType == 8;
    const int shaftSamples = fist ? 2 : 6;
    const float probeRadius = ProbeRadius(weaponType);

    void* const savedIgnore = *g.CWorld_pIgnoreEntity;
    *g.CWorld_pIgnoreEntity = player;

    for (int step = 1; step <= steps; ++step) {
        const float previousT = static_cast<float>(step - 1) / steps;
        const float t = static_cast<float>(step) / steps;
        const V3 previousRoot = rootStart + rootTravel * previousT;
        const V3 previousTip = tipStart + tipTravel * previousT;
        const V3 root = rootStart + rootTravel * t;
        const V3 tip = tipStart + tipTravel * t;

        for (int sample = 0; sample < shaftSamples; ++sample) {
            const float along = katana
                ? static_cast<float>(sample) / (shaftSamples - 1)
                : static_cast<float>(sample + 1) / shaftSamples;
            const V3 previousPoint = previousRoot +
                                     (previousTip - previousRoot) * along;
            const V3 point = root + (tip - root) * along;

            void* sphereEntity = g.CWorld_TestSphereAgainstWorld(
                ToGame(point), probeRadius, player,
                false, true, true, true, false, false);
            if (Damageable(sphereEntity, player)) {
                ColPoint sphereContact{};
                std::memcpy(sphereContact.bytes, g.gaTempSphereColPoints,
                            kColPointSize);
                if (VisibleFromHandle(root, sphereEntity)) {
                    target = sphereEntity;
                    targetContact = sphereContact;
                    *g.CWorld_pIgnoreEntity = savedIgnore;
                    return true;
                }
            }

            ColPoint sweptContact{};
            void* sweptEntity = nullptr;
            if (ProcessLine(previousPoint, point, sweptContact, sweptEntity) &&
                Damageable(sweptEntity, player) &&
                VisibleFromHandle(root, sweptEntity)) {
                target = sweptEntity;
                targetContact = sweptContact;
                *g.CWorld_pIgnoreEntity = savedIgnore;
                return true;
            }
        }

        ColPoint shaftContact{};
        void* shaftEntity = nullptr;
        if (ProcessLine(root, tip, shaftContact, shaftEntity) &&
            Damageable(shaftEntity, player)) {
            target = shaftEntity;
            targetContact = shaftContact;
            *g.CWorld_pIgnoreEntity = savedIgnore;
            return true;
        }
    }

    *g.CWorld_pIgnoreEntity = savedIgnore;
    return false;
}

// Surface-correct contact thud via the native collision audio path (same
// trick the vehicle branch has always used).
void ReportContactThud(void* target, V3 root, V3 impact,
                       const ColPoint& contact) {
    if (!g.AudioEngine || !g.CAudioEngine_ReportBulletHit) return;
    const V3 normal = Normalized(contact.Normal());
    const V3 strike = Normalized(impact - root);
    const float incidence = std::clamp(-(strike.x * normal.x +
                                         strike.y * normal.y +
                                         strike.z * normal.z),
                                        0.0f, 1.0f);
    constexpr float kRadiansToDegrees = 57.2957795131f;
    const float angle = std::asin(incidence) * kRadiansToDegrees;
    GameVec3 soundPosition = ToGame(impact);
    g.CAudioEngine_ReportBulletHit(
        g.AudioEngine, target, contact.SurfaceB(), &soundPosition,
        std::isfinite(angle) ? angle : 45.0f);
}

int DamageForWeapon(int weaponType) {
    int fallback = 10;
    if (weaponType == 1) fallback = 15;
    else if (weaponType == 8) fallback = 30;
    else if (weaponType == 9) fallback = 25;
    else if (weaponType >= 2 && weaponType <= 7) fallback = 20;
    else if (weaponType >= 10 && weaponType <= 15) fallback = 5;

    void* info = g.CWeaponInfo_GetWeaponInfo(
        weaponType, static_cast<signed char>(1));
    if (!info) return fallback;
    const int damage = *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const std::uint8_t*>(info) + kWeaponInfoDamage);
    return damage > 0 ? std::clamp(damage, 1, 200) : fallback;
}

void ApplyContact(void* player, int hand, int weaponType, float speed,
                  V3 root, void* target, const ColPoint& contact) {
    // These strict type checks are part of the ABI boundary: GenerateDamageEvent
    // immediately dereferences CPed-only fields and must never see another class.
    const int type = EntityType(target);
    const int damage = DamageForWeapon(weaponType);
    const V3 impact = contact.Point();
    bool accepted = true;
    int piece = contact.PieceB();

    if (type == kEntityPed) {
        if (piece < 3 || piece > 9) piece = 3; // torso fallback
        int finalDamage = damage;
        // Vice City katana rule: a blade crossing at the head is a head
        // strike. VC decapitates through FireMelee's head flag; SA's damage
        // event with PED_PIECE_HEAD keeps the one-swing kill. The collision
        // piece from the col spheres usually reports torso even on clean
        // head cuts, so test the impact against the live head bone instead.
        if (weaponType == 8 && g.CPed_GetBonePosition) {
            GameSymbols::Vec3 headBone{};
            g.CPed_GetBonePosition(target, &headBone, 5 /* BONE_HEAD */, true);
            const V3 headPos{headBone.x, headBone.y, headBone.z};
            if (std::isfinite(headPos.x) && std::isfinite(headPos.y) &&
                std::isfinite(headPos.z) &&
                Length(impact - headPos) < 0.35f) {
                piece = 9; // PED_PIECE_HEAD
                finalDamage = std::max(damage * 6, 150);
                LOGI("[vr.melee] katana head strike dist=%.2f",
                     Length(impact - headPos));
            }
        }
        GameVec3 centre{};
        g.CEntity_GetBoundCentre(target, &centre);
        const GameSymbols::Vec2 relative{root.x - centre.x,
                                         root.y - centre.y};
        int direction = g.CPed_GetLocalDirection(target, &relative);
        direction = std::clamp(direction, 0, 3);
        accepted = g.CWeapon_GenerateDamageEvent(
            target, player, weaponType, finalDamage, piece, direction);
    } else if (type == kEntityVehicle) {
        g.CVehicle_InflictDamage(target, player, weaponType,
                                 static_cast<float>(damage), ToGame(impact));
        ReportContactThud(target, root, impact, contact);
    } else if (type == kEntityObject) {
        if (!g.CObject_ObjectDamage || !g.CPhysical_ApplyForce) return;
        const auto* objectBytes = static_cast<const std::uint8_t*>(target);

        // Bullet-path rule (CWeapon::FireInstantHit): only objects with no
        // authored uproot limit may be knocked out of static. The gym
        // punching bags qualify; lamp posts and signs keep needing a car.
        float uprootLimit = 1.0f;
        void* objectInfo = nullptr;
        std::memcpy(&objectInfo, objectBytes + kObjectInfoOffset,
                    sizeof(objectInfo));
        if (objectInfo) {
            std::memcpy(&uprootLimit,
                        static_cast<const std::uint8_t*>(objectInfo) +
                            kObjectInfoUprootLimitOffset,
                        sizeof(uprootLimit));
        }
        if ((EntityFlags(target) & kEntityFlagIsStatic) != 0 &&
            std::isfinite(uprootLimit) && uprootLimit <= 0.0f &&
            g.CObject_SetIsStatic && g.CPhysical_AddToMovingList) {
            g.CObject_SetIsStatic(target, false);
            g.CPhysical_AddToMovingList(target);
        }

        if ((EntityFlags(target) & kEntityFlagIsStatic) == 0) {
            // Impulse along the punch. ApplyForce divides by object mass and
            // guards infinite-mass/door flags itself; bullets pass 2.0 here,
            // a punch scales with the tracked hand speed.
            V3 strike = Normalized(impact - root);
            if (LengthSquared(strike) < 0.5f) {
                strike = Normalized(contact.Normal()) * -1.0f;
            }
            const float impulse = std::clamp(0.8f + 0.55f * speed, 1.0f, 4.0f);
            const V3 offset = impact - EntityPosition(target);
            g.CPhysical_ApplyForce(
                target,
                strike.x * impulse, strike.y * impulse, strike.z * impulse,
                offset.x, offset.y, offset.z, true);
        }

        // Native damage keeps smash/break/model-swap semantics. Guarantee the
        // one-punch smash for objects authored as gun-breakable (effect >=
        // 200 is the same threshold FireInstantHit branches on).
        const std::uint8_t colDamageEffect =
            objectBytes[kObjectColDamageEffectOffset];
        const float objectDamage = colDamageEffect >= 200
            ? std::max(static_cast<float>(damage), 151.0f)
            : static_cast<float>(damage);
        GameVec3 fxPosition = ToGame(impact);
        const V3 normal = Normalized(contact.Normal());
        GameVec3 fxDirection{normal.x, normal.y, normal.z};
        g.CObject_ObjectDamage(target, objectDamage, &fxPosition,
                               &fxDirection, player, weaponType);

        // Bullet-hit audio on a prop reads as gunfire. Use the collision
        // report instead - the exact call CJ's body push makes - so the gym
        // bag resolves to its authored punchbag sound and other props to
        // their material thud. Impulse drives the volume; scale it with the
        // punch speed.
        if (g.AudioEngine && g.CAudioEngine_ReportCollision) {
            constexpr unsigned char kSurfacePed = 62; // SURFACE_PED
            GameVec3 soundPosition = ToGame(impact);
            GameVec3 soundNormal{normal.x, normal.y, normal.z};
            const float audioImpulse = std::clamp(0.10f * speed, 0.10f, 0.80f);
            g.CAudioEngine_ReportCollision(
                g.AudioEngine, target, player,
                contact.SurfaceB(), kSurfacePed,
                &soundPosition, &soundNormal,
                audioImpulse, 1.0f, 1, 0);
        } else {
            ReportContactThud(target, root, impact, contact);
        }
    } else {
        return;
    }

    static unsigned hitCount = 0;
    ++hitCount;
    if (hitCount <= 12 || (hitCount % 100) == 0) {
        LOGI("[vr.melee] hit hand=%d type=%d targetType=%d damage=%d "
             "piece=%d speed=%.2f accepted=%d at=(%.2f,%.2f,%.2f)",
             hand, weaponType, type, damage, piece, speed, accepted ? 1 : 0,
             impact.x, impact.y, impact.z);
    }
}

void ResetMotion(Motion& motion) { motion = Motion{}; }

} // namespace

void Reset() {
    for (Motion& motion : gMotion) ResetMotion(motion);
    gMotionOwner = nullptr;
}

void Update(bool interactionsBlocked) {
    if (!CoreReady()) {
        if (!gMissingSymbolsLogged) {
            gMissingSymbolsLogged = true;
            LOGW("[vr.melee] exact SA 2.11 collision/damage symbols missing; disabled");
        }
        Reset();
        return;
    }
    if (interactionsBlocked) {
        Reset();
        return;
    }

    void* player = g.FindPlayerPed(-1);
    if (!player || !g.CPed_IsAlive(player) ||
        g.FindPlayerVehicle(-1, false)) {
        Reset();
        return;
    }
    if (player != gMotionOwner) {
        Reset();
        gMotionOwner = player;
    }

    xr::HandPose hands[physicalweapon::kHandCount]{};
    if (!physicalweapon::GetHandPosesSnapshot(hands)) {
        Reset();
        return;
    }

    const double now = NowSeconds();
    for (int hand = 0; hand < physicalweapon::kHandCount; ++hand) {
        Segment segment{};
        if (!BuildSegment(hand, player, hands, segment)) {
            ResetMotion(gMotion[hand]);
            continue;
        }

        Motion& motion = gMotion[hand];
        const double dt = now - motion.previousAt;
        if (!motion.valid || motion.slot != segment.slot ||
            motion.weaponType != segment.weaponType ||
            motion.modelBound != segment.modelBound || dt <= 0.0 || dt > 0.05) {
            ResetMotion(motion);
            motion.valid = true;
            motion.slot = segment.slot;
            motion.weaponType = segment.weaponType;
            motion.modelBound = segment.modelBound;
            motion.previousRoot = segment.root;
            motion.previousTip = segment.tip;
            motion.previousAt = now;
            motion.calmSince = now;
            continue;
        }

        const V3 tipTravel = segment.tip - motion.previousTip;
        const V3 rootTravel = segment.root - motion.previousRoot;
        const float tipSpeed = Length(tipTravel) / static_cast<float>(dt);
        const float rootSpeed = Length(rootTravel) / static_cast<float>(dt);
        const bool fist = segment.slot == 0;
        const float threshold = fist ? 1.25f : 0.70f;
        const bool calm = fist ? tipSpeed <= 0.48f
                               : tipSpeed <= 0.40f && rootSpeed <= 0.30f;
        if (calm) {
            if (motion.calmSince <= 0.0) motion.calmSince = now;
            if (fist || now - motion.calmSince >= 0.070)
                motion.armed = true;
        } else {
            motion.calmSince = 0.0;
        }
        if (!motion.armed && motion.lastStrikeAt > 0.0 &&
            now - motion.lastStrikeAt >= 0.450) {
            motion.armed = true;
        }

        // Recenter/tracking-loss jumps must never become superhuman attacks.
        if (Length(tipTravel) > 0.65f) {
            motion.armed = false;
            motion.strikeInProgress = false;
            motion.strikePeakSpeed = 0.0f;
            motion.previousRoot = segment.root;
            motion.previousTip = segment.tip;
            motion.previousAt = now;
            continue;
        }

        const V3 previousRelative = motion.previousTip - motion.previousRoot;
        const V3 currentRelative = segment.tip - segment.root;
        const float relativeSpeed = Length(currentRelative - previousRelative) /
                                    static_cast<float>(dt);
        const bool deliberate = fist ? rootSpeed >= 0.75f
                                     : rootSpeed >= 0.25f || relativeSpeed >= 0.50f;
        if (motion.strikeInProgress && now > motion.strikeContinueUntil) {
            motion.strikeInProgress = false;
            motion.strikePeakSpeed = 0.0f;
        }
        const bool cooldownReady = motion.lastStrikeAt <= 0.0 ||
                                   now - motion.lastStrikeAt >= 0.220;
        const bool fastSample = segment.enabled && motion.armed &&
                                tipSpeed >= threshold && deliberate &&
                                cooldownReady;
        if (fastSample) {
            motion.strikeInProgress = true;
            motion.strikePeakSpeed = std::max(motion.strikePeakSpeed, tipSpeed);
            motion.strikeContinueUntil = now + 0.180;
        }
        const bool continuation = motion.strikeInProgress &&
                                  now <= motion.strikeContinueUntil &&
                                  tipSpeed >= 0.08f;

        if (segment.enabled && motion.armed && (fastSample || continuation)) {
            V3 rootStart{}, rootEnd{}, tipStart{}, tipEnd{};
            if (ToWorld(motion.previousRoot, rootStart) &&
                ToWorld(segment.root, rootEnd) &&
                ToWorld(motion.previousTip, tipStart) &&
                ToWorld(segment.tip, tipEnd)) {
                void* target = nullptr;
                ColPoint contact{};
                if (FindContact(player, segment.weaponType,
                                rootStart, rootEnd, tipStart, tipEnd,
                                target, contact)) {
                    ApplyContact(player, hand, segment.weaponType,
                                 std::max(tipSpeed, motion.strikePeakSpeed),
                                 rootEnd, target, contact);
                    // A real contact ends this swing. Misses remain live for the
                    // 180 ms continuation so a stationary player can complete an
                    // arc across a target without repeat-hitting every frame.
                    motion.armed = false;
                    motion.strikeInProgress = false;
                    motion.strikePeakSpeed = 0.0f;
                    motion.strikeContinueUntil = 0.0;
                    motion.lastStrikeAt = now;
                    motion.calmSince = 0.0;
                }
            }
        }

        motion.previousRoot = segment.root;
        motion.previousTip = segment.tip;
        motion.previousAt = now;
    }
}

} // namespace savr::melee
