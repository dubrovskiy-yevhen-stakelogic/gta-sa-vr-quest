#pragma once

namespace savr::locomotion {

enum MovementMode {
    MOVEMENT_BODY = 0,
    MOVEMENT_HEAD,
    MOVEMENT_HEAD_TURN,
    MOVEMENT_COUNT
};

enum TurnMode {
    TURN_SMOOTH = 0,
    TURN_SNAP,
    TURN_COUNT
};

void Init();

int GetMovementMode();
const char* MovementModeName();
void CycleMovementMode(int direction);

int GetTurnMode();
const char* TurnModeName();
void CycleTurnMode(int direction);

int GetTurnSensitivityPercent();
void AdjustTurnSensitivity(int direction);
int GetSnapAngleDegrees();
void AdjustSnapAngle(int direction);

bool HeadBobEnabled();
void ToggleHeadBob();

// Arm-swing running: pump both arms up and down to move forward on foot
// (vigorous swings sprint). Default ON; the toggle is the comfort opt-out.
bool GestureRunEnabled();
void ToggleGestureRun();

// Physical swim strokes: a big two-hand stroke throws the swimmer forward,
// dog-paddling keeps a slow crawl. Same opt-out contract as GestureRun.
bool GestureSwimEnabled();
void ToggleGestureSwim();

// Canopy/skydive steering rotates the camera with the parachute (default ON —
// the point of physical risers). OFF = comfort mode: the model turns while
// the camera holds its heading.
bool ParachuteCameraFollow();
void ToggleParachuteCameraFollow();

// Keep a parachute permanently in the inventory (re-given whenever missing).
bool AutoParachuteEnabled();
void ToggleAutoParachute();

// Canopy input source: false = DEFAULT (sticks), true = IMMERSIVE (physical
// riser toggles only; sticks muted under the canopy).
bool ParachuteControlImmersive();
void ToggleParachuteControl();

// Realistic flight camera: the view base rolls/pitches with the airframe
// (default OFF = level horizon comfort mode).
bool FlightCameraTilt();
void ToggleFlightCameraTilt();

// Rotate the physical left stick from the body frame into the current HMD
// heading for HEAD and HEAD TURN EXP modes.
void TransformMoveStick(float localHeadYaw, float* x, float* y);

} // namespace savr::locomotion
