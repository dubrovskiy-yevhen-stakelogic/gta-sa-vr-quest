#include "VrFire.h"

#include "Driving.h"
#include "Log.h"
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

namespace savr::vrfire {
namespace {

using Vec3 = GameSymbols::Vec3;
using FireInstantHitFn = bool (*)(void*, void*, Vec3*, Vec3*, void*, Vec3*,
                                  Vec3*, bool, bool);
using FireSniperFn = bool (*)(void*, void*, void*, Vec3*);
using PadGetWeaponFn = int (*)(void*, void*, bool);

std::atomic<PhysicalFireQuery> g_physicalFireQuery{nullptr};
std::atomic<bool> g_installed{false};
FireInstantHitFn g_origFireInstantHit{};
FireSniperFn g_origFireSniper{};
PadGetWeaponFn g_origPadGetWeaponBody{};
thread_local bool g_insideHook = false;
std::atomic<bool> g_padTriggerDown[2]{};
std::atomic<bool> g_directTriggerDown[2]{};
std::uint32_t g_directNextFireMs[2]{};
int g_directWeaponType[2]{-1, -1};

constexpr std::uint32_t kFireInstantHitPrologue[4] = {
    0xFC180FEA, 0x6D0123E9, 0xA9027BFD, 0xA9036FFC,
};
constexpr std::uint32_t kFireSniperPrologue[4] = {
    0xD104C3FF, 0x6D0D2BEB, 0x6D0E23E9, 0xA90F7BFD,
};
// GTA SA Android 2.11.311 CPad::GetWeapon. The first 16 bytes are two
// disable checks; the enabled body starts at +0x18. The conditional branches
// make a copied trampoline invalid, so the hook reproduces the two checks and
// calls the untouched body directly.
constexpr std::uint32_t kPadGetWeaponPrologue[4] = {
    0x79422008, 0x35000068, 0x3944BC08, 0x34000068,
};
constexpr std::uintptr_t kPadGetWeaponBodyOffset = 0x18;
constexpr std::uintptr_t kPadDisableControlsOffset = 0x110;
constexpr std::uintptr_t kPadDisableFireOffset = 0x12F;
constexpr std::uintptr_t kPedWeaponsOffset = 0x730;
constexpr std::uintptr_t kPedActiveSlotOffset = 0x8DC;
constexpr std::uintptr_t kWeaponStride = 0x20;
constexpr float kTriggerPress = 0.55f;
constexpr float kTriggerRelease = 0.45f;

void ResetPadTriggerLatch() {
    g_padTriggerDown[0].store(false, std::memory_order_relaxed);
    g_padTriggerDown[1].store(false, std::memory_order_relaxed);
}

bool SupportedHitscan(int type) {
    switch (type) {
    case 22: // pistol
    case 23: // silenced pistol
    case 24: // desert eagle
    case 25: // shotgun
    case 26: // sawed-off shotgun
    case 27: // SPAS-12
    case 28: // micro SMG
    case 29: // MP5
    case 30: // AK-47
    case 31: // M4
    case 32: // Tec-9
    case 33: // country rifle
    case 34: // sniper rifle
    case 38: // minigun
        return true;
    default:
        return false;
    }
}

// Continuous spray weapons whose FireAreaEffect must get an explicit target on
// the physical ray (a null target falls back to ped heading + pad look pitch).
bool AreaEffectWeapon(int type) {
    return type == 37 || type == 41 || type == 42;
}

// All weapons fired by the direct CWeapon::Fire path in Update() below. The
// photo camera (43) joins so its shutter runs under the forced MODE_CAMERA.
bool SupportedDirectFire(int type) {
    return SupportedHitscan(type) || AreaEffectWeapon(type) || type == 43;
}

bool AutomaticHitscan(int type) {
    switch (type) {
    case 28: // micro SMG
    case 29: // MP5
    case 30: // AK-47
    case 31: // M4
    case 32: // Tec-9
    case 38: // minigun
        return true;
    default:
        return false;
    }
}

std::uint32_t PhysicalFireIntervalMs(void* ped, int weaponType) {
    int skill = g.CPed_GetWeaponSkill ? g.CPed_GetWeaponSkill(ped, weaponType) : 1;
    if (skill < 0 || skill > 3) skill = 1;
    void* wi = g.CWeaponInfo_GetWeaponInfo
        ? g.CWeaponInfo_GetWeaponInfo(weaponType, static_cast<signed char>(skill))
        : nullptr;
    if (wi) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(wi);
        const float loopStart = *reinterpret_cast<const float*>(bytes + 0x40);
        const float loopEnd   = *reinterpret_cast<const float*>(bytes + 0x44);
        const float interval = (loopEnd - loopStart) * 900.0f;
        if (std::isfinite(interval) && interval > 0.0f)
            return static_cast<std::uint32_t>(std::clamp(interval, 45.0f, 1100.0f));
    }
    return AutomaticHitscan(weaponType) ? 90u : 280u;
}

void ResetDirectTrigger() {
    for (int hand = 0; hand < 2; ++hand) {
        g_directTriggerDown[hand].store(false, std::memory_order_relaxed);
        g_directNextFireMs[hand] = 0;
        g_directWeaponType[hand] = -1;
    }
}

bool Finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float LengthSquared(Vec3 v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

Vec3 Normalize(Vec3 v) {
    const float lenSq = LengthSquared(v);
    if (!std::isfinite(lenSq) || lenSq < 1.0e-8f) return {};
    const float inv = 1.0f / std::sqrt(lenSq);
    return {v.x * inv, v.y * inv, v.z * inv};
}

bool GetPhysicalRay(void* shooter, int weaponType, Vec3& origin, Vec3& direction,
                    bool* suppressShot = nullptr) {
    if (suppressShot) *suppressShot = false;
    if (!shooter || !SupportedHitscan(weaponType) || !g.FindPlayerPed ||
        shooter != g.FindPlayerPed(-1)) {
        return false;
    }
    const scopeaim::VisualState scope = scopeaim::Snapshot();
    const bool scopedType = scope.active && scope.weaponType == weaponType;
    if (suppressShot) *suppressShot = scopedType;
    // On foot and — Vice City parity — while seated with IMMERSIVE driving:
    // the physically held sidearm aims with the tracked hand there too.
    // DEFAULT driving keeps the vanilla drive-by ownership rules.
    if (!g.FindPlayerVehicle) return false;
    if (g.FindPlayerVehicle(-1, false) != nullptr &&
        driving::GetMode() != driving::MODE_IMMERSIVE)
        return false;

    const PhysicalFireQuery query =
        g_physicalFireQuery.load(std::memory_order_acquire);
    int hand = -1;
    if (!query || !query(weaponType, &hand) || hand < 0 || hand > 1)
        return false;
    const bool scoped = scopedType && scope.hand == hand;
    if (scopedType && !scoped) return false;
    if (suppressShot) *suppressShot = scoped;

    float o[3]{};
    float d[3]{};
    if (!vrcam::GetWeaponFireRay(hand, weaponType, o, d)) {
        if (suppressShot) *suppressShot = scoped;
        return false;
    }
    if (!scopeaim::ApplyReticleAim(hand, weaponType, o, d)) {
        // Never fall through to the native camera/barrel direction while the
        // zoomed reticle is visible. A transient invalid HMD centre ray suppresses
        // this shot, matching the current Quest VC implementation.
        if (suppressShot) *suppressShot = scoped;
        return false;
    }
    origin = {o[0], o[1], o[2]};
    direction = Normalize({d[0], d[1], d[2]});
    const bool valid = Finite(origin) && Finite(direction) &&
                       LengthSquared(direction) > 0.9f;
    return valid;
}

bool GetPhysicalTrigger(void* ped, int& weaponType, int& hand, float& level) {
    if (!ped || !g.FindPlayerPed || ped != g.FindPlayerPed(-1)) return false;
    const bool inVehicle =
        g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false) != nullptr;
    if (inVehicle && driving::GetMode() != driving::MODE_IMMERSIVE)
        return false;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(ped);
    const int slot = static_cast<int>(*reinterpret_cast<const std::int8_t*>(
        bytes + kPedActiveSlotOffset));
    if (slot <= 0 || slot >= 13) return false;
    weaponType = *reinterpret_cast<const std::int32_t*>(
        bytes + kPedWeaponsOffset + static_cast<std::uintptr_t>(slot) * kWeaponStride);

    const PhysicalFireQuery query =
        g_physicalFireQuery.load(std::memory_order_acquire);
    hand = -1;
    if (!query || !query(weaponType, &hand) || hand < 0 || hand > 1)
        return false;

    xr::InputState input{};
    xr::GetInput(input);
    // In a vehicle the triggers ARE accelerator/brake, so — like the Vice
    // City port — the held weapon fires with B instead.
    level = inVehicle ? (input.b ? 1.0f : 0.0f) : input.triggers[hand];
    if (!std::isfinite(level)) return false;
    level = std::clamp(level, 0.0f, 1.0f);
    return true;
}

int OnPadGetWeapon(void* pad, void* ped, bool allowPassive) {
    if (!g_origPadGetWeaponBody || !pad) return 0;

    // These are the exact two gates overwritten at the entry point. Preserve
    // them before entering the untouched +0x18 body and never let VR bypass a
    // cutscene/menu/player-control fire lock.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(pad);
    if (*reinterpret_cast<const std::uint16_t*>(
            bytes + kPadDisableControlsOffset) != 0 ||
        *(bytes + kPadDisableFireOffset) != 0) {
        ResetPadTriggerLatch();
        return 0;
    }

    const int vanilla = g_origPadGetWeaponBody(pad, ped, allowPassive);
    // ProcessPlayerWeapon also calls GetWeapon(nullptr) while maintaining an
    // existing use-gun task. That is still the player pad, so resolve its ped
    // here; otherwise moving while firing can prematurely end the task.
    if (!g.CPad_GetPad || pad != g.CPad_GetPad(0)) return vanilla;
    void* const playerPed = ped ? ped : (g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr);
    int weaponType = 0;
    int hand = -1;
    float trigger = 0.0f;
    if (!GetPhysicalTrigger(playerPed, weaponType, hand, trigger)) {
        ResetPadTriggerLatch();
        // Skydive deploy: while free-falling the game's active weapon is the
        // parachute on CJ's back — nothing is physically held, yet opening it
        // is the pad fire button. Pass the right trigger straight through.
        if (playerPed) {
            const auto* pedBytes =
                reinterpret_cast<const std::uint8_t*>(playerPed);
            const int slot = static_cast<int>(
                *reinterpret_cast<const std::int8_t*>(
                    pedBytes + kPedActiveSlotOffset));
            if (slot > 0 && slot < 13) {
                const int active = *reinterpret_cast<const std::int32_t*>(
                    pedBytes + kPedWeaponsOffset +
                    static_cast<std::uintptr_t>(slot) * kWeaponStride);
                if (active == 46) {
                    xr::InputState in{};
                    xr::GetInput(in);
                    if (in.triggers[1] >= 0.60f) return 255;
                }
            }
        }
        return vanilla;
    }

    // Physical launchers own an independent rising-edge CWeapon::Fire path.
    // Never merge their trigger into the legacy pad camera/task path as well.
    if (weaponType == 16 || weaponType == 17 || weaponType == 18 ||
        weaponType == 35 || weaponType == 36 || weaponType == 39) {
        ResetPadTriggerLatch();
        return 0;
    }

    // Held physical guns, sprays and the camera are fired directly from
    // vrfire::Update. Returning zero here prevents CPlayerPed's use-gun
    // animation task from adding latency or occasionally swallowing the
    // controller press. NPC/unheld guns stay native.
    if (SupportedDirectFire(weaponType)) {
        ResetPadTriggerLatch();
        return 0;
    }

    g_padTriggerDown[1 - hand].store(false, std::memory_order_relaxed);
    const bool wasDown = g_padTriggerDown[hand].load(std::memory_order_relaxed);
    const bool down = wasDown ? trigger >= kTriggerRelease
                              : trigger >= kTriggerPress;
    g_padTriggerDown[hand].store(down, std::memory_order_relaxed);
    if (down && !wasDown) {
        LOGI("[vr.fire] pad trigger routed type=%d hand=%d level=%.2f",
             weaponType, hand, trigger);
    }
    const int vr = down ? 255 : 0;
    return std::max(vanilla, vr);
}

class ScopedLineOptions {
public:
    explicit ScopedLineOptions(void* shooter)
        : ignore_(*g.CWorld_pIgnoreEntity),
          dead_(*g.CWorld_bIncludeDeadPeds),
          tyres_(*g.CWorld_bIncludeCarTyres),
          bikers_(*g.CWorld_bIncludeBikers) {
        *g.CWorld_pIgnoreEntity = shooter;
        *g.CWorld_bIncludeDeadPeds = true;
        *g.CWorld_bIncludeCarTyres = true;
        *g.CWorld_bIncludeBikers = true;
    }

    ~ScopedLineOptions() { Restore(); }

    void Restore() {
        if (!active_) return;
        *g.CWorld_pIgnoreEntity = ignore_;
        *g.CWorld_bIncludeDeadPeds = dead_;
        *g.CWorld_bIncludeCarTyres = tyres_;
        *g.CWorld_bIncludeBikers = bikers_;
        active_ = false;
    }

private:
    void* ignore_{};
    bool dead_{};
    bool tyres_{};
    bool bikers_{};
    bool active_{true};
};

void AddGunShotEvent(void* shooter, int weaponType, Vec3 origin, Vec3 impact) {
    if (!g.CEventGunShot_Construct || !g.CEventGunShot_Destruct ||
        !g.GetEventGlobalGroup || !g.CEventGroup_Add) {
        return;
    }
    void* group = g.GetEventGlobalGroup();
    if (!group) return;

    // 2.11's constructor writes through +0x38. This mirrors the game's stack
    // temporary: CEventGroup::Add clones it before the D1 destructor runs.
    alignas(16) std::uint8_t storage[0x40]{};
    g.CEventGunShot_Construct(storage, shooter, origin, impact,
                              weaponType == 23 /* silenced pistol */);
    g.CEventGroup_Add(group, storage, false);
    g.CEventGunShot_Destruct(storage);
}

// Retail SA shotguns are PELLET weapons: one trigger pull fires a cone of
// pellets and every pellet applies the weapon's damage on its own line test
// (CWeapon::FireInstantHit, 0x73FB10: 15 pellets for the pump/sawn-off,
// 4 for the SPAS-12, spread rate 0.05). The single-ray physical path made
// them hit like a pistol — one pellet per shot.
int PelletsForWeapon(int weaponType) {
    switch (weaponType) {
    case 25: return 15;  // shotgun
    case 26: return 15;  // sawn-off
    case 27: return 4;   // SPAS-12
    default: return 1;
    }
}

float PelletSpreadForWeapon(int weaponType) {
    switch (weaponType) {
    case 25: return 0.05f;
    case 26: return 0.09f;  // sawn-off: noticeably wider cone
    case 27: return 0.05f;
    default: return 0.0f;
    }
}

bool FireControllerRay(void* weapon, void* shooter, int weaponType,
                       Vec3 origin, Vec3 direction) {
    int skill = g.CPed_GetWeaponSkill(shooter, weaponType);
    if (skill < 0 || skill > 3) skill = 1;
    void* wi = g.CWeaponInfo_GetWeaponInfo(weaponType,
                                           static_cast<signed char>(skill));
    if (!wi) return false;

    float range = *reinterpret_cast<const float*>(
        reinterpret_cast<const std::uint8_t*>(wi) + 0x08);
    if (!std::isfinite(range)) return false;
    range = std::clamp(range, 1.0f, 300.0f);
    const Vec3 end{origin.x + direction.x * range,
                   origin.y + direction.y * range,
                   origin.z + direction.z * range};

    // Pellet weapons: derive a stable lateral basis once, then walk every
    // pellet through the same line-test + DoBulletImpact path retail uses,
    // scattering directions inside the spread cone.
    const int pellets = PelletsForWeapon(weaponType);
    const float spread = PelletSpreadForWeapon(weaponType);
    Vec3 lateralA{0.0f, 0.0f, 1.0f};
    Vec3 lateralB{1.0f, 0.0f, 0.0f};
    if (pellets > 1) {
        const Vec3 worldUp{0.0f, 0.0f, 1.0f};
        lateralA = Normalize({direction.y * worldUp.z - direction.z * worldUp.y,
                              direction.z * worldUp.x - direction.x * worldUp.z,
                              direction.x * worldUp.y - direction.y * worldUp.x});
        lateralB = {direction.y * lateralA.z - direction.z * lateralA.y,
                    direction.z * lateralA.x - direction.x * lateralA.z,
                    direction.x * lateralA.y - direction.y * lateralA.x};
        if (!Finite(lateralA) || LengthSquared(lateralA) < 0.5f) {
            lateralA = {0.0f, 0.0f, 1.0f};
            lateralB = {1.0f, 0.0f, 0.0f};
        }
    }
    static std::uint32_t pelletSeed = 0x1234567u;
    auto pelletRand = []() -> float {
        pelletSeed = pelletSeed * 1664525u + 1013904223u;
        return (static_cast<float>((pelletSeed >> 8) & 0xFFFF) / 32768.0f) -
               1.0f;
    };

    bool anyHit = false;
    Vec3 firstImpact = end;
    for (int pellet = 0; pellet < pellets; ++pellet) {
        Vec3 pelletDir = direction;
        if (pellet > 0 && spread > 0.0f) {
            const float ox = pelletRand() * spread;
            const float oy = pelletRand() * spread;
            pelletDir = Normalize({direction.x + lateralA.x * ox + lateralB.x * oy,
                                   direction.y + lateralA.y * ox + lateralB.y * oy,
                                   direction.z + lateralA.z * ox + lateralB.z * oy});
        }
        const Vec3 pelletEnd{origin.x + pelletDir.x * range,
                             origin.y + pelletDir.y * range,
                             origin.z + pelletDir.z * range};

    // CColPoint is 0x2c bytes on this ARM64 build; round up for alignment and
    // zero it so the no-hit path is deterministic.
    alignas(16) std::uint8_t colPoint[0x30]{};
    void* victim = nullptr;
    bool hit = false;
    {
        // Firing from a seat: the muzzle ray starts inside the cabin, so the
        // line test must ignore the OWN vehicle (the stock drive-by does the
        // same), or every shot ends on the windscreen.
        void* ownVehicle = g.FindPlayerVehicle
            ? g.FindPlayerVehicle(-1, false) : nullptr;
        ScopedLineOptions options(ownVehicle ? ownVehicle : shooter);
        hit = g.CWorld_ProcessLineOfSight(
            &origin, &pelletEnd, colPoint, &victim,
            true, true, true, true, true, false, false, true);
        if (hit && victim &&
            ((*reinterpret_cast<const std::uint8_t*>(
                 reinterpret_cast<const std::uint8_t*>(victim) + 0x5A)) & 7) == 2 &&
            g.CWeapon_CheckForShootingVehicleOccupant) {
            g.CWeapon_CheckForShootingVehicleOccupant(
                &victim, colPoint, weaponType, &origin, &pelletEnd);
        }
        // Restore before impact handling; DoBulletImpact can perform unrelated
        // world work and must see the caller's original line-test state.
        options.Restore();
    }

    Vec3 impact = pelletEnd;
    if (hit) {
        impact = *reinterpret_cast<const Vec3*>(colPoint); // m_vecPoint @ +0x00
    }
    if (pellet == 0) firstImpact = impact;
    anyHit = anyHit || hit;

    // Publish the first three pellets as stereo tracers: enough to read the
    // cone without flooding the tracer pool on every SPAS burst.
    if (pellet < 3) {
        const float worldStart[3]{origin.x, origin.y, origin.z};
        const float worldEnd[3]{impact.x, impact.y, impact.z};
        float trackingStart[3]{}, trackingEnd[3]{};
        if (vrcam::WorldPointToTracking(worldStart, trackingStart) &&
            vrcam::WorldPointToTracking(worldEnd, trackingEnd)) {
            xr::AddBulletTracer(trackingStart, trackingEnd, weaponType);
        }
    }

    // This is the vanilla damage/effects path. It handles ped damage events,
    // vehicle damage, tyres, glass, crime, bullet traces and a miss trace.
    // Retail fires it per PELLET for shotguns — that is where their damage
    // comes from — so the loop calls it once per pellet here too.
    g.CWeapon_DoBulletImpact(
        weapon, shooter, hit ? victim : nullptr,
        &origin, &impact, colPoint, 0);
    }  // pellet loop

    AddGunShotEvent(shooter, weaponType, origin, firstImpact);
    g.CWeapon_DoWeaponEffect(weapon, origin, direction);
    const bool hit = anyHit;

    static std::atomic<unsigned> shotCount{0};
    const unsigned n = shotCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 6 || (n % 120) == 0) {
        LOGI("[vr.fire] controller hitscan type=%d pellets=%d hit=%d "
             "origin=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f)",
             weaponType, pellets, hit ? 1 : 0,
             origin.x, origin.y, origin.z,
             direction.x, direction.y, direction.z);
    }
    return true;
}

bool OnFireInstantHit(void* weapon, void* firingEntity,
                      Vec3* origin, Vec3* muzzlePos,
                      void* targetEntity, Vec3* target,
                      Vec3* driveByOrigin, bool arg6, bool muzzle) {
    if (!g_origFireInstantHit) return false;
    if (g_insideHook || !weapon) {
        return g_origFireInstantHit(weapon, firingEntity, origin, muzzlePos,
                                    targetEntity, target, driveByOrigin,
                                    arg6, muzzle);
    }

    const int weaponType = *reinterpret_cast<const std::int32_t*>(weapon);
    Vec3 vrOrigin{};
    Vec3 vrDirection{};
    bool suppressShot = false;
    if (!GetPhysicalRay(firingEntity, weaponType, vrOrigin, vrDirection,
                        &suppressShot)) {
        if (suppressShot) return false;
        return g_origFireInstantHit(weapon, firingEntity, origin, muzzlePos,
                                    targetEntity, target, driveByOrigin,
                                    arg6, muzzle);
    }

    g_insideHook = true;
    const bool fired = FireControllerRay(
        weapon, firingEntity, weaponType, vrOrigin, vrDirection);
    g_insideHook = false;
    if (fired) return true;
    if (suppressShot) return false;
    return g_origFireInstantHit(weapon, firingEntity, origin, muzzlePos,
                                targetEntity, target, driveByOrigin,
                                arg6, muzzle);
}

bool OnFireSniper(void* weapon, void* shooter, void* targetEntity, Vec3* target) {
    if (!g_origFireSniper) return false;
    if (g_insideHook || !weapon) {
        return g_origFireSniper(weapon, shooter, targetEntity, target);
    }

    const int weaponType = *reinterpret_cast<const std::int32_t*>(weapon);
    Vec3 vrOrigin{};
    Vec3 vrDirection{};
    bool suppressShot = false;
    if (weaponType != 34 ||
        !GetPhysicalRay(shooter, weaponType, vrOrigin, vrDirection,
                        &suppressShot)) {
        if (suppressShot) return false;
        return g_origFireSniper(weapon, shooter, targetEntity, target);
    }

    g_insideHook = true;
    const bool fired = FireControllerRay(
        weapon, shooter, weaponType, vrOrigin, vrDirection);
    g_insideHook = false;
    if (fired) return true;
    if (suppressShot) return false;
    return g_origFireSniper(weapon, shooter, targetEntity, target);
}

void* InstallVerifiedTrampoline(void* target, void* replacement,
                                const std::uint32_t expected[4],
                                const char* name) {
    if (!target || !replacement) return nullptr;
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    if (std::memcmp(code, expected, 16) != 0) {
        LOGE("[vr.fire] %s prologue mismatch: %08x %08x %08x %08x; hook refused",
             name, code[0], code[1], code[2], code[3]);
        return nullptr;
    }

    const long pageSizeLong = sysconf(_SC_PAGESIZE);
    if (pageSizeLong <= 0) return nullptr;
    const std::size_t pageSize = static_cast<std::size_t>(pageSizeLong);
    void* trampoline = mmap(nullptr, pageSize,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) {
        LOGE("[vr.fire] mmap trampoline failed for %s", name);
        return nullptr;
    }

    auto* t = reinterpret_cast<std::uint32_t*>(trampoline);
    std::memcpy(t, code, 16);
    t[4] = 0x58000051; // LDR X17, #8
    t[5] = 0xD61F0220; // BR X17
    *reinterpret_cast<void**>(t + 6) =
        reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 32);

    const std::uintptr_t start =
        reinterpret_cast<std::uintptr_t>(code) & ~(pageSize - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(code) + 16 + pageSize - 1) &
        ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("[vr.fire] mprotect failed for %s", name);
        munmap(trampoline, pageSize);
        return nullptr;
    }
    code[0] = 0x58000051; // LDR X17, #8
    code[1] = 0xD61F0220; // BR X17
    *reinterpret_cast<void**>(code + 2) = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    return trampoline;
}

PadGetWeaponFn InstallPadGetWeaponHook(void* target) {
    if (!target) return nullptr;
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    if (std::memcmp(code, kPadGetWeaponPrologue, 16) != 0) {
        LOGE("[vr.fire] CPad::GetWeapon prologue mismatch: "
             "%08x %08x %08x %08x; hook refused",
             code[0], code[1], code[2], code[3]);
        return nullptr;
    }

    const long pageSizeLong = sysconf(_SC_PAGESIZE);
    if (pageSizeLong <= 0) return nullptr;
    const std::size_t pageSize = static_cast<std::size_t>(pageSizeLong);
    const std::uintptr_t start =
        reinterpret_cast<std::uintptr_t>(code) & ~(pageSize - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(code) + 16 + pageSize - 1) &
        ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("[vr.fire] mprotect failed for CPad::GetWeapon");
        return nullptr;
    }

    // The enabled path at +0x18 begins with its normal stack prologue and is
    // untouched. It is safe to call as a function after reproducing the two
    // entry gates in OnPadGetWeapon.
    auto body = reinterpret_cast<PadGetWeaponFn>(
        reinterpret_cast<std::uintptr_t>(target) + kPadGetWeaponBodyOffset);
    code[0] = 0x58000051; // LDR X17, #8
    code[1] = 0xD61F0220; // BR X17
    *reinterpret_cast<void**>(code + 2) = reinterpret_cast<void*>(&OnPadGetWeapon);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    return body;
}

bool CoreSymbolsReady() {
    return g.CWeapon_FireInstantHit && g.CWorld_ProcessLineOfSight &&
           g.CWorld_pIgnoreEntity && g.CWorld_bIncludeDeadPeds &&
           g.CWorld_bIncludeCarTyres && g.CWorld_bIncludeBikers &&
           g.CPed_GetWeaponSkill && g.CWeaponInfo_GetWeaponInfo &&
           g.CWeapon_CheckForShootingVehicleOccupant &&
           g.CWeapon_DoBulletImpact && g.CWeapon_DoWeaponEffect &&
           g.CEventGunShot_Construct && g.CEventGunShot_Destruct &&
           g.GetEventGlobalGroup && g.CEventGroup_Add &&
           g.FindPlayerPed && g.FindPlayerVehicle && g.CPad_GetWeapon;
}

} // namespace

void SetPhysicalFireQuery(PhysicalFireQuery query) {
    g_physicalFireQuery.store(query, std::memory_order_release);
    LOGI("[vr.fire] physical-fire query %s", query ? "registered" : "cleared");
}

void Update(bool blocked) {
    if (blocked || !g_installed.load(std::memory_order_acquire) ||
        !g.CWeapon_Fire || !g.CTimer_m_snTimeInMilliseconds ||
        !g.FindPlayerPed ||
        (g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false) &&
         driving::GetMode() != driving::MODE_IMMERSIVE)) {
        ResetDirectTrigger();
        return;
    }

    void* const ped = g.FindPlayerPed(-1);
    int weaponType = 0;
    int hand = -1;
    float trigger = 0.0f;
    if (!GetPhysicalTrigger(ped, weaponType, hand, trigger) ||
        !SupportedDirectFire(weaponType)) {
        ResetDirectTrigger();
        return;
    }

    const auto* pedBytes = reinterpret_cast<const std::uint8_t*>(ped);
    const int slot = static_cast<int>(*reinterpret_cast<const std::int8_t*>(
        pedBytes + kPedActiveSlotOffset));
    if (slot <= 0 || slot >= 13) {
        ResetDirectTrigger();
        return;
    }
    auto* weapon = const_cast<std::uint8_t*>(pedBytes) + kPedWeaponsOffset +
        static_cast<std::uintptr_t>(slot) * kWeaponStride;
    if (*reinterpret_cast<const std::int32_t*>(weapon) != weaponType) {
        ResetDirectTrigger();
        return;
    }

    g_directTriggerDown[1 - hand].store(false, std::memory_order_relaxed);
    const bool wasDown = g_directTriggerDown[hand].load(std::memory_order_relaxed);
    const bool down = wasDown ? trigger >= kTriggerRelease
                              : trigger >= kTriggerPress;
    g_directTriggerDown[hand].store(down, std::memory_order_relaxed);
    if (!down) {
        g_directNextFireMs[hand] = 0;
        g_directWeaponType[hand] = weaponType;
        return;
    }

    const bool freshPress = !wasDown || g_directWeaponType[hand] != weaponType;
    g_directWeaponType[hand] = weaponType;
    // Sprays keep firing for as long as the trigger is held, like automatics.
    if (!freshPress && !AutomaticHitscan(weaponType) &&
        !AreaEffectWeapon(weaponType))
        return;
    // VC rule: the camera has no hip shutter — it only shoots after the
    // viewfinder is physically at the eye (the scope optic is active).
    if (weaponType == 43 && !scopeaim::IsActiveFor(hand, weaponType)) return;

    const std::uint32_t now = *g.CTimer_m_snTimeInMilliseconds;
    if (!freshPress && now < g_directNextFireMs[hand]) return;

    // READY/FIRING are the only states accepted by CWeapon::Fire. Reloading and
    // out-of-ammo still advance through the native CWeapon::Update lifecycle.
    const std::uint32_t state = *reinterpret_cast<const std::uint32_t*>(weapon + 0x04);
    if (state > 1u) return;

    Vec3 origin{};
    Vec3 direction{};
    if (!GetPhysicalRay(ped, weaponType, origin, direction)) return;

    // The mobile country-rifle owner rejects a null target before reaching its
    // instant-hit path. Supply a physical-ray target for that weapon; the
    // verified FireInstantHit hook remains authoritative for the actual hit.
    Vec3 rifleTarget{origin.x+direction.x*150.0f,
                     origin.y+direction.y*150.0f,
                     origin.z+direction.z*150.0f};
    // Area-effect weapons (flamethrower/spraycan/extinguisher) with a null
    // target derive their direction from the ped heading plus the pad-era
    // look pitch — in VR that sprayed the extinguisher at the ground. A target
    // on the physical ray makes FireAreaEffect use the full 3D controller
    // aim. Keep it short (2m): CShotInfo::AddShot applies spread BEFORE
    // normalising, so a distant point would also flatten the spray cone.
    Vec3 areaTarget{origin.x+direction.x*2.0f,
                    origin.y+direction.y*2.0f,
                    origin.z+direction.z*2.0f};
    Vec3* target=weaponType==33?&rifleTarget:
                 (AreaEffectWeapon(weaponType)?&areaTarget:nullptr);
    // TakePhotograph's only gate on the 2.11 arm64 binary is the active CCam
    // mode halfword == 46 (MODE_CAMERA); disasm @0x6fd03c-0x6fd050 reads
    // TheCamera[+0x5f] as the active index, stride 0x228, mode at +0x186.
    // Our VR viewfinder replaces that camera task, so force the mode only
    // around the shutter call and restore it immediately.
    std::uint16_t* cameraMode = nullptr;
    std::uint16_t savedCameraMode = 0;
    if (weaponType == 43 && g.TheCamera) {
        auto* cameraBytes = reinterpret_cast<std::uint8_t*>(g.TheCamera);
        const unsigned activeCam = cameraBytes[0x5f];
        if (activeCam < 3u) {
            cameraMode = reinterpret_cast<std::uint16_t*>(
                cameraBytes + activeCam * 0x228u + 0x186u);
            savedCameraMode = *cameraMode;
            *cameraMode = 46; // MODE_CAMERA
        }
    }
    const bool fired = g.CWeapon_Fire(
        weapon, ped, &origin, &origin, nullptr, target, nullptr);
    if (cameraMode) *cameraMode = savedCameraMode;
    if (!fired) return;

    g_directNextFireMs[hand] = now + PhysicalFireIntervalMs(ped, weaponType);
    static std::atomic<unsigned> directCount{0};
    const unsigned n = directCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n % 120u) == 0u) {
        LOGI("[vr.fire] direct physical shot type=%d hand=%d next=%u",
             weaponType, hand, g_directNextFireMs[hand]);
    }
}

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    scopeaim::SetSniperFireAvailable(false);
    if (!CoreSymbolsReady()) {
        LOGW("[vr.fire] required 2.11 hitscan symbols missing; hooks disabled");
        return false;
    }

    g_origFireInstantHit = reinterpret_cast<FireInstantHitFn>(
        InstallVerifiedTrampoline(
            reinterpret_cast<void*>(g.CWeapon_FireInstantHit),
            reinterpret_cast<void*>(&OnFireInstantHit),
            kFireInstantHitPrologue, "CWeapon::FireInstantHit"));
    if (!g_origFireInstantHit) return false;

    if (g.CWeapon_FireSniper) {
        g_origFireSniper = reinterpret_cast<FireSniperFn>(
            InstallVerifiedTrampoline(
                reinterpret_cast<void*>(g.CWeapon_FireSniper),
                reinterpret_cast<void*>(&OnFireSniper),
                kFireSniperPrologue, "CWeapon::FireSniper"));
        if (!g_origFireSniper) {
            LOGW("[vr.fire] sniper hook unavailable; type 34 in MODE_SNIPER stays vanilla");
        }
    } else {
        LOGW("[vr.fire] FireSniper symbol missing; type 34 in MODE_SNIPER stays vanilla");
    }

    g_origPadGetWeaponBody = InstallPadGetWeaponHook(
        reinterpret_cast<void*>(g.CPad_GetWeapon));
    if (!g_origPadGetWeaponBody) {
        LOGE("[vr.fire] physical trigger route unavailable; hooks disabled");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    scopeaim::SetSniperFireAvailable(g_origFireSniper != nullptr);
    LOGI("[vr.fire] controller hitscan + pad trigger installed%s "
         "(inactive until physical query)",
         g_origFireSniper ? " + sniper" : "");
    return true;
}

} // namespace savr::vrfire
