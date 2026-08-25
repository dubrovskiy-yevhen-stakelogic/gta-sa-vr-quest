#include "Locomotion.h"

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace savr::locomotion {
namespace {

const char* const kPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_locomotion.ini";

std::once_flag g_initOnce;
std::mutex g_saveMutex;
std::atomic<int> g_movement{MOVEMENT_HEAD};
std::atomic<int> g_turn{TURN_SMOOTH};
std::atomic<int> g_turnSensitivity{100};
std::atomic<int> g_snapAngle{30};
std::atomic<bool> g_headBob{false};
// Gesture locomotion (arm-swing run / physical swim strokes). Default ON —
// they only engage on deliberate motion; the toggles are the comfort opt-out.
std::atomic<bool> g_gestureRun{true};
std::atomic<bool> g_gestureSwim{true};
// Default ON: canopy/skydive turns rotate the view with the parachute (the
// point of physical risers). OFF is the comfort fallback for motion-sensitive
// players — the model turns while the camera holds its heading.
std::atomic<bool> g_parachuteCameraFollow{true};
// Keep a parachute permanently in the inventory (re-given whenever missing),
// so no cheat-menu trip is needed before a jump. Enabled in release defaults.
std::atomic<bool> g_autoParachute{true};
// Canopy input source. DEFAULT = the sticks steer (script joystick path);
// IMMERSIVE = only the physical riser toggles steer, sticks are muted so the
// two never fight. Release defaults use IMMERSIVE.
std::atomic<bool> g_parachuteImmersiveControl{true};
// Realistic flight: the camera base rolls/pitches WITH the airframe instead
// of staying level while the cockpit rotates around the pilot. Release
// defaults enable the aircraft-relative camera.
std::atomic<bool> g_flightCameraTilt{true};

void Save() {
    std::lock_guard<std::mutex> lock(g_saveMutex);
    FILE* file=std::fopen(kPath,"w");
    if (!file) { LOGW("[locomotion] could not save %s",kPath); return; }
    std::fprintf(file,"MovementMode=%d\n",g_movement.load());
    std::fprintf(file,"TurnMode=%d\n",g_turn.load());
    std::fprintf(file,"TurnSensitivityPercent=%d\n",g_turnSensitivity.load());
    std::fprintf(file,"SnapAngleDegrees=%d\n",g_snapAngle.load());
    std::fprintf(file,"HeadBob=%d\n",g_headBob.load()?1:0);
    std::fprintf(file,"GestureRun=%d\n",g_gestureRun.load()?1:0);
    std::fprintf(file,"GestureSwim=%d\n",g_gestureSwim.load()?1:0);
    std::fprintf(file,"ParachuteCameraFollow=%d\n",
                 g_parachuteCameraFollow.load()?1:0);
    std::fprintf(file,"AutoParachute=%d\n",g_autoParachute.load()?1:0);
    std::fprintf(file,"ParachuteImmersiveControl=%d\n",
                 g_parachuteImmersiveControl.load()?1:0);
    std::fprintf(file,"FlightCameraTilt=%d\n",g_flightCameraTilt.load()?1:0);
    std::fclose(file);
}

void Load() {
    int movement=MOVEMENT_HEAD,turn=TURN_SMOOTH,sensitivity=100,snap=30,bob=0;
    int chuteFollow=1,autoChute=1,chuteImmersive=1,flightTilt=1;
    int gestureRun=1,gestureSwim=1;
    if (FILE* file=std::fopen(kPath,"r")) {
        char line[128];
        while (std::fgets(line,sizeof(line),file)) {
            int value=0;
            if (std::sscanf(line,"MovementMode=%d",&value)==1) movement=value;
            else if (std::sscanf(line,"TurnMode=%d",&value)==1) turn=value;
            else if (std::sscanf(line,"TurnSensitivityPercent=%d",&value)==1) sensitivity=value;
            else if (std::sscanf(line,"SnapAngleDegrees=%d",&value)==1) snap=value;
            else if (std::sscanf(line,"HeadBob=%d",&value)==1) bob=value;
            else if (std::sscanf(line,"GestureRun=%d",&value)==1) gestureRun=value;
            else if (std::sscanf(line,"GestureSwim=%d",&value)==1) gestureSwim=value;
            else if (std::sscanf(line,"ParachuteCameraFollow=%d",&value)==1)
                chuteFollow=value;
            else if (std::sscanf(line,"AutoParachute=%d",&value)==1)
                autoChute=value;
            else if (std::sscanf(line,"ParachuteImmersiveControl=%d",&value)==1)
                chuteImmersive=value;
            else if (std::sscanf(line,"FlightCameraTilt=%d",&value)==1)
                flightTilt=value;
        }
        std::fclose(file);
    }
    g_movement.store(std::clamp(movement,0,MOVEMENT_COUNT-1));
    g_turn.store(std::clamp(turn,0,TURN_COUNT-1));
    g_turnSensitivity.store(std::clamp(sensitivity,25,300));
    g_snapAngle.store(std::clamp(snap,15,90));
    g_headBob.store(bob!=0);
    g_gestureRun.store(gestureRun!=0);
    g_gestureSwim.store(gestureSwim!=0);
    g_parachuteCameraFollow.store(chuteFollow!=0);
    g_autoParachute.store(autoChute!=0);
    g_parachuteImmersiveControl.store(chuteImmersive!=0);
    g_flightCameraTilt.store(flightTilt!=0);
    const char* movementName=g_movement.load()==MOVEMENT_BODY?"BODY":
        (g_movement.load()==MOVEMENT_HEAD?"HEAD":"HEAD TURN EXP");
    const char* turnName=g_turn.load()==TURN_SNAP?"SNAP":"SMOOTH";
    LOGI("[locomotion] movement=%s turn=%s sensitivity=%d%% snap=%d bob=%d",
         movementName,turnName,g_turnSensitivity.load(),
         g_snapAngle.load(),g_headBob.load());
}

void EnsureInit() { std::call_once(g_initOnce,Load); }
int DirectionStep(int direction) { return direction<0?-1:1; }

} // namespace

void Init() { EnsureInit(); }
int GetMovementMode() { EnsureInit(); return g_movement.load(); }
const char* MovementModeName() {
    switch (GetMovementMode()) {
        case MOVEMENT_BODY:return "BODY";
        case MOVEMENT_HEAD:return "HEAD";
        case MOVEMENT_HEAD_TURN:return "HEAD TURN EXP";
        default:return "HEAD";
    }
}
void CycleMovementMode(int direction) {
    EnsureInit(); if (!direction) return;
    g_movement.store((g_movement.load()+MOVEMENT_COUNT+DirectionStep(direction))%MOVEMENT_COUNT);
    Save();
}
int GetTurnMode() { EnsureInit(); return g_turn.load(); }
const char* TurnModeName() { return GetTurnMode()==TURN_SNAP?"SNAP":"SMOOTH"; }
void CycleTurnMode(int direction) {
    EnsureInit(); if (!direction) return;
    g_turn.store((g_turn.load()+TURN_COUNT+DirectionStep(direction))%TURN_COUNT);
    Save();
}
int GetTurnSensitivityPercent() { EnsureInit(); return g_turnSensitivity.load(); }
void AdjustTurnSensitivity(int direction) {
    EnsureInit(); if (!direction) return;
    g_turnSensitivity.store(std::clamp(g_turnSensitivity.load()+DirectionStep(direction)*5,25,300));
    Save();
}
int GetSnapAngleDegrees() { EnsureInit(); return g_snapAngle.load(); }
void AdjustSnapAngle(int direction) {
    EnsureInit(); if (!direction) return;
    g_snapAngle.store(std::clamp(g_snapAngle.load()+DirectionStep(direction)*5,15,90));
    Save();
}
bool HeadBobEnabled() { EnsureInit(); return g_headBob.load(); }
void ToggleHeadBob() { EnsureInit(); g_headBob.store(!g_headBob.load()); Save(); }
bool GestureRunEnabled() { EnsureInit(); return g_gestureRun.load(); }
void ToggleGestureRun() {
    EnsureInit(); g_gestureRun.store(!g_gestureRun.load()); Save();
}
bool GestureSwimEnabled() { EnsureInit(); return g_gestureSwim.load(); }
void ToggleGestureSwim() {
    EnsureInit(); g_gestureSwim.store(!g_gestureSwim.load()); Save();
}
bool ParachuteCameraFollow() {
    EnsureInit(); return g_parachuteCameraFollow.load();
}
void ToggleParachuteCameraFollow() {
    EnsureInit();
    g_parachuteCameraFollow.store(!g_parachuteCameraFollow.load());
    Save();
}
bool AutoParachuteEnabled() { EnsureInit(); return g_autoParachute.load(); }
void ToggleAutoParachute() {
    EnsureInit();
    g_autoParachute.store(!g_autoParachute.load());
    Save();
}
bool ParachuteControlImmersive() {
    EnsureInit(); return g_parachuteImmersiveControl.load();
}
void ToggleParachuteControl() {
    EnsureInit();
    g_parachuteImmersiveControl.store(!g_parachuteImmersiveControl.load());
    Save();
}
bool FlightCameraTilt() { EnsureInit(); return g_flightCameraTilt.load(); }
void ToggleFlightCameraTilt() {
    EnsureInit();
    g_flightCameraTilt.store(!g_flightCameraTilt.load());
    Save();
}

void TransformMoveStick(float localHeadYaw,float* x,float* y) {
    EnsureInit();
    if (!x||!y) return;
    // The engine now maps on-foot movement against the FULL composed VR
    // camera (base + physical head) — the graphics-side heading sync made
    // the old pre-rotation double-count every physical turn, and the [vr.move]
    // diagnostics showed the ped heading oscillating: bodyYaw + 2*headYaw.
    // HEAD mode is therefore a pass-through (native camera-relative, exactly
    // like Vice City); BODY mode instead CANCELS the head component so the
    // stick stays in the body frame.
    if (g_movement.load()!=MOVEMENT_BODY) return;
    const float oldX=*x,oldY=*y;
    const float c=std::cos(-localHeadYaw),s=std::sin(-localHeadYaw);
    *x=oldX*c-oldY*s;
    *y=oldY*c+oldX*s;
}

} // namespace savr::locomotion
