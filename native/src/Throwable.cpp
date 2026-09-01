#include "Throwable.h"

#include "Log.h"
#include "PhysicalWeapon.h"
#include "ScopeAim.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace savr::throwable {
namespace {

using Vec3 = GameSymbols::Vec3;
using FireProjectileFn = bool (*)(void*, void*, Vec3*, void*, Vec3*, float);

constexpr std::uint32_t kFireProjectilePrologue[4] = {
    0xD10543FFu, 0x6D0C33EDu, 0x6D0D2BEBu, 0x6D0E23E9u,
};
constexpr int kProjectileCount = 32;
constexpr std::uintptr_t kPhysicalMoveSpeedOffset = 0x68;
constexpr std::uintptr_t kEntityMatrixOffset = 0x18;
constexpr std::uintptr_t kMatrixRightOffset = 0x00;
constexpr std::uintptr_t kMatrixForwardOffset = 0x10;
constexpr std::uintptr_t kMatrixUpOffset = 0x20;
constexpr int kColPointSize = 0x2c;
constexpr float kQuestVcThrowSpeed = 0.46f;
constexpr float kGravityPerTick = 0.008f;
constexpr float kTriggerPress = 0.62f;
constexpr float kTriggerRelease = 0.32f;
constexpr int kTrajectoryPointCount = 41;
constexpr int kRpgType = 35;
constexpr int kHeatSeekerType = 36;
constexpr int kRocketProjectileType = 19;
constexpr int kHeatSeekerProjectileType = 20;
constexpr float kRocketSpeed = 0.35f;

FireProjectileFn g_original{};
std::atomic<bool> g_installed{false};
thread_local bool g_insideHook = false;
thread_local int g_directLauncherHand = -1;
thread_local int g_directThrowableHand = -1;

struct HandThrowState {
    bool armed{};
    bool triggerDown{};
    int slot{-1};
    int weaponType{-1};
};
HandThrowState g_handState[2]{};

bool IsThrowable(int type) {
    return type == 16 || type == 17 || type == 18 || type == 39;
}

bool IsLauncher(int type) {
    return type == kRpgType || type == kHeatSeekerType;
}

int WeaponTypeInHeldSlot(void* ped, int hand);

bool Finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

float LengthSquared(Vec3 value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

Vec3 Normalized(Vec3 value) {
    const float lengthSq = LengthSquared(value);
    if (!std::isfinite(lengthSq) || lengthSq < 1.0e-8f) return {};
    const float inv = 1.0f / std::sqrt(lengthSq);
    return {value.x * inv, value.y * inv, value.z * inv};
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

bool AlignLauncherProjectile(void* projectile, Vec3 launchDirection) {
    if (!projectile) return false;
    launchDirection = Normalized(launchDirection);
    if (!Finite(launchDirection) || LengthSquared(launchDirection) < 0.9f)
        return false;

    // Rockets accelerate along their CMatrix forward axis every update. The
    // allocator seeds this basis from the legacy camera even when move speed is
    // replaced, so rebuild an orthonormal world-space basis around the physical
    // ray. Avoid the world-Z singularity for near-vertical shots.
    const Vec3 worldUp = std::abs(launchDirection.z) < 0.95f
        ? Vec3{0.0f, 0.0f, 1.0f}
        : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 right = Normalized(Cross(launchDirection, worldUp));
    const Vec3 up = Normalized(Cross(right, launchDirection));
    if (!Finite(right) || !Finite(up) || LengthSquared(right) < 0.9f ||
        LengthSquared(up) < 0.9f) {
        return false;
    }

    void* matrix = nullptr;
    std::memcpy(&matrix,
                static_cast<std::uint8_t*>(projectile) + kEntityMatrixOffset,
                sizeof(matrix));
    if (!matrix) return false;
    auto* bytes = static_cast<std::uint8_t*>(matrix);
    // CMatrix axes are 3 floats plus a 4-byte flags/padding lane. Copy only xyz
    // so the engine-owned padding is preserved.
    std::memcpy(bytes + kMatrixRightOffset, &right, sizeof(right));
    std::memcpy(bytes + kMatrixForwardOffset, &launchDirection,
                sizeof(launchDirection));
    std::memcpy(bytes + kMatrixUpOffset, &up, sizeof(up));
    return true;
}

bool BuildLauncherAim(void* player, int type, Vec3& source, Vec3& direction,
                      int& handOut, int requestedHand = -1) {
    if (!player || !g.FindPlayerPed || player != g.FindPlayerPed(-1) ||
        (g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false))) {
        return false;
    }
    const int hand = requestedHand >= 0 ? requestedHand
                                        : physicalweapon::FiringHand();
    if (hand < 0 || hand > 1 || WeaponTypeInHeldSlot(player, hand) != type)
        return false;

    float origin[3]{}, aim[3]{};
    if (!vrcam::GetWeaponFireRay(hand, type, origin, aim) ||
        !scopeaim::ApplyReticleAim(hand, type, origin, aim)) {
        return false;
    }
    source = {origin[0], origin[1], origin[2]};
    direction = Normalized({aim[0], aim[1], aim[2]});
    if (!Finite(source) || !Finite(direction) ||
        LengthSquared(direction) < 0.9f) {
        return false;
    }

    // reference Quest build's launcher path checks both the space between the headset and the
    // physical muzzle and the first half metre in front of the barrel. Suppress
    // a blocked shot instead of spawning a rocket through nearby geometry.
    if (g.CWorld_ProcessLineOfSight && g.CWorld_pIgnoreEntity) {
        alignas(4) std::uint8_t colPoint[kColPointSize]{};
        void* hitEntity = nullptr;
        void* const savedIgnore = *g.CWorld_pIgnoreEntity;
        *g.CWorld_pIgnoreEntity = player;
        bool blocked = false;
        float headTracking[3]{}, headOrientation[4]{}, headWorld[3]{};
        if (xr::GetHeadPose(headTracking, headOrientation) &&
            vrcam::TrackingPointToWorld(headTracking, headWorld)) {
            const Vec3 head{headWorld[0], headWorld[1], headWorld[2]};
            blocked = g.CWorld_ProcessLineOfSight(
                &head, &source, colPoint, &hitEntity,
                true, true, false, true, false, false, false, false);
        }
        if (!blocked) {
            const Vec3 ahead{source.x + direction.x * 0.55f,
                             source.y + direction.y * 0.55f,
                             source.z + direction.z * 0.55f};
            blocked = g.CWorld_ProcessLineOfSight(
                &source, &ahead, colPoint, &hitEntity,
                true, true, false, true, false, false, false, false);
        }
        *g.CWorld_pIgnoreEntity = savedIgnore;
        if (blocked) {
            LOGW("[vr.rocket] blocked at/near physical muzzle; shot suppressed");
            return false;
        }
    }
    handOut = hand;
    return true;
}

int WeaponTypeInHeldSlot(void* ped, int hand) {
    const int slot = physicalweapon::HeldSlot(hand);
    if (!ped || slot <= 0 || slot >= 13) return -1;
    const auto* bytes = static_cast<const std::uint8_t*>(ped);
    return *reinterpret_cast<const std::int32_t*>(
        bytes + 0x730 + static_cast<std::uintptr_t>(slot) * 0x20);
}

void* WeaponInHeldSlot(void* ped, int hand, int& slotOut, int& typeOut) {
    slotOut = physicalweapon::HeldSlot(hand);
    typeOut = -1;
    if (!ped || slotOut <= 0 || slotOut >= 13) return nullptr;
    auto* weapon = static_cast<std::uint8_t*>(ped) + 0x730 +
                   static_cast<std::uintptr_t>(slotOut) * 0x20;
    typeOut = *reinterpret_cast<const std::int32_t*>(weapon);
    return weapon;
}

bool BuildLaunch(void* player, int type, Vec3& source, Vec3& velocity,
                 int& handOut, int requestedHand = -1) {
    if (!player || !g.FindPlayerPed || player != g.FindPlayerPed(-1) ||
        (g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false))) {
        return false;
    }
    const int hand = requestedHand >= 0 ? requestedHand
                                        : physicalweapon::FiringHand();
    if (hand < 0 || hand > 1 || WeaponTypeInHeldSlot(player, hand) != type)
        return false;

    float origin[3]{}, direction[3]{};
    if (!vrcam::GetWeaponFireRay(hand, type, origin, direction)) return false;
    source = {origin[0], origin[1], origin[2]};
    Vec3 forward = Normalized({direction[0], direction[1], direction[2]});
    if (!Finite(source) || !Finite(forward) || LengthSquared(forward) < 0.9f)
        return false;

    // A tracked controller may enter a wall. Clamp the spawn point to the first
    // head-to-hand obstruction exactly like the current Quest VC path.
    if (g.CWorld_ProcessLineOfSight && g.CWorld_pIgnoreEntity) {
        float headTracking[3]{}, headOrientation[4]{};
        float headWorld[3]{};
        if (xr::GetHeadPose(headTracking, headOrientation) &&
            vrcam::TrackingPointToWorld(headTracking, headWorld)) {
            const Vec3 anchor{headWorld[0], headWorld[1], headWorld[2]};
            const Vec3 reach{source.x - anchor.x, source.y - anchor.y,
                             source.z - anchor.z};
            const float distance = std::sqrt(std::max(0.0f, LengthSquared(reach)));
            if (distance > 0.001f) {
                alignas(4) std::uint8_t colPoint[kColPointSize]{};
                void* entity = nullptr;
                void* const savedIgnore = *g.CWorld_pIgnoreEntity;
                *g.CWorld_pIgnoreEntity = player;
                const bool hit = g.CWorld_ProcessLineOfSight(
                    &anchor, &source, colPoint, &entity,
                    true, true, true, true, true, false, false, false);
                *g.CWorld_pIgnoreEntity = savedIgnore;
                if (hit) {
                    Vec3 point{};
                    std::memcpy(&point, colPoint, sizeof(point));
                    const Vec3 unitReach{reach.x / distance, reach.y / distance,
                                         reach.z / distance};
                    const Vec3 hitDelta{point.x - anchor.x, point.y - anchor.y,
                                        point.z - anchor.z};
                    const float projected = hitDelta.x * unitReach.x +
                                            hitDelta.y * unitReach.y +
                                            hitDelta.z * unitReach.z;
                    const float safe = std::max(0.0f, projected - 0.08f);
                    source = {anchor.x + unitReach.x * safe,
                              anchor.y + unitReach.y * safe,
                              anchor.z + unitReach.z * safe};
                }
            }
        }
    }

    velocity = {forward.x * kQuestVcThrowSpeed,
                forward.y * kQuestVcThrowSpeed,
                forward.z * kQuestVcThrowSpeed};
    handOut = hand;
    return true;
}

void ClearPreview(int hand) {
    xr::SetThrowableTrajectory(hand, nullptr, 0, false);
}

bool PublishTrajectory(void* player, int hand, int type,
                       Vec3 source, Vec3 velocity) {
    Vec3 worldPoints[kTrajectoryPointCount]{};
    worldPoints[0] = source;
    int pointCount = 1;
    bool arcHit = false;
    const int segmentCount = type == 17 ? 40 : 30;
    void* const savedIgnore = g.CWorld_pIgnoreEntity
        ? *g.CWorld_pIgnoreEntity : nullptr;
    if (g.CWorld_pIgnoreEntity) *g.CWorld_pIgnoreEntity = player;
    Vec3 previous = source;
    for (int segment = 0; segment < segmentCount; ++segment) {
        const float time = static_cast<float>(segment + 1) * 2.0f;
        Vec3 next{source.x + velocity.x * time,
                  source.y + velocity.y * time,
                  source.z + velocity.z * time -
                      0.5f * kGravityPerTick * time * time};
        if (g.CWorld_ProcessLineOfSight) {
            alignas(4) std::uint8_t colPoint[kColPointSize]{};
            void* entity = nullptr;
            arcHit = g.CWorld_ProcessLineOfSight(
                &previous, &next, colPoint, &entity,
                true, true, true, true, true, false, false, false);
            if (arcHit) std::memcpy(&next, colPoint, sizeof(next));
        }
        worldPoints[pointCount++] = next;
        previous = next;
        if (arcHit) break;
    }
    if (g.CWorld_pIgnoreEntity) *g.CWorld_pIgnoreEntity = savedIgnore;

    float trackingPoints[kTrajectoryPointCount][3]{};
    for (int i = 0; i < pointCount; ++i) {
        const float world[3]{worldPoints[i].x, worldPoints[i].y,
                             worldPoints[i].z};
        if (!vrcam::WorldPointToTracking(world, trackingPoints[i])) {
            ClearPreview(hand);
            return false;
        }
    }
    xr::SetThrowableTrajectory(hand, trackingPoints, pointCount, arcHit);
    return true;
}

void SnapshotProjectiles(void* out[kProjectileCount]) {
    for (int i = 0; i < kProjectileCount; ++i)
        out[i] = g.CProjectileInfo_ms_apProjectile
            ? g.CProjectileInfo_ms_apProjectile[i] : nullptr;
}

void* FindFreshProjectile(void* const before[kProjectileCount]) {
    if (!g.CProjectileInfo_ms_apProjectile) return nullptr;
    for (int i = 0; i < kProjectileCount; ++i) {
        void* const candidate = g.CProjectileInfo_ms_apProjectile[i];
        if (!candidate) continue;
        bool existed = false;
        for (int j = 0; j < kProjectileCount; ++j) {
            if (before[j] == candidate) { existed = true; break; }
        }
        if (!existed) return candidate;
    }
    return nullptr;
}

bool FireTrackedRelease(void* weapon, void* player, int hand, int slot,
                        int type, Vec3 source, Vec3 velocity) {
    if (!weapon || !player || !g.CWeapon_Fire || !IsThrowable(type))
        return false;
    const auto* bytes = static_cast<const std::uint8_t*>(weapon);
    // This independent release replaces ProcessPlayerWeapon's call site, so it
    // must preserve the same READY gate. CWeapon::Fire remains the sole owner of
    // ammo, reload state, cooldown, audio and satchel/detonator progression.
    if (*reinterpret_cast<const std::uint32_t*>(bytes + 0x04) != 0)
        return false;
    const Vec3 direction = Normalized(velocity);
    if (!Finite(source) || !Finite(direction) ||
        LengthSquared(direction) < 0.9f) {
        return false;
    }
    // Mobile SA's local throwable branch assumes targetPosn is non-null in the
    // full CWeapon owner. It is used only to derive native throw power; the hook
    // below replaces the resulting projectile source and velocity atomically.
    Vec3 target{source.x + direction.x * 10.0f,
                source.y + direction.y * 10.0f,
                source.z + direction.z * 10.0f};
    g_directThrowableHand = hand;
    const bool fired = g.CWeapon_Fire(weapon, player, &source, &source,
                                      nullptr, &target, nullptr);
    g_directThrowableHand = -1;
    if (!fired) return false;

    physicalweapon::ReleaseAfterUse(hand, slot);
    // SA hands out the detonator (slot 12) with the first thrown satchel
    // inside CWeapon::Fire. Vice City parity: it materialises straight in a
    // free hand, ready to blow the charge with the trigger.
    if (type == 39) {
        int detonatorHand = -1;
        if (physicalweapon::HeldSlot(1 - hand) < 0) detonatorHand = 1 - hand;
        else if (physicalweapon::HeldSlot(hand) < 0) detonatorHand = hand;
        if (detonatorHand >= 0 &&
            physicalweapon::ForceHold(detonatorHand, 12))
            LOGI("[vr.throw] detonator -> %s hand",
                 detonatorHand == 0 ? "LEFT" : "RIGHT");
    }
    const auto* totalAmmo = reinterpret_cast<const std::uint32_t*>(bytes + 0x0c);
    LOGI("[vr.throw] RELEASE type=%d hand=%s source=(%.2f,%.2f,%.2f) "
         "velocity=(%.2f,%.2f,%.2f) nativeAmmo=%u",
         type, hand == 0 ? "LEFT" : "RIGHT", source.x, source.y, source.z,
         velocity.x, velocity.y, velocity.z, *totalAmmo);
    return true;
}

bool FireTrackedLauncher(void* weapon, void* player, int hand, int type) {
    if (!weapon || !player || !g.CWeapon_Fire || !IsLauncher(type))
        return false;
    // ProcessPlayerWeapon normally enforces this before calling CWeapon::Fire.
    // Our independent trigger edge must reproduce the gate because Fire itself
    // also accepts WEAPONSTATE_FIRING and would otherwise allow rapid re-presses
    // to bypass the launcher's native cadence.
    if (*reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(weapon) + 0x04) != 0) {
        return false; // WEAPONSTATE_READY
    }
    Vec3 source{}, direction{};
    int launchHand = -1;
    if (!BuildLauncherAim(player, type, source, direction,
                          launchHand, hand)) {
        return false;
    }

    // Call the full native weapon owner, not the allocator: CWeapon::Fire keeps
    // SA's ammo, cooldown, state and launch audio. Its synchronous
    // FireProjectile call enters OnFireProjectile below, where the legacy
    // camera gate is replaced by the already-validated physical launcher ray.
    g_directLauncherHand = hand;
    const bool fired = g.CWeapon_Fire(weapon, player, &source, &source,
                                      nullptr, nullptr, nullptr);
    g_directLauncherHand = -1;
    if (fired) {
        LOGI("[vr.rocket] rising-edge native fire type=%d hand=%s",
             type, launchHand == 0 ? "LEFT" : "RIGHT");
    }
    return fired;
}

bool OnFireProjectile(void* weapon, void* shooter, Vec3* origin,
                      void* targetEntity, Vec3* target, float force) {
    if (!g_original) return false;
    if (g_insideHook || !weapon || !shooter || !origin) {
        return g_original(weapon, shooter, origin, targetEntity, target, force);
    }
    const int type = *reinterpret_cast<const std::int32_t*>(weapon);
    if (IsLauncher(type)) {
        const int firingHand = g_directLauncherHand >= 0
            ? g_directLauncherHand : physicalweapon::FiringHand();
        const bool localHeld = g.FindPlayerPed &&
            shooter == g.FindPlayerPed(-1) && firingHand >= 0 && firingHand < 2 &&
            WeaponTypeInHeldSlot(shooter, firingHand) == type;
        if (!localHeld)
            return g_original(weapon, shooter, origin, targetEntity, target, force);

        Vec3 launchSource{}, launchDirection{};
        int hand = -1;
        if (!g.CProjectileInfo_AddProjectile ||
            !BuildLauncherAim(shooter, type, launchSource,
                              launchDirection, hand, firingHand)) {
            // Never fall back to stock FireProjectile for a physically-held
            // launcher: it either rejects the shot outside legacy camera mode or
            // launches along the desktop camera rather than the visible optic.
            return false;
        }
        Vec3 velocity{launchDirection.x * kRocketSpeed,
                      launchDirection.y * kRocketSpeed,
                      launchDirection.z * kRocketSpeed};
        const bool hasHeatTarget = type == kHeatSeekerType && targetEntity;
        const int projectileType = hasHeatTarget
            ? kHeatSeekerProjectileType : kRocketProjectileType;
        void* before[kProjectileCount]{};
        SnapshotProjectiles(before);
        const bool fired = g.CProjectileInfo_AddProjectile(
            shooter, projectileType, launchSource, force, &velocity,
            hasHeatTarget ? targetEntity : nullptr);
        if (fired) {
            // SA 2.11 ignores AddProjectile's direction argument for rockets
            // fired by the player and seeds CPhysical::m_vecMoveSpeed from the
            // legacy camera instead. Identify only the newly-created object and
            // replace that velocity with the physical/reticle ray, matching the
            // verified throwable rewrite without touching any existing rocket.
            void* const projectile = FindFreshProjectile(before);
            if (!projectile) {
                LOGE("[vr.rocket] allocator succeeded but fresh projectile was not identifiable");
                return true;
            }
            std::memcpy(static_cast<std::uint8_t*>(projectile) +
                            kPhysicalMoveSpeedOffset,
                        &velocity, sizeof(velocity));
            if (!AlignLauncherProjectile(projectile, launchDirection)) {
                LOGE("[vr.rocket] fresh projectile has no writable physical matrix basis");
            }
            LOGI("[vr.rocket] type=%d projectile=%d hand=%s source=(%.2f,%.2f,%.2f) "
                 "direction=(%.2f,%.2f,%.2f) object=%p",
                 type, projectileType, hand == 0 ? "LEFT" : "RIGHT",
                 launchSource.x, launchSource.y, launchSource.z,
                 launchDirection.x, launchDirection.y, launchDirection.z,
                 projectile);
        }
        return fired;
    }
    if (!IsThrowable(type))
        return g_original(weapon, shooter, origin, targetEntity, target, force);

    Vec3 launchSource{}, launchVelocity{};
    int hand = -1;
    const int requestedHand = g_directThrowableHand >= 0
        ? g_directThrowableHand : -1;
    if (!BuildLaunch(shooter, type, launchSource, launchVelocity, hand,
                     requestedHand))
        return g_original(weapon, shooter, origin, targetEntity, target, force);

    void* before[kProjectileCount]{};
    SnapshotProjectiles(before);
    g_insideHook = true;
    const bool fired = g_original(weapon, shooter, &launchSource,
                                  targetEntity, target, force);
    g_insideHook = false;
    if (!fired) return false;

    if (void* projectile = FindFreshProjectile(before)) {
        std::memcpy(static_cast<std::uint8_t*>(projectile) +
                        kPhysicalMoveSpeedOffset,
                    &launchVelocity, sizeof(launchVelocity));
        LOGI("[vr.throw] type=%d hand=%s source=(%.2f,%.2f,%.2f) "
             "velocity=(%.2f,%.2f,%.2f) projectile=%p",
             type, hand == 0 ? "LEFT" : "RIGHT",
             launchSource.x, launchSource.y, launchSource.z,
             launchVelocity.x, launchVelocity.y, launchVelocity.z,
             projectile);
    } else {
        // The native call succeeded but did not expose a new pointer. Keep its
        // safe native throw rather than touching an ambiguous existing object.
        LOGW("[vr.throw] type=%d fired but new projectile was not identifiable", type);
    }
    return true;
}

void* InstallVerifiedTrampoline(void* target, void* replacement) {
    if (!target || !replacement) return nullptr;
    auto* code = static_cast<std::uint32_t*>(target);
    if (std::memcmp(code, kFireProjectilePrologue,
                    sizeof(kFireProjectilePrologue)) != 0) {
        LOGE("[vr.throw] FireProjectile prologue mismatch: %08x %08x %08x %08x",
             code[0], code[1], code[2], code[3]);
        return nullptr;
    }
    const long pageSizeLong = sysconf(_SC_PAGESIZE);
    if (pageSizeLong <= 0) return nullptr;
    const std::size_t pageSize = static_cast<std::size_t>(pageSizeLong);
    void* trampoline = mmap(nullptr, pageSize,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return nullptr;
    auto* copy = static_cast<std::uint32_t*>(trampoline);
    std::memcpy(copy, code, 16);
    copy[4] = 0x58000051u; // LDR X17,#8
    copy[5] = 0xD61F0220u; // BR X17
    *reinterpret_cast<void**>(copy + 6) =
        static_cast<std::uint8_t*>(target) + 16;
    __builtin___clear_cache(reinterpret_cast<char*>(copy),
                            reinterpret_cast<char*>(copy) + 32);

    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(code) &
                                 ~(pageSize - 1);
    const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(code) + 16 +
                                pageSize - 1) & ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(trampoline, pageSize);
        return nullptr;
    }
    code[0] = 0x58000051u;
    code[1] = 0xD61F0220u;
    *reinterpret_cast<void**>(code + 2) = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    return trampoline;
}

} // namespace

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    scopeaim::SetProjectileFireAvailable(false);
    if (!g.CWeapon_Fire || !g.CWeapon_FireProjectile ||
        !g.CProjectileInfo_ms_apProjectile ||
        !g.CProjectileInfo_AddProjectile || !g.FindPlayerPed) {
        LOGW("[vr.throw] required SA 2.11 projectile symbols missing; disabled");
        return false;
    }
    g_original = reinterpret_cast<FireProjectileFn>(InstallVerifiedTrampoline(
        reinterpret_cast<void*>(g.CWeapon_FireProjectile),
        reinterpret_cast<void*>(&OnFireProjectile)));
    if (!g_original) return false;
    g_installed.store(true, std::memory_order_release);
    scopeaim::SetProjectileFireAvailable(true);
    LOGI("[vr.throw] physical Quest-VC throwable/launcher hook installed");
    return true;
}

void Update(bool interactionsBlocked) {
    xr::InputState input{};
    xr::GetInput(input);
    void* const player = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    const bool unavailable = interactionsBlocked || !player ||
        !g_installed.load(std::memory_order_acquire) ||
        (g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false));

    for (int hand = 0; hand < 2; ++hand) {
        HandThrowState& state = g_handState[hand];
        const float level = std::isfinite(input.triggers[hand])
            ? std::clamp(input.triggers[hand], 0.0f, 1.0f) : 0.0f;
        const bool triggerDown = state.triggerDown
            ? level >= kTriggerRelease : level >= kTriggerPress;

        int slot = -1, type = -1;
        void* const weapon = unavailable
            ? nullptr : WeaponInHeldSlot(player, hand, slot, type);
        const bool validThrowable = weapon && IsThrowable(type);
        const bool validLauncher = weapon && IsLauncher(type);
        if (unavailable || (!validThrowable && !validLauncher)) {
            state.armed = false;
            state.triggerDown = triggerDown;
            state.slot = slot;
            state.weaponType = type;
            ClearPreview(hand);
            continue;
        }

        if (state.slot != slot || state.weaponType != type) {
            // A trigger already held while a grenade is grabbed is not a fresh
            // arming press. This also prevents a menu/weapon switch from throwing.
            state = {};
            state.slot = slot;
            state.weaponType = type;
            state.triggerDown = triggerDown;
            ClearPreview(hand);
            continue;
        }

        const bool justPressed = triggerDown && !state.triggerDown;
        const bool justReleased = !triggerDown && state.triggerDown;
        if (validLauncher) {
            state.armed = false;
            ClearPreview(hand);
            if (justPressed)
                FireTrackedLauncher(weapon, player, hand, type);
            state.triggerDown = triggerDown;
            continue;
        }
        if (justPressed) {
            state.armed = true;
            LOGI("[vr.throw] ARMED type=%d hand=%s", type,
                 hand == 0 ? "LEFT" : "RIGHT");
        }

        if (state.armed && triggerDown) {
            Vec3 source{}, velocity{};
            int launchHand = -1;
            if (BuildLaunch(player, type, source, velocity, launchHand, hand))
                PublishTrajectory(player, hand, type, source, velocity);
            else
                ClearPreview(hand);
        } else {
            ClearPreview(hand);
        }

        if (state.armed && justReleased) {
            Vec3 source{}, velocity{};
            int launchHand = -1;
            if (BuildLaunch(player, type, source, velocity, launchHand, hand))
                FireTrackedRelease(weapon, player, hand, slot, type,
                                   source, velocity);
            state.armed = false;
            ClearPreview(hand);
        }
        state.triggerDown = triggerDown;
    }
}

void Reset() {
    for (int hand = 0; hand < 2; ++hand) {
        g_handState[hand] = {};
        ClearPreview(hand);
    }
}

} // namespace savr::throwable
