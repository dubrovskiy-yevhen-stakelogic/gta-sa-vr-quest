// Custom immersive basketball (see Basketball.h).
//
// Engine pieces, all disasm-verified on GTA:SA 2.11.311 arm64:
//   CObject::Create(int, bool)  @0x539754  — plain object factory
//   CWorld::Add                 @0x4c7db8  — world registration
//   CObject::Teleport           @0x53c718  — sector-correct reposition
//   CEntity matrix ptr @+0x18 (pos @+0x30), model id @+0x32
//   CPhysical move speed @+0x68..0x73, turn speed @+0x74..0x7F
//   (ApplyForce @0x4a12a8 writes the move vector at +0x68/+0x70)
#include "Basketball.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

#include "Log.h"
#include "PhysicalWeapon.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

namespace savr::basketball {
namespace {

constexpr int   kBallModel        = 3120;   // BBALL_ingame
constexpr float kGrabGrip         = 0.55f;
constexpr float kReleaseGrip      = 0.35f;
// Metres per frame per (metre per second): SA physics integrates moveSpeed
// once per 50 Hz logic frame.

// --- user-calibratable physics (see Basketball.h) --------------------------
struct PhysicsSetting {
    const char* name;
    int         defaultValue;
    int         minimum;
    int         maximum;
    int         step;
};
// Deliberately near-unbounded: the user calibrates by feel; only hard
// physical absurdities (negative values) are excluded.
constexpr PhysicsSetting kPhysicsSettings[PHYS_FIELD_COUNT] = {
    {"BOUNCE",        96, 0, 500,  2},   // percent kept per bounce
    {"GROUND GRIP",   96, 0, 500,  1},   // horizontal keep per bounce
    {"THROW POWER",  135, 0, 2000, 5},   // percent of hand speed
    {"THROW MAX",     90, 1, 2000, 10},  // dm/s
    {"MAGNET RANGE", 160, 0, 2000, 10},  // cm
    {"MAGNET SPEED",  12, 1, 200,  1},   // cm per frame
    {"GRAB RANGE",    50, 1, 500,  5},   // cm
    {"BALL RADIUS",   12, 1, 100,  1},   // cm
    {"HIT POWER",    130, 0, 2000, 5},   // percent of hand speed
    {"HIT RANGE",     28, 1, 200,  2},
    {"CASUAL THROW",   1, 0,   1,  1},   // 1 = trigger aim-arc throw
    {"CASUAL SPEED",  90, 10, 300,  5},   // arc speed, dm/s
    {"CASUAL PRESSURE",0, 0,   1,  1},   // arc speed follows trigger press
    {"SHOW PHYSICS",   0, 0,   1,  1},   // draw rim collider + ball circle
    {"AUTO RETURN",    0, 0,   1,  1},   // opt-in: empty-hand trigger recalls ball
    // Rim calibration: nudge with SHOW PHYSICS on until the drawn ring sits
    // on the visible metal. One calibration fixes EVERY hoop - the offset is
    // applied along each stand's own IPL facing.
    {"RIM FORWARD",   50, -200, 200, 2},  // cm along facing
    {"RIM HEIGHT",   -60, -200, 200, 2},  // cm vertical
    {"RIM SIDE",      -8, -200, 200, 2},  // cm sideways (perp to facing)
    {"RIM RADIUS",    25, 5,  100,  1},   // cm
    {"BULLET DEFLATE", 1, 0,   1,  1},   // 0 = shots only shove the ball
};
std::atomic<int> g_physics[PHYS_FIELD_COUNT] = {
    kPhysicsSettings[0].defaultValue, kPhysicsSettings[1].defaultValue,
    kPhysicsSettings[2].defaultValue, kPhysicsSettings[3].defaultValue,
    kPhysicsSettings[4].defaultValue, kPhysicsSettings[5].defaultValue,
    kPhysicsSettings[6].defaultValue, kPhysicsSettings[7].defaultValue,
    kPhysicsSettings[8].defaultValue, kPhysicsSettings[9].defaultValue,
    kPhysicsSettings[10].defaultValue, kPhysicsSettings[11].defaultValue,
    kPhysicsSettings[12].defaultValue, kPhysicsSettings[13].defaultValue,
    kPhysicsSettings[14].defaultValue, kPhysicsSettings[15].defaultValue,
    kPhysicsSettings[16].defaultValue, kPhysicsSettings[17].defaultValue,
    kPhysicsSettings[18].defaultValue, kPhysicsSettings[19].defaultValue,
};
std::atomic<bool> g_physicsLoaded{false};
struct HandCalibSetting {
    const char* name;
    int minimum;
    int maximum;
    int step;
};
constexpr HandCalibSetting kHandCalibSettings[HAND_CALIB_FIELD_COUNT] = {
    {"HAND X",     -300, 300, 1},
    {"HAND Y",     -300, 300, 1},
    {"HAND Z",     -300, 300, 1},
    {"HAND PITCH", -180, 180, 1},
    {"HAND YAW",   -180, 180, 1},
    {"HAND ROLL",  -180, 180, 1},
};
std::atomic<int> g_handCalib[HAND_CALIB_FIELD_COUNT]{};
std::atomic<bool> g_handCalibrationActive{false};
const char* const kPhysicsIniPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_basketball.ini";

void SavePhysics() {
    if (FILE* file = std::fopen(kPhysicsIniPath, "w")) {
        for (int i = 0; i < PHYS_FIELD_COUNT; ++i)
            std::fprintf(file, "%s=%d\n", kPhysicsSettings[i].name,
                         g_physics[i].load());
        for (int i = 0; i < HAND_CALIB_FIELD_COUNT; ++i)
            std::fprintf(file, "%s=%d\n", kHandCalibSettings[i].name,
                         g_handCalib[i].load());
        std::fclose(file);
    }
}

void LoadPhysicsOnce() {
    if (g_physicsLoaded.exchange(true)) return;
    if (FILE* file = std::fopen(kPhysicsIniPath, "r")) {
        char line[96];
        while (std::fgets(line, sizeof(line), file)) {
            char key[64];
            int value;
            if (std::sscanf(line, "%63[^=]=%d", key, &value) != 2) continue;
            for (int i = 0; i < PHYS_FIELD_COUNT; ++i) {
                if (std::strcmp(key, kPhysicsSettings[i].name) == 0) {
                    g_physics[i].store(std::clamp(
                        value, kPhysicsSettings[i].minimum,
                        kPhysicsSettings[i].maximum));
                }
            }
            for (int i = 0; i < HAND_CALIB_FIELD_COUNT; ++i) {
                if (std::strcmp(key, kHandCalibSettings[i].name) == 0) {
                    g_handCalib[i].store(std::clamp(
                        value, kHandCalibSettings[i].minimum,
                        kHandCalibSettings[i].maximum));
                }
            }
        }
        std::fclose(file);
    }
}

float Phys(int field, float scale) {
    return static_cast<float>(g_physics[field].load(
               std::memory_order_relaxed)) * scale;
}

constexpr int kEntityMatrixOffset = 0x18;
constexpr int kMatrixPosOffset    = 0x30;
constexpr int kEntityModelOffset  = 0x32;
constexpr int kMoveSpeedOffset    = 0x68;
constexpr int kTurnSpeedOffset    = 0x74;

// Manual floor physics: the mobile build ships no working collision for the
// ball model (field telemetry: freefall straight through the map), so the
// floor bounce is ours. Tunables live in kPhysicsSettings / the VR menu;
// the engine keeps applying gravity and integrating the velocity we set.

struct V3 {
    float x{}, y{}, z{};
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(V3 v) {
    return std::sqrt(std::max(0.0f, v.x * v.x + v.y * v.y + v.z * v.z));
}
bool Finite(V3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

using CreateObjectFn = void* (*)(int modelId, bool temporary);
using WorldAddFn     = void (*)(void* entity);
// CWorld::ProcessLineOfSight - real collision raycast. Replaces the
// ground-column wall hack, which saw ROOF tops as walls (the ball bounced
// metres before a building at roof-footprint level) and knew nothing about
// the backboard plank or fences. CColPoint layout (no pointers, identical
// on arm64): point @0x00, normal @0x10, 0x2C bytes.
// CWaterLevel::GetWaterLevel(x, y, z, &outLevel, withWaves, outNormal) -
// true where water exists. Buoyancy uses the wave-modulated level, so the
// floating ball bobs on the swell for free.
using GetWaterLevelFn = bool (*)(float x, float y, float z, float* outLevel,
                                 bool withWaves, void* outNormal);

using ProcessLineOfSightFn = bool (*)(
    const float* origin, const float* target, void* outColPoint,
    void** outEntity, bool buildings, bool vehicles, bool peds,
    bool objects, bool dummies, bool doSeeThroughCheck,
    bool doCameraIgnoreCheck, bool doShootThroughCheck);

using TeleportFn     = void (*)(void* object, float x, float y, float z,
                                unsigned char resetRotation);
using LoadAllFn      = void (*)(bool priorityOnly);

CreateObjectFn g_createObject = nullptr;
WorldAddFn     g_worldAdd     = nullptr;
TeleportFn     g_teleport     = nullptr;
ProcessLineOfSightFn g_processLineOfSight = nullptr;
GetWaterLevelFn      g_getWaterLevel      = nullptr;
LoadAllFn      g_loadAll      = nullptr;
// CPools::ms_pObjectPool — used for HONEST liveness validation of the ball
// pointer (the population/garbage systems can delete objects behind us; a
// stale pointer crashed the first field test). Pool layout (disasm
// CObject::Create @0x539754): +0 objects array (stride 0x220), +8 ref
// bytes (bit7 = slot free), +0x10 capacity.
void** g_objectPool = nullptr;
using CreateRwFn = void (*)(void* object);
CreateRwFn g_createRw = nullptr;

void* g_ball = nullptr;
int   g_ballSlot = -1;
std::uint8_t g_ballRef = 0;
int   g_heldHand = -1;
// The held ball is a rigid child of the primary controller.  The old visual
// scale helper rebuilt an identity matrix before every Teleport, so the ball
// followed the hand position but its texture never rotated with the wrist.
struct BallVisualBasis {
    V3 right{1.0f, 0.0f, 0.0f};
    V3 forward{0.0f, 1.0f, 0.0f};
    V3 up{0.0f, 0.0f, 1.0f};
    bool valid{};
};
BallVisualBasis g_heldBallBasis{};
// Once the support palm touches the ball, keep that visual contact rigid in
// the primary controller's local frame. Re-evaluating the support controller
// independently every frame made its rendered hand skate over the sphere.
struct BallSupportContact {
    bool valid{};
    int primary{-1};
    int support{-1};
    V3 localPosition{};
    V3 localUp{};
    V3 localForward{};
};
BallSupportContact g_ballSupportContact{};
// Post-throw no-touch window per hand: the follow-through kept "hitting"
// the just-thrown ball with a decelerating hand, killing every throw.
double g_noTouchUntil[2]{};
// Grab INTENT: magnet/grab require a fresh grip press (edge), not a resting
// squeeze - without this the ball boomeranged straight back into the hand
// right after every throw (field log: thrown -> grabbed 0.6s later).
bool   g_grabIntent[2]{};
float  g_prevGrip[2]{};
// Short velocity history per hand: the grip crosses the release threshold
// AFTER the arm already started decelerating, so the throw uses the PEAK
// hand velocity of the last ~150 ms instead of the release-frame velocity.
struct VelSample { V3 v; double at; };
VelSample g_velHistory[2][10]{};
int       g_velHistoryNext[2]{};
// World-space hand tracking for throw velocity.
bool   g_handValid[2]{};
V3     g_handWorld[2]{};
V3     g_handVelocity[2]{};
double g_handSampleAt = 0.0;
std::atomic<bool> g_spawnRequested{false};

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
               Clock::now().time_since_epoch()).count();
}

void* BallMatrix() {
    if (!g_ball) return nullptr;
    void* matrix = nullptr;
    std::memcpy(&matrix,
                static_cast<const std::uint8_t*>(g_ball) +
                    kEntityMatrixOffset,
                sizeof(matrix));
    return matrix;
}

bool CaptureBallHandle() {
    g_ballSlot = -1;
    if (!g_ball || !g_objectPool || !*g_objectPool) return false;
    const auto* pool = static_cast<const std::uint8_t*>(*g_objectPool);
    std::uint8_t* objects;
    std::uint8_t* refs;
    std::int32_t capacity;
    std::memcpy(&objects, pool, sizeof(objects));
    std::memcpy(&refs, pool + 8, sizeof(refs));
    std::memcpy(&capacity, pool + 0x10, sizeof(capacity));
    const auto delta = static_cast<std::intptr_t>(
        static_cast<std::uint8_t*>(g_ball) - objects);
    if (delta < 0 || delta % 0x220 != 0) return false;
    const auto slot = static_cast<std::int32_t>(delta / 0x220);
    if (slot >= capacity) return false;
    g_ballSlot = slot;
    g_ballRef = refs[slot];
    return (g_ballRef & 0x80u) == 0;
}

bool BallValid() {
    if (!g_ball) return false;
    // HONEST pool liveness: the slot's ref byte must still match what we
    // captured at spawn, and the free bit must be clear. A model-only check
    // let a stale pointer through and crashed the first field test.
    if (g_ballSlot < 0 || !g_objectPool || !*g_objectPool) {
        g_ball = nullptr;
        g_heldHand = -1;
        return false;
    }
    const auto* pool = static_cast<const std::uint8_t*>(*g_objectPool);
    std::uint8_t* refs;
    std::memcpy(&refs, pool + 8, sizeof(refs));
    const std::uint8_t current = refs[g_ballSlot];
    if (current != g_ballRef || (current & 0x80u) != 0) {
        LOGI("[basketball] ball slot recycled (ref %u -> %u) - dropped",
             g_ballRef, current);
        g_ball = nullptr;
        g_ballSlot = -1;
        g_heldHand = -1;
        return false;
    }
    return true;
}

V3 BallPosition() {
    void* matrix = BallMatrix();
    V3 pos{};
    if (matrix) {
        std::memcpy(&pos,
                    static_cast<const std::uint8_t*>(matrix) +
                        kMatrixPosOffset,
                    sizeof(pos));
    }
    return pos;
}

// The ball's motion is simulated ENTIRELY by us in metres per second.
// Handing the engine a moveSpeed looked right in logs, but CPhysical's
// air-resistance pass strangled the horizontal component within a second
// (field telemetry: a 4.9 m/s throw moved the ball 0.33 m in 2 s while the
// y coordinate froze outright) - so the engine integrator is never used:
// every frame we integrate, bounce and Teleport, and keep the engine's
// moveSpeed pinned at zero.
V3     g_ballVel{};
double g_simLastAt = 0.0;
// Throw snapshot: full release (grip < 0.35) is detected LATE, well into the
// follow-through, so a peak taken then points down/back (field log: a
// "7.5 m/s" throw dropped the ball almost straight down). The hand velocity
// is frozen at the moment the fingers BEGIN to open (grip falls below the
// grab threshold) - that instant is closest to the real ball release.
V3     g_throwSnap[2]{};
double g_throwSnapAt[2] = {-1.0, -1.0};
// Two-hand hold: both palms on the ball; releasing within a 0.25 s window
// of each other counts as one two-hand throw (nobody releases two grips in
// the same millisecond).
bool   g_twoHanded = false;
double g_relWindowAt = -1.0;
// Last position where the ball sat ABOVE its ground column - the trusted
// anchor for the under-world rescue. Near buildings the column top is the
// ROOF (or an awning), so "below the column top" does NOT mean lost.
V3    g_lastSafePos{};
float g_lastSafeGround = 0.0f;
bool  g_haveSafePos = false;
// Casual throw: while holding the ball with CASUAL THROW on, the trigger
// shows a grenade-style aim arc and releasing it throws the ball along it.
bool g_casualAiming = false;
V3   g_casualVelocity{};
// Bullet deflation: a shot flattens the ball over ~1.2 s (visual z-scale
// and physics together), it stops bouncing, sinks, and despawns after 8 s
// by handing the object back to the population GC.
bool   g_deflated = false;
double g_deflatedAt = 0.0;

// 1.0 = round, 0.12 = fully flat.
float DeflateFactor() {
    if (!g_deflated) return 1.0f;
    const float t =
        static_cast<float>((NowSeconds() - g_deflatedAt) / 1.2);
    if (t <= 0.0f) return 1.0f;
    if (t >= 1.0f) return 0.12f;
    return 1.0f - 0.88f * t;
}
// Post-throw sim trace so weird trajectories are debuggable from logcat.
double g_simTraceUntil = 0.0;
double g_simTraceLast  = 0.0;

// BALL RADIUS drives the VISUAL size too: the ball is a sphere, so its
// matrix axes are simply the uniform scale (setting / the model's natural
// ~12 cm radius). Written before every Teleport - Teleport(pos, 0) keeps
// the rotation part and syncs the matrix into the RwFrame for rendering.
constexpr float kBallModelRadius = 0.12f;

void ApplyBallVisualScale() {
    if (!g_ball) return;
    const std::uintptr_t mtx = *reinterpret_cast<const std::uintptr_t*>(
        static_cast<const std::uint8_t*>(g_ball) + 0x18);
    if (mtx == 0) return;
    const float s = std::max(0.05f,
        Phys(PHYS_BALL_RADIUS, 0.01f) / kBallModelRadius);
    // A shot ball goes flat: z collapses, xy bulges slightly.
    const float flat = DeflateFactor();
    const float sxy = s * (1.0f + 0.25f * (1.0f - flat));
    auto* m = reinterpret_cast<float*>(mtx);
    const bool heldBasis = g_heldHand >= 0 && g_heldBallBasis.valid &&
        Finite(g_heldBallBasis.right) && Finite(g_heldBallBasis.forward) &&
        Finite(g_heldBallBasis.up);
    const V3 right = heldBasis ? g_heldBallBasis.right : V3{1.0f, 0.0f, 0.0f};
    const V3 forward = heldBasis ? g_heldBallBasis.forward : V3{0.0f, 1.0f, 0.0f};
    const V3 up = heldBasis ? g_heldBallBasis.up : V3{0.0f, 0.0f, 1.0f};
    m[0] = right.x * sxy;   m[1] = right.y * sxy;
    m[2] = right.z * sxy;
    m[4] = forward.x * sxy; m[5] = forward.y * sxy;
    m[6] = forward.z * sxy;
    m[8] = up.x * s * flat; m[9] = up.y * s * flat;
    m[10] = up.z * s * flat;
}

void TeleportBall(float x, float y, float z, int keepRotation) {
    ApplyBallVisualScale();
    g_teleport(g_ball, x, y, z, keepRotation ? 0 : 1);
}

void ZeroBallVelocity() {
    g_ballVel = V3{};
    if (!g_ball) return;
    auto* bytes = static_cast<std::uint8_t*>(g_ball);
    const float zero[3]{};
    std::memcpy(bytes + kMoveSpeedOffset, zero, sizeof(zero));
    std::memcpy(bytes + kTurnSpeedOffset, zero, sizeof(zero));
}

void ApplyRim(const V3& ball, V3& next, float radius);
void ApplyVehicles(V3& next, float radius);
struct Hoop;
V3 RimCentre(const Hoop& hoop);
float RimRingRadius();

// One frame of our own ballistics: gravity, light air drag, floor bounce,
// then a sector-correct Teleport to the integrated position. The engine's
// own integrator stays off (moveSpeed pinned to zero) - its air-resistance
// pass is what killed every throw.
void StepBallistics() {
    if (!g_teleport) return;
    const double now = NowSeconds();
    const float dt = g_simLastAt > 0.0
        ? static_cast<float>(now - g_simLastAt) : 0.0f;
    g_simLastAt = now;
    if (dt <= 0.0f || dt > 0.1f) return;   // pause/hitch: resync next frame

    const V3 ball = BallPosition();
    if (!Finite(ball)) return;

    g_ballVel.z -= 9.81f * dt;
    const float drag = std::max(0.0f, 1.0f - 0.05f * dt);
    g_ballVel.x *= drag;
    g_ballVel.y *= drag;

    V3 next = ball + g_ballVel * dt;
    const float flat = DeflateFactor();
    const float radius = Phys(PHYS_BALL_RADIUS, 0.01f) *
                         std::max(0.15f, flat);

    // Real collision: one short raycast along this frame's motion, padded
    // by the ball radius. Buildings + map objects + dummies cover walls,
    // ground, the hoop stand with its backboard, and fences. The ball
    // itself is a CObject WITHOUT a collision model (that is why this sim
    // exists), so it cannot self-hit.
    if (g_processLineOfSight != nullptr) {
        const V3 motion = next - ball;
        const float len = Length(motion);
        if (len > 1e-5f) {
            const V3 dir = motion * (1.0f / len);
            const V3 target = ball + dir * (len + radius);
            std::uint8_t colPoint[0x2C]{};
            void* entity = nullptr;
            const float o[3]{ball.x, ball.y, ball.z};
            const float t[3]{target.x, target.y, target.z};
            void* const player = g.FindPlayerPed ? g.FindPlayerPed(-1)
                                                  : nullptr;
            void* const savedIgnore = g.CWorld_pIgnoreEntity
                ? *g.CWorld_pIgnoreEntity : nullptr;
            if (g.CWorld_pIgnoreEntity) *g.CWorld_pIgnoreEntity = player;
            const bool rayHit =
                g_processLineOfSight(o, t, colPoint, &entity,
                                     true, false, true, true, true,
                                     false, false, false);
            if (g.CWorld_pIgnoreEntity) *g.CWorld_pIgnoreEntity = savedIgnore;
            if (rayHit) {
                V3 point, normal;
                std::memcpy(&point, colPoint + 0x00, sizeof(point));
                std::memcpy(&normal, colPoint + 0x10, sizeof(normal));
                // A ped took the ball: hand him the same damage event the
                // VR fists use (small damage, torso) so he reacts, with a
                // cooldown so a rolling ball does not spam events.
                if (entity != nullptr && entity != player &&
                    g.CWeapon_GenerateDamageEvent && player &&
                    (*(reinterpret_cast<const std::uint8_t*>(entity) +
                       0x5A) & 7) == 3) {
                    static double s_lastPedHitAt = 0.0;
                    const float ballSpeed = Length(g_ballVel);
                    if (ballSpeed > 2.5f &&
                        now - s_lastPedHitAt > 0.3) {
                        s_lastPedHitAt = now;
                        int direction = 0;
                        if (g.CEntity_GetBoundCentre &&
                            g.CPed_GetLocalDirection) {
                            GameSymbols::Vec3 centre{};
                            g.CEntity_GetBoundCentre(entity, &centre);
                            const GameSymbols::Vec2 rel{
                                point.x - centre.x, point.y - centre.y};
                            direction = std::clamp(
                                g.CPed_GetLocalDirection(entity, &rel),
                                0, 3);
                        }
                        g.CWeapon_GenerateDamageEvent(
                            entity, player, 0 /*unarmed*/,
                            static_cast<int>(std::min(12.0f, ballSpeed)),
                            3 /*torso*/, direction);
                        LOGI("[basketball] ped hit speed=%.1f", ballSpeed);
                    }
                }
                const float nLen = Length(normal);
                if (Finite(point) && Finite(normal) && nLen > 0.1f) {
                    V3 n = normal * (1.0f / nLen);
                    // The surface normal must oppose the motion.
                    if (Dot(n, dir) > 0.0f) n = n * -1.0f;
                    next = point + n * (radius + 0.01f);
                    const float vn = Dot(g_ballVel, n);
                    if (vn < 0.0f) {
                        // A flat ball thuds dead instead of bouncing
                        // (cubed: at flat=0.12 restitution is ~0.2%).
                        g_ballVel =
                            g_ballVel - n * (vn * (1.0f +
                                Phys(PHYS_BOUNCE, 0.01f) *
                                flat * flat * flat));
                        if (n.z > 0.7f) {
                            // Ground-like: roll friction + settle; a flat
                            // ball also scrubs off speed much faster.
                            const float scrub =
                                Phys(PHYS_FRICTION, 0.01f) *
                                (0.4f + 0.6f * flat);
                            g_ballVel.x *= scrub;
                            g_ballVel.y *= scrub;
                            if (g_ballVel.z > 0.0f && g_ballVel.z < 0.6f)
                                g_ballVel.z = 0.0f;
                        } else if (now < g_simTraceUntil + 2.0) {
                            LOGI("[basketball] surface bounce "
                                 "n=(%.2f %.2f %.2f) v=(%.1f %.1f %.1f)",
                                 n.x, n.y, n.z, g_ballVel.x, g_ballVel.y,
                                 g_ballVel.z);
                        }
                    }
                }
            }
        }
    }
    ApplyRim(ball, next, radius);
    ApplyVehicles(next, radius);

    // Buoyancy: the ball floats instead of sinking. A spring toward the
    // waterline (ball ~half submerged at rest) plus water drag; the level
    // includes waves, so it bobs on the swell. One table lookup per frame.
    if (g_getWaterLevel != nullptr) {
        float waterZ = 0.0f;
        if (g_getWaterLevel(next.x, next.y, next.z, &waterZ, true, nullptr) &&
            std::isfinite(waterZ)) {
            const float floatLine = waterZ + radius * 0.2f;
            if (next.z < floatLine) {
                const float depth =
                    std::min(floatLine - next.z, 2.0f * radius + 0.5f);
                // Buoyancy comes from the air inside: flat = sinks
                // (spring falls below gravity once deflated).
                g_ballVel.z += depth * 80.0f * flat * flat * dt;
                const float waterDrag = std::max(0.0f, 1.0f - 3.0f * dt);
                g_ballVel = g_ballVel * waterDrag;
            }
        }
    }

    // Under-world rescue, last safety net. The column top near buildings
    // is the ROOF or an awning, so a ball bouncing at street level under an
    // overhang is NOT lost - lifting it there teleported it onto roofs.
    // Lift only onto a surface near the last trusted floor level; a ball
    // that truly sank far below that comes back to the last safe spot.
    if (g.CWorld_FindGroundZForCoord) {
        const float ground = g.CWorld_FindGroundZForCoord(next.x, next.y);
        if (std::isfinite(ground)) {
            if (next.z > ground - 0.5f) {
                g_lastSafePos = next;
                g_lastSafeGround = ground;
                g_haveSafePos = true;
            } else if (next.z < ground - 3.0f && g_haveSafePos) {
                if (ground <= g_lastSafeGround + 1.5f) {
                    next.z = ground + radius + 0.05f;
                    g_ballVel = V3{};
                    LOGI("[basketball] rescued from under the world");
                } else if (next.z < g_lastSafeGround - 3.0f) {
                    next = g_lastSafePos;
                    next.z += 0.1f;
                    g_ballVel = V3{};
                    LOGI("[basketball] returned from inside a building");
                }
                // else: street-level ball under an overhang - leave it,
                // the raycast floor keeps it bouncing normally.
            }
        }
    }
    if (!Finite(next)) return;
    if (now < g_simTraceUntil && now - g_simTraceLast > 0.1) {
        g_simTraceLast = now;
        LOGI("[basketball] sim pos=(%.2f %.2f %.2f) v=(%.1f %.1f %.1f)",
             next.x, next.y, next.z, g_ballVel.x, g_ballVel.y, g_ballVel.z);
    }
    TeleportBall(next.x, next.y, next.z, 1);
    // Pin the engine integrator to zero WITHOUT touching our velocity.
    if (g_ball) {
        auto* bytes = static_cast<std::uint8_t*>(g_ball);
        const float zero[3]{};
        std::memcpy(bytes + kMoveSpeedOffset, zero, sizeof(zero));
        std::memcpy(bytes + kTurnSpeedOffset, zero, sizeof(zero));
    }
}

void SetBallVelocity(V3 metresPerSecond) {
    if (!g_ball) return;
    const float speed = Length(metresPerSecond);
    // Field footgun: THROW MAX=10 (= 1 m/s) silently killed every throw.
    // The cap exists to stop physics-breaking flings, not to be a brake.
    const float maxSpeed = std::max(3.0f, Phys(PHYS_THROW_MAX, 0.1f));
    if (speed > maxSpeed)
        metresPerSecond = metresPerSecond * (maxSpeed / speed);
    g_ballVel = metresPerSecond;
}

void UpdateHandTracking() {
    xr::HandPose poses[2]{};
    const bool ok = physicalweapon::GetHandPosesSnapshot(poses);
    const double now = NowSeconds();
    for (int hand = 0; hand < 2 && ok; ++hand) {
        const float grip = poses[hand].valid ? poses[hand].grip : 0.0f;
        if (grip >= kGrabGrip && g_prevGrip[hand] < 0.45f)
            g_grabIntent[hand] = true;            // fresh squeeze
        else if (grip < kReleaseGrip)
            g_grabIntent[hand] = false;           // fully released
        g_prevGrip[hand] = grip;
    }
    const float dt = g_handSampleAt > 0.0
        ? static_cast<float>(now - g_handSampleAt) : 0.0f;
    for (int hand = 0; hand < 2; ++hand) {
        const bool wasValid = g_handValid[hand];
        const V3 previous = g_handWorld[hand];
        g_handValid[hand] = false;
        if (!ok || !poses[hand].valid) continue;
        float world[3]{};
        if (!vrcam::TrackingPointToWorld(poses[hand].gripPos, world))
            continue;
        const V3 current{world[0], world[1], world[2]};
        if (!Finite(current)) continue;
        g_handWorld[hand] = current;
        g_handValid[hand] = true;
        if (wasValid && dt > 0.0f && dt <= 0.1f) {
            const V3 raw = (current - previous) * (1.0f / dt);
            if (Finite(raw) && Length(raw) < 40.0f) {
                // Light low-pass: controller jitter must not spike a throw.
                g_handVelocity[hand] =
                    g_handVelocity[hand] * 0.35f + raw * 0.65f;
                g_velHistory[hand][g_velHistoryNext[hand]] =
                    VelSample{g_handVelocity[hand], now};
                g_velHistoryNext[hand] =
                    (g_velHistoryNext[hand] + 1) % 10;
            }
        } else {
            g_handVelocity[hand] = V3{};
        }
    }
    g_handSampleAt = now;
}

// Peak hand velocity over the last `window` seconds - the natural release
// happens after the swing peak, and using the release-frame velocity made
// every throw feel weak.
V3 PeakHandVelocity(int hand, double window) {
    const double now = NowSeconds();
    V3 best = g_handVelocity[hand];
    float bestLen = Length(best);
    for (const VelSample& sample : g_velHistory[hand]) {
        if (sample.at <= 0.0 || now - sample.at > window) continue;
        const float len = Length(sample.v);
        if (len > bestLen) {
            bestLen = len;
            best = sample.v;
        }
    }
    return best;
}

float HandGrip(int hand) {
    xr::HandPose poses[2]{};
    if (!physicalweapon::GetHandPosesSnapshot(poses)) return 0.0f;
    return poses[hand].grip;
}

V3 CrossV(const V3& a, const V3& b) {
    return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x};
}

// Where a HELD ball sits: offset from the grip centre along the palm
// normal by (almost) the ball radius, so the hand visibly wraps the ball
// from the side instead of poking out of its middle. The palm normal is
// the grip pose's local X axis (mirrored per hand, OpenXR convention),
// rotated into tracking space and converted to world via two points.
// World-space palm normal (from the hand pivot toward where a held ball
// sits). Grip-pose local X, mirrored per hand, rotated into tracking and
// converted to world via two points.
bool PalmDirWorld(int hand, V3& out) {
    xr::HandPose poses[2]{};
    if (!physicalweapon::GetHandPosesSnapshot(poses) || !poses[hand].valid)
        return false;
    const float* q = poses[hand].gripOri;    // x,y,z,w
    V3 axis{hand == 0 ? 1.0f : -1.0f, 0.0f, 0.0f};
    const V3 u{q[0], q[1], q[2]};
    const V3 t = CrossV(u, axis) * 2.0f;
    const V3 rotated = axis + t * q[3] + CrossV(u, t);
    float probe[3]{poses[hand].gripPos[0] + rotated.x * 0.1f,
                   poses[hand].gripPos[1] + rotated.y * 0.1f,
                   poses[hand].gripPos[2] + rotated.z * 0.1f};
    float baseWorld[3]{}, probeWorld[3]{};
    if (!vrcam::TrackingPointToWorld(poses[hand].gripPos, baseWorld) ||
        !vrcam::TrackingPointToWorld(probe, probeWorld))
        return false;
    V3 dir{probeWorld[0] - baseWorld[0], probeWorld[1] - baseWorld[1],
           probeWorld[2] - baseWorld[2]};
    const float len = Length(dir);
    if (!std::isfinite(len) || len < 1e-4f) return false;
    out = dir * (1.0f / len);
    return true;
}

// Hand pivot -> ball centre distance: the ball sits just past the palm
// surface (pivot outside the sphere), so open fingers rest ON the ball.
float BallHoldDistance() {
    return Phys(PHYS_BALL_RADIUS, 0.01f) + 0.025f;
}

V3 BallHoldPosition(int hand) {
    const V3 fallback = g_handWorld[hand];
    V3 dir{};
    if (!PalmDirWorld(hand, dir)) return fallback;
    return fallback + dir * BallHoldDistance();
}

V3 RotateByQuat(const float q[4], const V3& v) {
    const V3 u{q[0], q[1], q[2]};
    const V3 t = CrossV(u, v) * 2.0f;
    return v + t * q[3] + CrossV(u, t);
}

bool PalmFrameTracking(const xr::HandPose& pose, int hand,
                       V3& palm, V3& forward) {
    if (!pose.valid) return false;
    const float* q = pose.gripOri;
    palm = RotateByQuat(
        q, V3{hand == 0 ? 1.0f : -1.0f, 0.0f, 0.0f});
    const float palmLen = Length(palm);
    if (!std::isfinite(palmLen) || palmLen < 0.2f) return false;
    palm = palm * (1.0f / palmLen);
    const V3 rawForward = RotateByQuat(q, V3{0.0f, 0.0f, -1.0f});
    forward = rawForward - palm * Dot(rawForward, palm);
    const float forwardLen = Length(forward);
    if (!std::isfinite(forwardLen) || forwardLen < 0.2f) return false;
    forward = forward * (1.0f / forwardLen);
    return true;
}

bool HandBallCentreWorld(const xr::HandPose& pose, int hand,
                         const V3& palmTracking, V3& centreWorld) {
    float probe[3]{pose.gripPos[0] + palmTracking.x * 0.10f,
                   pose.gripPos[1] + palmTracking.y * 0.10f,
                   pose.gripPos[2] + palmTracking.z * 0.10f};
    float baseWorld[3]{}, probeWorld[3]{};
    if (!vrcam::TrackingPointToWorld(pose.gripPos, baseWorld) ||
        !vrcam::TrackingPointToWorld(probe, probeWorld)) return false;
    V3 direction{probeWorld[0] - baseWorld[0],
                 probeWorld[1] - baseWorld[1],
                 probeWorld[2] - baseWorld[2]};
    const float length = Length(direction);
    if (!std::isfinite(length) || length < 1e-4f) return false;
    direction = direction * (1.0f / length);
    centreWorld = V3{baseWorld[0], baseWorld[1], baseWorld[2]} +
                  direction * BallHoldDistance();
    return Finite(centreWorld);
}

bool TrackingDirectionToWorld(const xr::HandPose& pose,
                              const V3& directionTracking,
                              V3& directionWorld) {
    float probe[3]{pose.gripPos[0] + directionTracking.x * 0.10f,
                   pose.gripPos[1] + directionTracking.y * 0.10f,
                   pose.gripPos[2] + directionTracking.z * 0.10f};
    float baseWorld[3]{}, probeWorld[3]{};
    if (!vrcam::TrackingPointToWorld(pose.gripPos, baseWorld) ||
        !vrcam::TrackingPointToWorld(probe, probeWorld)) return false;
    directionWorld = V3{probeWorld[0] - baseWorld[0],
                        probeWorld[1] - baseWorld[1],
                        probeWorld[2] - baseWorld[2]};
    const float length = Length(directionWorld);
    if (!Finite(directionWorld) || !std::isfinite(length) || length < 1e-4f)
        return false;
    directionWorld = directionWorld * (1.0f / length);
    return true;
}

bool BuildHeldBallBasis(const xr::HandPose& pose, BallVisualBasis& basis) {
    if (!pose.valid) return false;
    const V3 rightTracking = RotateByQuat(pose.gripOri, {1.0f, 0.0f, 0.0f});
    const V3 forwardTracking = RotateByQuat(pose.gripOri, {0.0f, 0.0f, -1.0f});
    const V3 upTracking = RotateByQuat(pose.gripOri, {0.0f, 1.0f, 0.0f});
    BallVisualBasis candidate{};
    if (!TrackingDirectionToWorld(pose, rightTracking, candidate.right) ||
        !TrackingDirectionToWorld(pose, forwardTracking, candidate.forward) ||
        !TrackingDirectionToWorld(pose, upTracking, candidate.up)) return false;
    candidate.valid = true;
    basis = candidate;
    return true;
}

bool BuildTrackingBasis(const xr::HandPose& pose,
                        V3& right, V3& forward, V3& up) {
    if (!pose.valid) return false;
    right = RotateByQuat(pose.gripOri, {1.0f, 0.0f, 0.0f});
    forward = RotateByQuat(pose.gripOri, {0.0f, 0.0f, -1.0f});
    up = RotateByQuat(pose.gripOri, {0.0f, 1.0f, 0.0f});
    const float rightLen = Length(right);
    const float forwardLen = Length(forward);
    const float upLen = Length(up);
    if (!Finite(right) || !Finite(forward) || !Finite(up) ||
        !std::isfinite(rightLen) || !std::isfinite(forwardLen) ||
        !std::isfinite(upLen) || rightLen < 0.2f ||
        forwardLen < 0.2f || upLen < 0.2f) return false;
    right = right * (1.0f / rightLen);
    forward = forward * (1.0f / forwardLen);
    up = up * (1.0f / upLen);
    return true;
}

V3 ToLocal(const V3& value, const V3& right,
           const V3& forward, const V3& up) {
    return {Dot(value, right), Dot(value, forward), Dot(value, up)};
}

V3 FromLocal(const V3& value, const V3& right,
             const V3& forward, const V3& up) {
    return right * value.x + forward * value.y + up * value.z;
}

V3 RotateAroundAxis(const V3& value, const V3& axis, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return value * c + CrossV(axis, value) * s +
           axis * (Dot(axis, value) * (1.0f - c));
}

void ApplyHandCalibration(int hand, V3& wrist, V3& meshUp,
                          V3& meshForward) {
    // Contact-space frame: X runs across the palm, Y towards the fingertips,
    // Z out of the palm into the ball.  Values are authored once on the right
    // hand. Mirroring X/yaw/roll produces the matching left-hand contact.
    V3 across = CrossV(meshForward, meshUp);
    const float acrossLen = Length(across);
    if (!Finite(across) || !std::isfinite(acrossLen) || acrossLen < 0.2f)
        return;
    across = across * (1.0f / acrossLen);
    constexpr float kMm = 0.001f;
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    const float mirror = hand == 0 ? -1.0f : 1.0f;
    wrist = wrist + across * (g_handCalib[HAND_CALIB_X].load() * kMm * mirror) +
            meshForward * (g_handCalib[HAND_CALIB_Y].load() * kMm) +
            meshUp * (g_handCalib[HAND_CALIB_Z].load() * kMm);

    const float pitch = g_handCalib[HAND_CALIB_PITCH].load() * kDeg;
    const float yaw = g_handCalib[HAND_CALIB_YAW].load() * kDeg * mirror;
    const float roll = g_handCalib[HAND_CALIB_ROLL].load() * kDeg * mirror;
    meshForward = RotateAroundAxis(meshForward, across, pitch);
    meshUp = RotateAroundAxis(meshUp, across, pitch);
    meshForward = RotateAroundAxis(meshForward, meshUp, yaw);
    across = RotateAroundAxis(across, meshUp, yaw);
    meshUp = RotateAroundAxis(meshUp, meshForward, roll);
}

// One pose snapshot drives BOTH the physical ball centre and the rendered
// wrists.  The old code sampled the centre live but latched the palm basis in
// tracking/world space at grab time; rotating a controller therefore sent the
// ball and hand around different arcs.  This is a true rigid controller-local
// carry transform, copied into the same stereo ring slot as the pixels.
bool BuildHeldBallPose(V3& ballCentre, xr::BallHandLock& lock) {
    xr::HandPose poses[2]{};
    if (!physicalweapon::GetHandPosesSnapshot(poses)) return false;
    V3 palm[2]{}, forward[2]{}, centre[2]{};
    bool usable[2]{};
    for (int hand = 0; hand < 2; ++hand) {
        const bool holding = g_heldHand == hand ||
                             (g_twoHanded && g_heldHand == 1 - hand);
        if (!holding || !PalmFrameTracking(
                poses[hand], hand, palm[hand], forward[hand]) ||
            !HandBallCentreWorld(poses[hand], hand, palm[hand], centre[hand]))
            continue;
        usable[hand] = true;
    }
    if (g_heldHand < 0 || g_heldHand >= 2 || !usable[g_heldHand])
        return false;
    // The physical ball is a rigid child of the primary hand. Averaging both
    // independently sampled palm centres made the ball itself move under the
    // support hand and guaranteed visible sliding.
    ballCentre = centre[g_heldHand];
    // Use the same input snapshot for rotation and position.  Keeping the
    // primary hand as the rigid parent avoids a two-hand average introducing
    // shear or a sudden texture snap while the support hand is added/removed.
    g_heldBallBasis.valid = g_heldHand >= 0 && g_heldHand < 2 &&
        BuildHeldBallBasis(poses[g_heldHand], g_heldBallBasis);

    const float hold = BallHoldDistance();
    const float visualHold = std::max(0.01f, hold - 0.02f);
    const int primary = g_heldHand;
    const int support = 1 - primary;
    const V3 centreTracking{
        poses[primary].gripPos[0] + palm[primary].x * hold,
        poses[primary].gripPos[1] + palm[primary].y * hold,
        poses[primary].gripPos[2] + palm[primary].z * hold};
    V3 primaryRight{}, primaryForward{}, primaryUp{};
    if (!BuildTrackingBasis(poses[primary], primaryRight,
                            primaryForward, primaryUp)) return false;
    for (int hand = 0; hand < 2; ++hand) {
        if (!usable[hand]) continue;
        // Move the visual wrist two centimetres INTO the previous contact
        // point. The former +4 cm pullback left only fingertips touching the
        // sphere; this seats the broad open palm without moving the ball.
        V3 wrist = centreTracking - palm[hand] * visualHold;
        V3 meshUp = palm[hand] * -1.0f;
        V3 meshForward = forward[hand];
        if (g_twoHanded && hand == support) {
            if (!g_ballSupportContact.valid ||
                g_ballSupportContact.primary != primary ||
                g_ballSupportContact.support != support) {
                g_ballSupportContact.valid = true;
                g_ballSupportContact.primary = primary;
                g_ballSupportContact.support = support;
                g_ballSupportContact.localPosition = ToLocal(
                    wrist - centreTracking, primaryRight,
                    primaryForward, primaryUp);
                g_ballSupportContact.localUp = ToLocal(
                    meshUp, primaryRight, primaryForward, primaryUp);
                g_ballSupportContact.localForward = ToLocal(
                    meshForward, primaryRight, primaryForward, primaryUp);
            }
            wrist = centreTracking + FromLocal(
                g_ballSupportContact.localPosition, primaryRight,
                primaryForward, primaryUp);
            meshUp = FromLocal(g_ballSupportContact.localUp, primaryRight,
                               primaryForward, primaryUp);
            meshForward = FromLocal(g_ballSupportContact.localForward,
                                     primaryRight, primaryForward, primaryUp);
        }
        ApplyHandCalibration(hand, wrist, meshUp, meshForward);
        lock.pos[hand][0] = wrist.x;
        lock.pos[hand][1] = wrist.y;
        lock.pos[hand][2] = wrist.z;
        // Mesh convention flip (field-tested): the hand model's 'up' is
        // the BACK of the hand, so the palm faces the ball with -palm.
        lock.up[hand][0] = meshUp.x;
        lock.up[hand][1] = meshUp.y;
        lock.up[hand][2] = meshUp.z;
        lock.fwd[hand][0] = meshForward.x;
        lock.fwd[hand][1] = meshForward.y;
        lock.fwd[hand][2] = meshForward.z;
        lock.locked[hand] = true;
    }
    if (!g_twoHanded) g_ballSupportContact = {};
    return true;
}

void ClearBallHandLock() {
    g_ballSupportContact = {};
    xr::SetBallHandLock(xr::BallHandLock{});
}

float HandTrigger(int hand) {
    xr::HandPose poses[2]{};
    if (!physicalweapon::GetHandPosesSnapshot(poses)) return 0.0f;
    return poses[hand].trigger;
}

// Grenade-style aim arc for the casual throw: integrate OUR ballistics
// (seconds, 9.81) from the hand along the weapon aim ray, raycast each
// segment, and hand the points to the same renderer the throwables use.
void PublishCasualArc(int hand, const V3& source, const V3& velocity) {
    constexpr int kMaxPoints = 41;
    V3 world[kMaxPoints];
    world[0] = source;
    int count = 1;
    bool arcHit = false;
    V3 previous = source;
    for (int segment = 1; segment < kMaxPoints; ++segment) {
        const float time = static_cast<float>(segment) * 0.08f;
        V3 point{source.x + velocity.x * time,
                 source.y + velocity.y * time,
                 source.z + velocity.z * time - 4.905f * time * time};
        if (g_processLineOfSight != nullptr) {
            std::uint8_t colPoint[0x2C]{};
            void* entity = nullptr;
            const float o[3]{previous.x, previous.y, previous.z};
            const float t[3]{point.x, point.y, point.z};
            if (g_processLineOfSight(o, t, colPoint, &entity,
                                     true, true, false, true, true,
                                     false, false, false)) {
                std::memcpy(&point, colPoint, sizeof(point));
                arcHit = true;
            }
        }
        world[count++] = point;
        previous = point;
        if (arcHit) break;
    }
    float tracking[kMaxPoints][3]{};
    for (int i = 0; i < count; ++i) {
        const float w[3]{world[i].x, world[i].y, world[i].z};
        if (!vrcam::WorldPointToTracking(w, tracking[i])) {
            xr::SetThrowableTrajectory(hand, nullptr, 0, false);
            return;
        }
    }
    xr::SetThrowableTrajectory(hand, tracking, count, arcHit);
}

void ClearCasualArc() {
    if (!g_casualAiming) return;
    g_casualAiming = false;
    xr::SetThrowableTrajectory(0, nullptr, 0, false);
    xr::SetThrowableTrajectory(1, nullptr, 0, false);
}

// Score probe: a downward crossing inside a rim-sized cylinder around a
// known hoop. Rim heights are calibrated from the [basketball] pass logs
// (they came out as IPL base z + 1.5 for all five stands). fx/fy is the
// unit facing toward the COURT, decoded from the binary-IPL quaternions
// (which are stored inverted): Ganton pairs face each other along Y, the
// hub stand faces west.
struct Hoop {
    float x, y, rimZ, fx, fy;
};
constexpr Hoop kHoops[] = {
    // Ganton double court (hoop stands, lae2_stream1.ipl; base z ~26.5-28).
    {2290.64f, -1541.61f, 29.6f,  0.0f,  1.0f},
    {2290.58f, -1514.27f, 29.6f,  0.0f, -1.0f},
    {2316.94f, -1541.61f, 28.0f,  0.0f,  1.0f},
    {2316.94f, -1514.27f, 28.0f,  0.0f, -1.0f},
    // East LS hub court (lae2_stream0.ipl).
    {2533.88f, -1667.58f, 17.8f, -1.0f,  0.0f},
};

// The rim as a horizontal ring (torus, tube radius 3 cm): the ball bounces
// off the metal from inside and outside, so shots can rattle in or rim out
// instead of ghosting through the hoop.
// SHOW PHYSICS: draw the collider geometry through the trajectory renderer
// (slot 0 = nearest rim ring at its collision position, slot 1 = the
// ball's physics contact circle) so a mismatch between the visual model
// and the physics is visible directly in the headset.
bool g_colliderDebugShown = false;

void PublishColliderDebug(const V3& ball) {
    const Hoop* nearest = nullptr;
    float bestD2 = 16.0f;                 // within 4 m
    for (const Hoop& hoop : kHoops) {
        const float dx = ball.x - hoop.x;
        const float dy = ball.y - hoop.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; nearest = &hoop; }
    }
    float points[41][3]{};
    if (nearest != nullptr) {
        const V3 centre = RimCentre(*nearest);
        const float ring = RimRingRadius();
        bool ok = true;
        for (int i = 0; i <= 40 && ok; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 40.0f;
            const float w[3]{centre.x + ring * std::cos(a),
                             centre.y + ring * std::sin(a),
                             centre.z};
            ok = vrcam::WorldPointToTracking(w, points[i]);
        }
        if (ok) xr::SetThrowableTrajectory(0, points, 41, true);
        else    xr::SetThrowableTrajectory(0, nullptr, 0, false);
    } else {
        xr::SetThrowableTrajectory(0, nullptr, 0, false);
    }
    // Ball contact circle (horizontal, at the ball's physics radius).
    {
        const float radius = Phys(PHYS_BALL_RADIUS, 0.01f);
        bool ok = Finite(ball);
        for (int i = 0; i <= 24 && ok; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 24.0f;
            const float w[3]{ball.x + radius * std::cos(a),
                             ball.y + radius * std::sin(a), ball.z};
            ok = vrcam::WorldPointToTracking(w, points[i]);
        }
        if (ok) xr::SetThrowableTrajectory(1, points, 25, false);
        else    xr::SetThrowableTrajectory(1, nullptr, 0, false);
    }
    g_colliderDebugShown = true;
}

void ClearColliderDebug() {
    if (!g_colliderDebugShown) return;
    g_colliderDebugShown = false;
    xr::SetThrowableTrajectory(0, nullptr, 0, false);
    xr::SetThrowableTrajectory(1, nullptr, 0, false);
}

// Distance from a point to a hoop's rim circle; fills the shell normal.
// Calibrated rim geometry, shared by collision, scoring and the debug
// overlay: one FORWARD/HEIGHT/RADIUS calibration applies to every hoop
// through its own IPL facing.
V3 RimCentre(const Hoop& hoop) {
    const float forward = Phys(PHYS_RIM_FORWARD, 0.01f);
    const float side    = Phys(PHYS_RIM_SIDE, 0.01f);
    // Sideways = the facing rotated 90 degrees CCW; consistent per stand,
    // so one calibration fits every hoop in its own frame.
    const float sx = -hoop.fy, sy = hoop.fx;
    return V3{hoop.x + hoop.fx * forward + sx * side,
              hoop.y + hoop.fy * forward + sy * side,
              hoop.rimZ + Phys(PHYS_RIM_HEIGHT, 0.01f)};
}
float RimRingRadius() { return Phys(PHYS_RIM_RADIUS, 0.01f); }

float RimShellDistance(const Hoop& hoop, const V3& p, V3* normalOut) {
    const V3 centre = RimCentre(hoop);
    const float ring = RimRingRadius();
    const float dx = p.x - centre.x;
    const float dy = p.y - centre.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    const float nx = d > 1e-3f ? centre.x + dx / d * ring : centre.x + ring;
    const float ny = d > 1e-3f ? centre.y + dy / d * ring : centre.y;
    const V3 fromRing{p.x - nx, p.y - ny, p.z - centre.z};
    const float dist = Length(fromRing);
    if (normalOut && dist > 1e-4f) *normalOut = fromRing * (1.0f / dist);
    return dist;
}

void ApplyRim(const V3& ball, V3& next, float radius) {
    constexpr float kRimTube = 0.03f;
    for (const Hoop& hoop : kHoops) {
        const V3 centre = RimCentre(hoop);
        const float dx = next.x - centre.x;
        const float dy = next.y - centre.y;
        if (dx * dx + dy * dy > 2.25f) continue;       // > 1.5 m away
        if (std::fabs(next.z - centre.z) > 1.0f) continue;
        const float minDist = radius + kRimTube;
        // Swept test: a 9 m/s ball moves ~13 cm per frame - more than the
        // 3 cm tube - so the segment ball->next is subsampled.
        float bestDist = 1e9f;
        V3 bestPoint{}, bestNormal{};
        for (int step = 0; step <= 4; ++step) {
            const float t = static_cast<float>(step) / 4.0f;
            const V3 p = ball + (next - ball) * t;
            V3 n{};
            const float dist = RimShellDistance(hoop, p, &n);
            if (dist < bestDist) {
                bestDist = dist;
                bestPoint = p;
                bestNormal = n;
            }
        }
        if (bestDist >= minDist) {
            // Near-miss diagnostics: how close did it come, throttled.
            static double lastNearLog = 0.0;
            const double logNow = NowSeconds();
            if (bestDist < minDist + 0.25f &&
                logNow - lastNearLog > 0.4) {
                lastNearLog = logNow;
                LOGI("[basketball] rim near-miss dist=%.2f need<%.2f "
                     "pos=(%.2f %.2f %.2f) rim_z=%.2f",
                     bestDist, minDist, next.x, next.y, next.z,
                     RimCentre(hoop).z);
            }
            continue;
        }
        const V3 n = bestNormal;
        const float vn = Dot(g_ballVel, n);
        if (vn < 0.0f) {
            const float keep = Phys(PHYS_BOUNCE, 0.01f);
            g_ballVel = g_ballVel - n * (vn * (1.0f + keep));
        }
        next = bestPoint + n * (minDist + 0.005f);
        LOGI("[basketball] rim bounce hoop=(%.1f,%.1f) n=(%.2f %.2f %.2f)",
             hoop.x, hoop.y, n.x, n.y, n.z);
        g_simTraceUntil = NowSeconds() + 0.5;
        break;
    }
}

// Vehicles as oriented boxes (generic car half-extents). The pool scan is
// ~110 slots with a one-byte liveness test and an early distance reject -
// far below anything measurable per frame. Stride 0xC68 and the +0/+8/+0x10
// pool layout are disasm-verified against CPools::GetVehicle.
void ApplyVehicles(V3& next, float radius) {
    if (g.CPools_ms_pVehiclePool == nullptr) return;
    auto* pool = static_cast<std::uint8_t*>(*g.CPools_ms_pVehiclePool);
    if (pool == nullptr) return;
    auto* slots = *reinterpret_cast<std::uint8_t**>(pool);
    auto* refs  = *reinterpret_cast<std::uint8_t**>(pool + 8);
    const int capacity = *reinterpret_cast<std::int32_t*>(pool + 0x10);
    if (slots == nullptr || refs == nullptr || capacity <= 0 ||
        capacity > 1000)
        return;
    constexpr float kHalfX = 1.05f, kHalfY = 2.35f, kHalfZ = 0.85f;
    for (int i = 0; i < capacity; ++i) {
        if (refs[i] & 0x80) continue;                  // free slot
        std::uint8_t* vehicle = slots + i * 0xC68;
        const std::uintptr_t mtx =
            *reinterpret_cast<const std::uintptr_t*>(vehicle + 0x18);
        if (mtx == 0) continue;
        const V3 pos = *reinterpret_cast<const V3*>(mtx + 0x30);
        if (std::fabs(pos.x - next.x) > 5.0f ||
            std::fabs(pos.y - next.y) > 5.0f ||
            std::fabs(pos.z - next.z) > 5.0f)
            continue;
        const V3 right = *reinterpret_cast<const V3*>(mtx + 0x00);
        const V3 fwd   = *reinterpret_cast<const V3*>(mtx + 0x10);
        const V3 up    = *reinterpret_cast<const V3*>(mtx + 0x20);
        if (!Finite(right) || !Finite(fwd) || !Finite(up)) continue;
        const V3 rel{next.x - pos.x, next.y - pos.y, next.z - pos.z};
        const float lx = Dot(rel, right);
        const float ly = Dot(rel, fwd);
        const float lz = Dot(rel, up);
        const float cx = std::clamp(lx, -kHalfX, kHalfX);
        const float cy = std::clamp(ly, -kHalfY, kHalfY);
        const float cz = std::clamp(lz, -kHalfZ, kHalfZ);
        V3 fromBox{(lx - cx), (ly - cy), (lz - cz)};   // vehicle-local
        float dist = Length(fromBox);
        V3 n;
        if (dist < 1e-4f) {
            // Centre inside the box: exit along the least-penetrated face.
            const float px = kHalfX - std::fabs(lx);
            const float py = kHalfY - std::fabs(ly);
            const float pz = kHalfZ - std::fabs(lz);
            if (pz <= px && pz <= py)
                n = up * (lz >= 0.0f ? 1.0f : -1.0f);
            else if (px <= py)
                n = right * (lx >= 0.0f ? 1.0f : -1.0f);
            else
                n = fwd * (ly >= 0.0f ? 1.0f : -1.0f);
            dist = 0.0f;
        } else {
            if (dist >= radius) continue;
            const V3 local = fromBox * (1.0f / dist);
            n = right * local.x + fwd * local.y + up * local.z;
        }
        // Bounce relative to the CAR's velocity, so a moving car really
        // knocks the ball (moveSpeed is units/frame at 50 fps -> m/s x50).
        V3 carVel{};
        std::memcpy(&carVel, vehicle + 0x68, sizeof(carVel));
        carVel = Finite(carVel) ? carVel * 50.0f : V3{};
        const V3 relVel = g_ballVel - carVel;
        const float vn = Dot(relVel, n);
        if (vn < 0.0f) {
            const float keep = Phys(PHYS_BOUNCE, 0.01f);
            g_ballVel = g_ballVel - n * (vn * (1.0f + keep));
            // Safety cap: a speeding car must not fling the ball into orbit.
            const float speed = Length(g_ballVel);
            if (speed > 22.0f) g_ballVel = g_ballVel * (22.0f / speed);
        }
        next = next + n * (radius - dist + 0.01f);
        LOGI("[basketball] car bounce slot=%d n=(%.2f %.2f %.2f) "
             "carv=%.1f", i, n.x, n.y, n.z, Length(carVel));
        g_simTraceUntil = NowSeconds() + 0.5;
        break;
    }
}
float g_previousBallZ = 0.0f;

void ProbeScore(const V3& ball) {
    for (const Hoop& hoop : kHoops) {
        const V3 centre = RimCentre(hoop);
        const float rimZ = centre.z;
        const float dx = ball.x - centre.x;
        const float dy = ball.y - centre.y;
        const float horizontal = std::sqrt(dx * dx + dy * dy);
        if (horizontal > 1.1f) continue;
        // Log every rim-area pass so the rim heights can be calibrated from
        // real throws; count a score on a downward crossing near rim level.
        static double lastPassLog = 0.0;
        const double now = NowSeconds();
        if (now - lastPassLog > 0.5) {
            lastPassLog = now;
            LOGI("[basketball] rim pass hoop=(%.1f,%.1f) dist=%.2f z=%.2f "
                 "prev_z=%.2f rim_z=%.2f",
                 hoop.x, hoop.y, horizontal, ball.z, g_previousBallZ,
                 rimZ);
        }
        // Only crossings INSIDE the actual ring (0.23) count - the rim
        // physics makes outside-of-ring rimZ crossings common.
        if (horizontal < RimRingRadius() && g_previousBallZ > rimZ &&
            ball.z <= rimZ) {
            LOGI("[basketball] SCORE at hoop (%.1f, %.1f)", hoop.x, hoop.y);
            if (g.AudioEngine && g.CAudioEngine_ReportFrontendAudioEvent) {
                // AE_FRONTEND_PART_MISSION_COMPLETE-style jingle.
                g.CAudioEngine_ReportFrontendAudioEvent(
                    g.AudioEngine, 60, 0.0f, 1.0f);
            }
        }
    }
    g_previousBallZ = ball.z;
}

} // namespace

int GetPhysicsValue(int field) {
    if (field < 0 || field >= PHYS_FIELD_COUNT) return 0;
    LoadPhysicsOnce();
    return g_physics[field].load(std::memory_order_relaxed);
}

void AdjustPhysics(int field, int direction) {
    if (field < 0 || field >= PHYS_FIELD_COUNT || direction == 0) return;
    LoadPhysicsOnce();
    const PhysicsSetting& setting = kPhysicsSettings[field];
    const int value = std::clamp(
        g_physics[field].load() +
            (direction < 0 ? -setting.step : setting.step),
        setting.minimum, setting.maximum);
    g_physics[field].store(value);
    SavePhysics();
}

const char* PhysicsFieldName(int field) {
    if (field < 0 || field >= PHYS_FIELD_COUNT) return "?";
    return kPhysicsSettings[field].name;
}

int GetHandCalibValue(int field) {
    if (field < 0 || field >= HAND_CALIB_FIELD_COUNT) return 0;
    LoadPhysicsOnce();
    return g_handCalib[field].load(std::memory_order_relaxed);
}

void AdjustHandCalib(int field, int direction) {
    if (field < 0 || field >= HAND_CALIB_FIELD_COUNT || direction == 0) return;
    LoadPhysicsOnce();
    const HandCalibSetting& setting = kHandCalibSettings[field];
    const int steps = std::max(1, std::abs(direction));
    const int value = std::clamp(
        g_handCalib[field].load() +
            (direction < 0 ? -setting.step * steps : setting.step * steps),
        setting.minimum, setting.maximum);
    g_handCalib[field].store(value);
    SavePhysics();
}

const char* HandCalibFieldName(int field) {
    if (field < 0 || field >= HAND_CALIB_FIELD_COUNT) return "?";
    return kHandCalibSettings[field].name;
}

void ResetHandCalibration() {
    LoadPhysicsOnce();
    for (auto& value : g_handCalib) value.store(0);
    SavePhysics();
}

void BeginHandCalibration() {
    LoadPhysicsOnce();
    g_deflated = false;
    g_handCalibrationActive.store(true, std::memory_order_release);
    SpawnBall();
    LOGI("[basketball.calib] preview begin (right master, left mirrored)");
}

void EndHandCalibration() {
    if (!g_handCalibrationActive.exchange(false, std::memory_order_acq_rel))
        return;
    LOGI("[basketball.calib] preview end");
}

bool HandCalibrationActive() {
    return g_handCalibrationActive.load(std::memory_order_acquire);
}

void Install(void* handle) {
    if (!handle) return;
    g_createObject = reinterpret_cast<CreateObjectFn>(
        dlsym(handle, "_ZN7CObject6CreateEib"));
    g_worldAdd = reinterpret_cast<WorldAddFn>(
        dlsym(handle, "_ZN6CWorld3AddEP7CEntity"));
    g_teleport = reinterpret_cast<TeleportFn>(
        dlsym(handle, "_ZN7CObject8TeleportE7CVectorh"));
    g_processLineOfSight = reinterpret_cast<ProcessLineOfSightFn>(dlsym(
        handle,
        "_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntity"
        "bbbbbbbb"));
    LOGI("[basketball] raycast %s",
         g_processLineOfSight ? "resolved" : "MISSING");
    g_getWaterLevel = reinterpret_cast<GetWaterLevelFn>(
        dlsym(handle, "_ZN11CWaterLevel13GetWaterLevelEfffPfbP7CVector"));
    g_loadAll = reinterpret_cast<LoadAllFn>(
        dlsym(handle, "_ZN10CStreaming22LoadAllRequestedModelsEb"));
    g_objectPool = static_cast<void**>(
        dlsym(handle, "_ZN6CPools14ms_pObjectPoolE"));
    g_createRw = reinterpret_cast<CreateRwFn>(
        dlsym(handle, "_ZN7CObject14CreateRwObjectEv"));
    LOGI("[basketball] install create=%d add=%d teleport=%d loadall=%d "
         "pool=%d",
         g_createObject != nullptr, g_worldAdd != nullptr,
         g_teleport != nullptr, g_loadAll != nullptr,
         g_objectPool != nullptr);
}

void OnBulletShot(const float origin[3], const float dir[3],
                  bool dirIsTarget) {
    if (!BallValid() || g_deflated) return;
    const V3 o{origin[0], origin[1], origin[2]};
    V3 d{dir[0], dir[1], dir[2]};
    if (dirIsTarget) d = d - o;
    const float len = Length(d);
    if (!std::isfinite(len) || len < 1e-3f) return;
    d = d * (1.0f / len);
    const float range = dirIsTarget ? len : 60.0f;
    const V3 ball = BallPosition();
    if (!Finite(ball)) return;
    // Closest approach of the shot segment to the ball centre.
    const V3 toBall = ball - o;
    const float along = std::clamp(Dot(toBall, d), 0.0f, range);
    const V3 closest = o + d * along;
    const float missBy = Length(ball - closest);
    if (missBy > Phys(PHYS_BALL_RADIUS, 0.01f) * 1.3f + 0.05f) return;

    if (Phys(PHYS_BULLET_DEFLATE, 1.0f) >= 0.5f) {
        g_deflated = true;
        g_deflatedAt = NowSeconds();
    }
    if (g_heldHand >= 0) g_heldHand = -1;   // shot out of the hand
    g_twoHanded = false;
    // Modest impulse: enough to shove it visibly, never into orbit -
    // the deflation must stay watchable.
    g_ballVel = g_ballVel + d * 3.0f + V3{0.0f, 0.0f, 1.2f};
    const float speed = Length(g_ballVel);
    if (speed > 5.5f) g_ballVel = g_ballVel * (5.5f / speed);
    LOGI("[basketball] ball shot (miss_by=%.2f) deflate=%d", missBy,
         g_deflated ? 1 : 0);
}

void SpawnBall() { g_deflated = false; g_spawnRequested.store(true, std::memory_order_release); }

void Update() {
    LoadPhysicsOnce();
    UpdateHandTracking();

    // Deferred spawn: the cheat runs on the input path; object creation
    // belongs on the game thread tick with streaming serviced.
    if (g_spawnRequested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_createObject || !g_worldAdd || !g_teleport ||
            !g.FindPlayerPed || !g.CStreaming_RequestModel ||
            !g.CStreaming_ms_aInfoForModel) {
            LOGW("[basketball] spawn unavailable (symbols missing)");
        } else if (void* ped = g.FindPlayerPed(-1)) {
            // Player position + forward from the ped matrix.
            const auto* pedBytes = static_cast<const std::uint8_t*>(ped);
            void* pedMatrix = nullptr;
            std::memcpy(&pedMatrix, pedBytes + kEntityMatrixOffset,
                        sizeof(pedMatrix));
            V3 pos{}, forward{0.0f, 1.0f, 0.0f};
            if (pedMatrix) {
                std::memcpy(&pos,
                            static_cast<const std::uint8_t*>(pedMatrix) +
                                kMatrixPosOffset,
                            sizeof(pos));
                std::memcpy(&forward,
                            static_cast<const std::uint8_t*>(pedMatrix) +
                                0x10,
                            sizeof(forward));
            }
            V3 spawnAt = pos + forward * 1.0f + V3{0.0f, 0.0f, 0.3f};
            // Snap to the actual ground: the ped root can sit below nearby
            // court geometry (log showed a spawn 0.8m under the surface —
            // the ball then drops through and reads as "vanished"). Retail
            // CREATE_OBJECT grounds script objects the same way.
            if (g.CWorld_FindGroundZForCoord) {
                const float ground =
                    g.CWorld_FindGroundZForCoord(spawnAt.x, spawnAt.y);
                if (std::isfinite(ground) && ground > pos.z - 4.0f &&
                    ground < pos.z + 4.0f) {
                    spawnAt.z = ground + 0.6f;
                }
            }

            if (g.CStreaming_ms_aInfoForModel[kBallModel * 20 + 0x10] != 1) {
                g.CStreaming_RequestModel(kBallModel, 0x0C);
                if (g_loadAll) g_loadAll(false);
            }
            if (g.CStreaming_ms_aInfoForModel[kBallModel * 20 + 0x10] != 1) {
                LOGW("[basketball] ball model %d not streamed yet - try the "
                     "cheat again in a second", kBallModel);
            } else if (BallValid()) {
                if (g.CModelInfo_ms_modelInfoPtrs) {
                    if (auto* info = static_cast<std::uint8_t*>(
                            g.CModelInfo_ms_modelInfoPtrs[kBallModel]))
                        info[0x26] = 0xFF;
                }
                void* rwObject = nullptr;
                std::memcpy(&rwObject,
                            static_cast<const std::uint8_t*>(g_ball) + 0x20,
                            sizeof(rwObject));
                if (rwObject == nullptr && g_createRw) g_createRw(g_ball);
                TeleportBall(spawnAt.x, spawnAt.y, spawnAt.z, 0);
                ZeroBallVelocity();
                g_heldHand = -1;
                LOGI("[basketball] ball recalled to player rw=%d",
                     rwObject != nullptr);
            } else {
                // Retail CREATE_OBJECT forces the MODEL alpha to 255 before
                // creating (strb 0xFF,[modelInfo+0x26] @0x4152f4): a freshly
                // streamed model can sit at alpha 0 (distance fade-in), which
                // leaves the spawned object alive but INVISIBLE — the first
                // field test's "ball vanished" was exactly this.
                if (g.CModelInfo_ms_modelInfoPtrs) {
                    if (auto* info = static_cast<std::uint8_t*>(
                            g.CModelInfo_ms_modelInfoPtrs[kBallModel]))
                        info[0x26] = 0xFF;
                }
                g_ball = g_createObject(kBallModel, false);
                if (g_ball) {
                    // Mission ownership (byte at +0x1A0 = 6): the retail
                    // CREATE_OBJECT opcode handler @0x41532c marks script
                    // objects exactly like this - population garbage
                    // collection then leaves the ball alone (the unmarked
                    // first field-test ball vanished within seconds).
                    static_cast<std::uint8_t*>(g_ball)[0x1A0] = 6;
                    if (g.CObject_SetIsStatic)
                        g.CObject_SetIsStatic(g_ball, false);
                    if (g.CPhysical_AddToMovingList)
                        g.CPhysical_AddToMovingList(g_ball);
                    g_worldAdd(g_ball);
                    TeleportBall(spawnAt.x, spawnAt.y, spawnAt.z, 0);
                    ZeroBallVelocity();
                    // Make sure the render object exists (entity RwObject
                    // pointer at +0x20); without it the ball is physical
                    // but never drawn.
                    void* rwObject = nullptr;
                    std::memcpy(&rwObject,
                                static_cast<const std::uint8_t*>(g_ball) +
                                    0x20,
                                sizeof(rwObject));
                    if (rwObject == nullptr && g_createRw) {
                        g_createRw(g_ball);
                        std::memcpy(&rwObject,
                                    static_cast<const std::uint8_t*>(g_ball) +
                                        0x20,
                                    sizeof(rwObject));
                    }
                    if (CaptureBallHandle()) {
                        LOGI("[basketball] ball spawned at (%.1f %.1f %.1f) "
                             "slot=%d ref=%u rw=%d",
                             spawnAt.x, spawnAt.y, spawnAt.z,
                             g_ballSlot, g_ballRef, rwObject != nullptr);
                    } else {
                        LOGW("[basketball] pool handle capture failed - "
                             "ball unmanaged");
                        g_ball = nullptr;
                    }
                } else {
                    LOGW("[basketball] CObject::Create failed");
                }
            }
        }
    }

    if (!BallValid()) {
        ClearBallHandLock();
        return;
    }

    // Field telemetry: where IS the ball. Falling through the map shows as
    // a plummeting z, a runaway bounce as drifting xy, a pure render bug as
    // a stable position the player cannot see.
    {
        static double lastTelemetry = 0.0;
        const double now = NowSeconds();
        if (now - lastTelemetry > 2.0) {
            lastTelemetry = now;
            const V3 ball = BallPosition();
            void* rwObject = nullptr;
            std::memcpy(&rwObject,
                        static_cast<const std::uint8_t*>(g_ball) + 0x20,
                        sizeof(rwObject));
            std::uint64_t entityFlags = 0;
            std::memcpy(&entityFlags,
                        static_cast<const std::uint8_t*>(g_ball) + 0x28,
                        sizeof(entityFlags));
            LOGI("[basketball] telemetry pos=(%.2f %.2f %.2f) rw=%d "
                 "flags=%llx held=%d",
                 ball.x, ball.y, ball.z, rwObject != nullptr,
                 static_cast<unsigned long long>(entityFlags), g_heldHand);
        }
    }

    // Collider debug overlay - never while the casual aim owns the
    // trajectory slots.
    if (Phys(PHYS_SHOW_PHYSICS, 1.0f) >= 0.5f && !g_casualAiming) {
        PublishColliderDebug(BallPosition());
    } else if (!g_casualAiming) {
        ClearColliderDebug();
    } else {
        g_colliderDebugShown = false;   // aim overwrote the slots
    }

    // Menu preview owns the ball outright: it is always visible in the right
    // palm, cannot be thrown by opening the grip, and uses the same production
    // pose path the player is calibrating. Leaving the page restores normal
    // input without deleting the ball.
    if (HandCalibrationActive()) {
        g_heldHand = 1;
        g_twoHanded = false;
        g_ballSupportContact = {};
        ClearCasualArc();
        V3 hold{};
        xr::BallHandLock previewLock{};
        if (BuildHeldBallPose(hold, previewLock)) {
            TeleportBall(hold.x, hold.y, hold.z, 1);
            ZeroBallVelocity();
            xr::SetBallHandLock(previewLock);
        }
        return;
    }

    if (g_heldHand < 0 && Phys(PHYS_AUTO_RETURN, 1.0f) >= 0.5f) {
        static float s_prevRecallTrig[2] = {0.0f, 0.0f};
        const bool inVehicle = g.FindPlayerVehicle &&
            g.FindPlayerVehicle(-1, false) != nullptr;
        for (int hand = 0; hand < 2; ++hand) {
            const float trig = HandTrigger(hand);
            const bool edge = trig > 0.6f && s_prevRecallTrig[hand] < 0.4f;
            s_prevRecallTrig[hand] = trig;
            if (!edge || inVehicle) continue;
            if (!g_handValid[hand]) continue;
            // Only a hand with no physical weapon may call the ball -
            // otherwise the trigger is the weapon's.
            if (physicalweapon::HeldSlot(hand) > 0) continue;
            g_heldHand = hand;
            g_grabIntent[hand] = false;
            const V3 to = BallHoldPosition(hand);
            TeleportBall(to.x, to.y, to.z, 1);
            ZeroBallVelocity();
            LOGI("[basketball] ball recalled to hand=%d", hand);
            break;
        }
    }

    if (g_deflated && NowSeconds() - g_deflatedAt > 8.0 && g_ball) {
        auto* bytes = static_cast<std::uint8_t*>(g_ball);
        bytes[0x1A0] = 2;   // ownedBy = population -> engine GC collects it
        g_ball = nullptr;
        g_heldHand = -1;
        g_twoHanded = false;
        g_deflated = false;
        LOGI("[basketball] deflated ball released to the GC");
        return;
    }

    if (g_heldHand < 0) {
        g_heldBallBasis.valid = false;
        ClearBallHandLock();
        StepBallistics();
        // Grab: squeezed grip near the ball. Prefer whichever hand is closer.
        const V3 ball = BallPosition();
        int best = -1;
        float bestDist = Phys(PHYS_GRAB_RANGE, 0.01f);
        for (int hand = 0; hand < 2; ++hand) {
            if (!g_handValid[hand]) continue;
            if (NowSeconds() < g_noTouchUntil[hand]) continue;
            if (!g_grabIntent[hand]) continue;
            if (HandGrip(hand) < kGrabGrip) continue;
            const float dist = Length(g_handWorld[hand] - ball);
            if (dist <= bestDist) {
                bestDist = dist;
                best = hand;
            }
        }
        if (best >= 0) {
            g_heldHand = best;
            g_grabIntent[best] = false;   // consumed; re-squeeze to re-grab
            ZeroBallVelocity();
            LOGI("[basketball] grabbed hand=%d", best);
        } else if (int hitHand = -1; true) {
            // Open-hand strike: a moving hand touching the ball knocks it
            // with the hand velocity - dribbling by palm, volleyball pokes,
            // punches. Grip must be OPEN (a squeezed grip means grab/magnet).
            static double lastHitAt = 0.0;
            const double hitNow = NowSeconds();
            // Hand-to-ball-CENTRE contact distance. Adding the ball
            // radius on top made the default 44 cm and the ball repelled
            // well before any visible touch.
            const float hitRange = Phys(PHYS_HIT_RANGE, 0.01f);
            // A strike counts when the hand APPROACHES the ball fast enough
            // (relative velocity along hand->ball). A follow-through hand
            // trailing a thrown ball never approaches it, and a dribble slap
            // onto a bouncing ball always does - unlike the old rule that
            // compared raw speeds and made hits impossible once the ball
            // kept real momentum.
            float bestApproach = 0.9f;
            for (int hand = 0; hand < 2; ++hand) {
                if (!g_handValid[hand]) continue;
                if (hitNow < g_noTouchUntil[hand]) continue;
                if (HandGrip(hand) >= kGrabGrip) continue;
                // The HAND must be doing the hitting: approach alone let a
                // ball bounce off a perfectly still hand it flew past.
                if (Length(g_handVelocity[hand]) < 0.7f) continue;
                const V3 toBall = ball - g_handWorld[hand];
                const float dist = Length(toBall);
                if (dist > hitRange || dist < 1e-3f) continue;
                const V3 dir = toBall * (1.0f / dist);
                const float approach =
                    Dot(g_handVelocity[hand] - g_ballVel, dir);
                if (approach > bestApproach) {
                    bestApproach = approach;
                    hitHand = hand;
                }
            }
            if (hitHand >= 0 && hitNow - lastHitAt > 0.12) {
                lastHitAt = hitNow;
                // The ball takes the HAND's velocity (scaled by HIT POWER).
                // The earlier hand->ball-centre push model flung every
                // off-centre palm contact sideways and made dribbling
                // impossible; palm moving down = ball goes down, period.
                SetBallVelocity(g_handVelocity[hitHand] *
                                Phys(PHYS_HIT_POWER, 0.01f));
                g_simTraceUntil = hitNow + 0.5;
                LOGI("[basketball] hit hand=%d approach=%.2f m/s "
                     "v=(%.1f %.1f %.1f)",
                     hitHand, bestApproach,
                     g_ballVel.x, g_ballVel.y, g_ballVel.z);
            }
            // Magnet assist: a squeezed grip near the ball reels it in, so
            // picking it off the floor doesn't demand a perfect crouch.
            int magnetHand = -1;
            float magnetDist = Phys(PHYS_MAGNET_RANGE, 0.01f);
            for (int hand = 0; hand < 2; ++hand) {
                if (!g_handValid[hand]) continue;
                if (NowSeconds() < g_noTouchUntil[hand]) continue;
                if (!g_grabIntent[hand]) continue;
                if (HandGrip(hand) < kGrabGrip) continue;
                const float dist = Length(g_handWorld[hand] - ball);
                if (dist <= magnetDist) {
                    magnetDist = dist;
                    magnetHand = hand;
                }
            }
            if (magnetHand >= 0 && g_teleport && magnetDist > 0.01f) {
                const V3 toHand = g_handWorld[magnetHand] - ball;
                const float magnetStep = Phys(PHYS_MAGNET_SPEED, 0.01f);
                const V3 step = toHand *
                    (std::min(magnetStep, magnetDist) / magnetDist);
                const V3 next = ball + step;
                TeleportBall(next.x, next.y, next.z, 1);
                ZeroBallVelocity();
            }
        }
        ProbeScore(ball);
        return;
    }

    // ---------------- Held ----------------
    const double nowHeld = NowSeconds();

    // A second squeezed palm near the ball joins the hold.
    if (!g_twoHanded) {
        const int other = 1 - g_heldHand;
        if (g_handValid[other] && g_grabIntent[other] &&
            HandGrip(other) >= kGrabGrip) {
            const float reach = Length(g_handWorld[other] - BallPosition());
            if (reach < Phys(PHYS_BALL_RADIUS, 0.01f) * 2.0f + 0.30f) {
                g_twoHanded = true;
                g_grabIntent[other] = false;
                g_relWindowAt = -1.0;
                LOGI("[basketball] two-hand hold");
            }
        }
    }

    // Per-hand throw snapshots: velocity frozen the moment that hand's
    // fingers begin to open.
    const auto updateSnap = [&](int hand) {
        if (!g_handValid[hand] || HandGrip(hand) >= kGrabGrip) {
            g_throwSnapAt[hand] = -1.0;
        } else if (g_throwSnapAt[hand] < 0.0) {
            g_throwSnap[hand] = PeakHandVelocity(hand, 0.12);
            g_throwSnapAt[hand] = nowHeld;
        }
    };
    const auto takeVel = [&](int hand) -> V3 {
        if (g_throwSnapAt[hand] > 0.0 &&
            nowHeld - g_throwSnapAt[hand] < 0.30) {
            return g_throwSnap[hand];
        }
        return g_handValid[hand] ? PeakHandVelocity(hand, 0.15) : V3{};
    };
    const auto releasedHand = [&](int hand) {
        return !g_handValid[hand] || HandGrip(hand) < kReleaseGrip;
    };
    const auto finishThrow = [&](const V3& velocity, const char* how) {
        ClearCasualArc();
        ClearBallHandLock();
        SetBallVelocity(velocity);
        g_throwSnapAt[0] = g_throwSnapAt[1] = -1.0;
        g_noTouchUntil[0] = nowHeld + 0.40;
        g_noTouchUntil[1] = nowHeld + 0.40;
        g_simTraceUntil = nowHeld + 0.8;
        LOGI("[basketball] %s v=(%.1f %.1f %.1f) speed=%.2f m/s",
             how, g_ballVel.x, g_ballVel.y, g_ballVel.z,
             Length(g_ballVel));
        g_heldHand = -1;
        g_twoHanded = false;
        g_relWindowAt = -1.0;
    };

    updateSnap(g_heldHand);
    if (g_twoHanded) updateSnap(1 - g_heldHand);

    if (g_twoHanded) {
        const int a = g_heldHand, b = 1 - g_heldHand;
        const bool relA = releasedHand(a), relB = releasedHand(b);
        if (relA && relB) {
            // Two-hand throw: both frozen palm velocities averaged. The
            // release window below already absorbed the human offset
            // between the two grips letting go.
            finishThrow((takeVel(a) + takeVel(b)) * 0.5f *
                            Phys(PHYS_THROW_POWER, 0.01f),
                        "two-hand throw");
            return;
        }
        if (relA || relB) {
            // One grip opened: wait for the other instead of letting the
            // ball hop into the remaining hand mid-throw.
            if (g_relWindowAt < 0.0) g_relWindowAt = nowHeld;
            if (nowHeld - g_relWindowAt > 0.25) {
                g_heldHand = relA ? b : a;   // deliberate one-hand carry
                g_twoHanded = false;
                g_relWindowAt = -1.0;
            }
        } else {
            g_relWindowAt = -1.0;            // both gripping again
        }
    } else if (releasedHand(g_heldHand)) {
        finishThrow(takeVel(g_heldHand) * Phys(PHYS_THROW_POWER, 0.01f),
                    "thrown");
        return;
    }

    // Casual throw (one-handed only): hold the trigger to aim the arc,
    // release it to send the ball along the preview.
    if (!g_twoHanded && Phys(PHYS_CASUAL_THROW, 1.0f) >= 0.5f) {
        const float trig = HandTrigger(g_heldHand);
        const bool pressureMode = Phys(PHYS_CASUAL_PRESSURE, 1.0f) >= 0.5f;
        const float aimThreshold = pressureMode ? 0.15f : 0.5f;
        if (trig > aimThreshold) {
            float origin[3]{}, direction[3]{};
            if (vrcam::GetWeaponFireRay(g_heldHand, 16, origin, direction)) {
                const V3 src{origin[0], origin[1], origin[2]};
                V3 fwd{direction[0], direction[1], direction[2]};
                const float len = Length(fwd);
                if (Finite(src) && std::isfinite(len) && len > 0.5f) {
                    fwd = fwd * (1.0f / len);
                    float speed = Phys(PHYS_CASUAL_SPEED, 0.1f);
                    if (pressureMode) {
                        speed *= std::clamp(
                            (trig - aimThreshold) / (1.0f - aimThreshold),
                            0.08f, 1.0f);
                    }
                    g_casualVelocity = fwd * speed;
                    g_casualAiming = true;
                    PublishCasualArc(g_heldHand, src, g_casualVelocity);
                }
            }
        } else if (g_casualAiming) {
            finishThrow(g_casualVelocity, "casual throw");
            return;
        }
    } else {
        ClearCasualArc();
    }

    // Carry: the ball rides the palm(s); two hands hold it between them.
    V3 hold{};
    xr::BallHandLock carryLock{};
    if (!BuildHeldBallPose(hold, carryLock)) {
        hold = BallHoldPosition(g_heldHand);
        if (g_twoHanded)
            hold = (hold + BallHoldPosition(1 - g_heldHand)) * 0.5f;
    }
    {
        static double lastCalibLog = 0.0;
        const double calibNow = NowSeconds();
        if (calibNow - lastCalibLog > 0.4) {
            for (const Hoop& hoop : kHoops) {
                const float cdx = hold.x - hoop.x;
                const float cdy = hold.y - hoop.y;
                if (cdx * cdx + cdy * cdy > 1.0f) continue;
                if (std::fabs(hold.z - hoop.rimZ) > 1.5f) continue;
                lastCalibLog = calibNow;
                LOGI("[basketball] rim calib rel=(%.2f %.2f %+.2f) "
                     "(ball-held offset from hoop origin; z rel to rimZ "
                     "%.2f)",
                     cdx, cdy, hold.z - hoop.rimZ, hoop.rimZ);
                break;
            }
        }
    }
    TeleportBall(hold.x, hold.y, hold.z, 1);
    ZeroBallVelocity();
    xr::SetBallHandLock(carryLock);
}

} // namespace savr::basketball
