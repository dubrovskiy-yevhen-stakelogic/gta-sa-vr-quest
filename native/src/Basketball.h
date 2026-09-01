#pragma once

// Custom immersive basketball: a real physical ball (BBALL_ingame, model
// 3120 — mass 10, elasticity 0.99 in object.dat) spawned as a plain CObject,
// grabbed with a tracked hand, thrown with the hand's velocity and dribbled
// off the ground through the engine's own bounce physics. Replaces the
// story-locked basketb.scm minigame entirely.
namespace savr::basketball {

// Resolve engine entry points. Call once with the libGame.so NOLOAD handle.
void Install(void* handle);

// Cheat action: spawn the ball in front of the player (or bring the existing
// ball back if one is already alive).
void SpawnBall();

// Per-gameplay-frame update from the input pump: grab / carry / throw and
// the hoop score probe.
void Update();

// Bullet notification from the weapon pipeline: a shot from `origin` along
// `dir` (unit direction when dirIsTarget=false, or an absolute end point
// when dirIsTarget=true). A hit deflates the ball: modest impulse, it goes
// flat over a second, stops bouncing, sinks in water and despawns later.
void OnBulletShot(const float origin[3], const float dir[3],
                  bool dirIsTarget);

// User-calibratable physics, persisted in vr_basketball.ini. Values are
// integers in UI-friendly units (percent / centimetres / decimetres per
// second); the simulation converts internally.
enum PhysicsField {
    PHYS_BOUNCE = 0,     // floor restitution, percent of speed kept
    PHYS_FRICTION,       // horizontal speed kept per bounce, percent
    PHYS_THROW_POWER,    // hand-speed multiplier, percent
    PHYS_THROW_MAX,      // throw speed cap, dm/s (90 = 9.0 m/s)
    PHYS_MAGNET_RANGE,   // call-the-ball reach, cm
    PHYS_MAGNET_SPEED,   // pull step per frame, cm
    PHYS_GRAB_RANGE,     // in-hand snap distance, cm
    PHYS_BALL_RADIUS,    // floor contact height, cm
    PHYS_HIT_POWER,      // open-hand strike multiplier, percent
    PHYS_HIT_RANGE,      // strike contact distance, cm
    PHYS_CASUAL_THROW,   // 1 = trigger-aimed arc throw while holding
    PHYS_CASUAL_SPEED,   // arc throw speed, dm/s
    PHYS_CASUAL_PRESSURE,// 1 = arc speed scales with trigger pressure
    PHYS_SHOW_PHYSICS,   // 1 = draw the rim collider + ball contact circle
    PHYS_AUTO_RETURN,    // 1 = empty-hand trigger recalls the ball to hand
    PHYS_RIM_FORWARD,    // rim centre offset from the stand origin, cm,
                         // along the hoop facing (all hoops at once)
    PHYS_RIM_HEIGHT,     // rim height offset from the calibrated rimZ, cm
    PHYS_RIM_SIDE,       // rim sideways offset (perpendicular to facing), cm
    PHYS_RIM_RADIUS,     // rim ring radius, cm
    PHYS_BULLET_DEFLATE, // 1 = bullets deflate the ball; 0 = impulse only
    PHYS_FIELD_COUNT
};
int         GetPhysicsValue(int field);
void        AdjustPhysics(int field, int direction);
const char* PhysicsFieldName(int field);

// One master (right-hand) ball contact pose.  The left hand is derived by
// mirroring this calibration, so players never have to tune the same contact
// twice. Position values are millimetres; rotations are degrees.
enum HandCalibField {
    HAND_CALIB_X = 0,
    HAND_CALIB_Y,
    HAND_CALIB_Z,
    HAND_CALIB_PITCH,
    HAND_CALIB_YAW,
    HAND_CALIB_ROLL,
    HAND_CALIB_FIELD_COUNT
};
int         GetHandCalibValue(int field);
void        AdjustHandCalib(int field, int direction);
const char* HandCalibFieldName(int field);
void        ResetHandCalibration();

// Calibration preview keeps a real ball rigidly in the right palm while the
// submenu is open. This is intentionally separate from normal grab/throw input.
void BeginHandCalibration();
void EndHandCalibration();
bool HandCalibrationActive();

}  // namespace savr::basketball
