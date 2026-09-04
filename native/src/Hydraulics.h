#pragma once

// Lowrider hydraulics from VR. While the player drives a car that actually has
// hydraulics installed, holding BOTH grips turns the right stick into the
// hydraulics control: up lifts the front, down the rear, left/right lift that
// side. The right stick is otherwise unused in a ground vehicle (steering is
// the left stick and drive-by uses a single grip), so the chord is free.
namespace savr::hydraulics {

// Resolve the beat table. The CPad car-gun accessors are NOT hooked here:
// Driving.cpp already owns those two trampolines (for the Hydra nozzles), and a
// second hook cannot be layered on top of the first. Driving's handlers consult
// IsActive()/AxisLeftRight()/AxisUpDown() instead.
void Install(void* libGameHandle);

// Publish this frame's intent from the game thread (VrCamera::OnUpdatePads).
// `active` must already mean "in a hydraulics-equipped car with both grips
// held"; x/y are the RAW right stick in [-1,1] (up positive).
void SetStick(bool active, float x, float y);

// True while the override is feeding the game.
bool IsActive();

// The values Driving's CPad car-gun hooks must return while IsActive(). Already
// in the game's own units and sign convention (leftRight<0 lifts the LEFT side,
// upDown<0 lifts the FRONT).
int AxisLeftRight();
int AxisUpDown();

// Rhythm-minigame beat readout (lowrider dance / nightclub). Returns the
// direction the player must push next - 0 none, 1 UP, 2 DOWN, 3 LEFT, 4 RIGHT -
// and writes the milliseconds until that beat. The stock arrows are SCM sprites
// that never reach the VR eyes, so the mod draws its own from this.
int PollBeat(int* msUntilOut);

// Lowrider dance score. The stock counters go through the mobile touch-HUD
// widget layer (opcode 0x0A8A -> CTouchInterface::SetWidgetInfo, widgets 103
// "LOWR1" / 104 "LOWR2"), which never reaches the VR eyes - so read the script
// globals directly and let the mod print them. Returns false when the minigame
// is not running. There is no round timer: the round ends with the beat track.
bool GetDanceScore(int* playerOut, int* opponentOut);

}  // namespace savr::hydraulics
