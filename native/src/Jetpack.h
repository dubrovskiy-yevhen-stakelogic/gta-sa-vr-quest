#pragma once

#include <cstdint>

#include "Xr.h"

// Immersive jetpack (rocket belt). The retail CTaskSimpleJetPack is fully
// intact but reads the flat gamepad: on foot the VR port never fed it, so the
// jetpack was uncontrollable. This turns the two controllers into the belt's
// two handles and writes the exact pad fields ProcessControlInput reads
// (Cross=climb, Square=descend, LeftStickY=forward-tilt, LeftStickX=turn,
// Triangle=drop). Two control schemes, switchable from the LOCOMOTION menu.
namespace savr::jetpack {

// Realistic = handheld jet turbines, true vectored physics. Reliable = simple
// trigger-throttle + stick control.
enum class Mode { Realistic, Reliable };

// Resolve CPedIntelligence::GetTaskJetPack from the loaded libGame.so.
void Init(void* libGameHandle);

// True while the player ped is running the jetpack task this frame.
bool IsActive();

// The active CTaskSimpleJetPack (or null). Exposed so the renderer can draw the
// belt's clump (m_JetPackClump at task+0x48) in the eyes while CJ is hidden.
void* ActiveTask();

// Arm/disarm the B-drop lockout from the menu loop, which runs EVERY frame
// (including while game input is blocked and WritePad is skipped). While any VR
// menu page is open B is "claimed" by the menu; the belt can only be dropped
// once B has been fully released after the menu closes. Call each frame with the
// live menu-open state and the raw B-button state.
void NoteMenuInput(bool anyMenuOpen, bool bDown);

// Write the immersive control into CPad::NewState (the int16 array at the start
// of CPad). `headYaw` is the recentered local head yaw, used to make "turn"
// relative to where the player faces. Call from OnUpdatePads INSTEAD of the
// on-foot locomotion stick write while IsActive() is true.
void WritePad(std::int16_t* newState, const xr::InputState& in, float headYaw);

// JETPACK submenu control-scheme toggle (cycles Realistic<->Reliable).
Mode        GetMode();
void        CycleMode();
const char* ModeName();

// Realistic uses the true vectored hand-thrust physics; Reliable uses the
// simple pad scheme. Kept as a helper so callers read intent, not the enum.
bool        IsVectoredMode();

// Prevent dropping the belt with B while airborne (default ON) so a stray B in
// flight can't strip the jetpack mid-air. Drop then only works when standing.
bool        LockInAir();
void        SetLockInAir(bool on);
void        ToggleLockInAir();

// Turbine power: scales the vectored thrust. Percent in [50,200], default 100.
int         TurbinePowerPercent();
float       TurbinePower();          // == percent / 100
void        AdjustTurbinePower(int step);


}  // namespace savr::jetpack
