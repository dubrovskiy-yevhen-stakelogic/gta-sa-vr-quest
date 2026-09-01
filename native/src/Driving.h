#pragma once

namespace savr::xr {
struct InputState;
}

namespace savr::driving {

// Vice City Quest exposes ordinary pad steering and a physical cockpit as two
// separate modes. DEFAULT leaves steering on the stick; IMMERSIVE uses the
// tracked virtual wheel.
enum Mode {
    MODE_DEFAULT = 0,
    MODE_IMMERSIVE,
    // Vice City Quest parity: steer cars/bikes by ROTATING the controller
    // (aim-pose yaw against a reference captured on the first throttle
    // press). No wheel grab; hands stay free.
    MODE_MOTION,
    MODE_COUNT
};

// Pedal bicycles have two distinct immersive input styles. Motorcycles do not
// use this selector: they always keep the physical VC-style handlebars, while
// their per-model accelerator can be held R2 (default) or a twist grip.
enum BicycleImmersiveMode {
    BICYCLE_MOTION = 0,
    BICYCLE_HANDLEBAR_TRIGGER,
    BICYCLE_MODE_COUNT
};

// Stored per motorcycle model. Mode 0 keeps the physical twist grip and mode 1
// is the ordinary held-R2 default. Pedal bicycles in HANDLEBAR + R2 always use
// a plain held trigger and deliberately ignore this selector.
enum BikeAcceleratorMode {
    BIKE_ACCEL_PHYSICAL_OR_TAP = 0,
    BIKE_ACCEL_HOLD_TRIGGER,
    BIKE_ACCEL_MODE_COUNT
};

enum VehicleType {
    VEHICLE_CAR = 0,
    VEHICLE_BIKE,
    VEHICLE_PLANE,   // fixed-wing + helicopters (shared settings page)
    VEHICLE_BOAT,
    VEHICLE_TYPE_COUNT,
    VEHICLE_NONE = -1
};

enum OffsetField {
    F_SIDE = 0,
    F_DISTANCE,
    F_HEIGHT,
    F_BIKE_SEAT_DISTANCE,
    F_BIKE_SEAT_HEIGHT,
    F_IMMERSIVE_CAR_SEAT_DISTANCE,
    F_IMMERSIVE_CAR_SEAT_HEIGHT,
    F_IMMERSIVE_BIKE_SEAT_DISTANCE,
    F_IMMERSIVE_BIKE_SEAT_HEIGHT,
    F_WHEEL_SIDE,
    F_WHEEL_DISTANCE,
    F_WHEEL_HEIGHT,
    F_WHEEL_RADIUS,
    F_BIKE_HALF_WIDTH,
    F_BIKE_DISTANCE,
    F_BIKE_HEIGHT,
    F_PLANE_SEAT_DISTANCE,
    F_PLANE_SEAT_HEIGHT,
    F_BOAT_SEAT_DISTANCE,
    F_BOAT_SEAT_HEIGHT,
    F_IMMERSIVE_BOAT_SEAT_DISTANCE,
    F_IMMERSIVE_BOAT_SEAT_HEIGHT,
    // Appended for config-array compatibility with existing installs.  Each
    // vehicle category/mode now owns a lateral seat offset just like forward
    // and height; the old F_SIDE remains DEFAULT CAR side.
    F_BIKE_SEAT_SIDE,
    F_IMMERSIVE_CAR_SEAT_SIDE,
    F_IMMERSIVE_BIKE_SEAT_SIDE,
    F_PLANE_SEAT_SIDE,
    F_BOAT_SEAT_SIDE,
    F_IMMERSIVE_BOAT_SEAT_SIDE,
    F_COUNT
};

// Published to the OpenXR present thread in LOCAL tracking space.
struct WheelVisualState {
    bool active{};
    bool visible{};
    bool bike{};
    // Plane: rendered as a ram-horn yoke (bottom arc + two upright grips)
    // instead of the closed car ring.
    bool yoke{};
    // The wheel/handlebar centre frame is valid for anchoring the dashboard
    // HUD panels, even when the immersive wheel interaction itself is off
    // (DEFAULT driving publishes geometry only, with active=false).
    bool dashAnchorValid{};
    // When enabled, a grabbed bike hand uses the full live handlebar frame,
    // including bike pitch and roll. Disabled keeps controller wrist rotation.
    bool bikeHandsFollowTilt{};
    // DEFAULT driving uses the seated game body; IMMERSIVE owns tracked hands.
    bool trackedHandsEnabled{true};
    bool grabbed[2]{};
    bool markerVisible[2]{};
    int modelId{-1};
    float center[3]{};
    float right[3]{};
    float up[3]{};
    float normal[3]{};
    float radius{};
    float physicalAngle{};
    float steering{};
    // Visual-only motorcycle throttle twist, normalized to the current
    // vehicle's handling top speed. It never feeds the accelerator query.
    float throttleVisual{};
    float handlePosition[2][3]{};
    float handleRight[2][3]{};
    float handleUp[2][3]{};
    float handleForward[2][3]{};
};

// Per-vehicle-type camera view for stereo driving. FIRST anchors the eyes at
// the seat; THIRD renders the world from the game's own chase camera (the
// Vice City DEFAULT behaviour) with the physical head on top. The Rhino tank
// forces THIRD — nothing is visible from inside its hull.
enum ViewMode {
    VIEW_FIRST_PERSON = 0,
    VIEW_THIRD_PERSON,
    VIEW_MODE_COUNT
};

enum MenuItem {
    MENU_VEHICLE_TYPE = 0,
    MENU_DRIVING_TYPE,
    MENU_CAMERA_VIEW,
    MENU_BICYCLE_MODE,
    MENU_BIKE_ACCELERATOR,
    MENU_GLOBAL_SIDE,
    MENU_GLOBAL_FORWARD,
    MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE,
    MENU_MODEL_FORWARD,
    MENU_MODEL_HEIGHT,
    MENU_CONTROL_CALIBRATION,
    MENU_YOKE_SENSITIVITY,
    MENU_HANDLE_HIGHLIGHTS,
    MENU_BIKE_HAND_TILT,
    MENU_LOCAL_HORIZON,
    MENU_BIKE_VISUAL_LEAN,
    MENU_KEEP_RIDER_ON_FLIPS,
    MENU_WHEEL_VISIBLE,
    MENU_INTERIOR_GLASS,
    MENU_DRIVEBY_AIM,
    MENU_CAR_CAMERA_TILT,
    MENU_BOAT_CAMERA_TILT,
    MENU_RESET,
    MENU_BACK
};

enum ControlCalibrationField {
    CONTROL_OFFSET_X = 0,
    CONTROL_OFFSET_Y,
    CONTROL_OFFSET_Z,
    CONTROL_ROT_X,
    CONTROL_ROT_Y,
    CONTROL_ROT_Z,
    CONTROL_FIELD_COUNT
};

// Per-model calibration of the VIRTUAL WHEEL/HELM/YOKE itself (the Vice City
// wheel-calibration set): position in the vehicle frame, ring radius and the
// plane's pitch/yaw/roll. Applies to cars, boats and planes alike; values are
// deltas on top of the shared global wheel preset. cm and half-degrees.
enum WheelCalibrationField {
    WHEEL_CAL_SIDE = 0,
    WHEEL_CAL_FORWARD,
    WHEEL_CAL_HEIGHT,
    WHEEL_CAL_RADIUS,
    WHEEL_CAL_PITCH,
    WHEEL_CAL_YAW,
    WHEEL_CAL_ROLL,
    WHEEL_CAL_FIELD_COUNT
};

void Init();
void ResetDefaultPreset();

int  GetOffsetCm(int field);
void AdjustOffsetCm(int field, int direction);
const char* FieldLabel(int field);

int GetMode();
const char* GetModeName();
void CycleMode(int direction);
int GetCarMode();
int GetBikeMode();
const char* GetCarModeName();
const char* GetBikeModeName();
void CycleCarMode(int direction);
void CycleBikeMode(int direction);

const char* VehicleTypeName(int vehicleType);
int GetModeForVehicleType(int vehicleType);
const char* GetModeNameForVehicleType(int vehicleType);
void CycleModeForVehicleType(int vehicleType, int direction);
int GetCameraViewForVehicleType(int vehicleType);
const char* GetCameraViewName(int vehicleType);
void CycleCameraView(int vehicleType, int direction);
// Shared helicopter+plane camera view (several planes have no cockpit model).
// VEHICLE_PLANE's CAMERA VIEW row maps here; VEHICLE_BOAT has its own.
int GetAirView();
const char* GetAirViewName();
void CycleAirView(int direction);
// True when the active vehicle should render from the game's chase camera:
// the per-type THIRD PERSON setting, or always for the Rhino tank.
bool ThirdPersonViewActive();
int GetBicycleImmersiveMode();
const char* GetBicycleImmersiveModeName();
void CycleBicycleImmersiveMode(int direction);
int GetCurrentBikeAcceleratorMode();
const char* GetCurrentBikeAcceleratorModeName();
void CycleCurrentBikeAcceleratorMode(int direction);

int GetMenuItemCount(int vehicleType);
int GetMenuItemForRow(int vehicleType, int row);
bool IsMenuItemAvailable(int vehicleType, int item);

int GetGlobalSeatForwardCm(int vehicleType);
int GetGlobalSeatHeightCm(int vehicleType);
int GetGlobalSeatSideCm(int vehicleType);
void AdjustGlobalSeatForwardCm(int vehicleType, int direction);
void AdjustGlobalSeatHeightCm(int vehicleType, int direction);
void AdjustGlobalSeatSideCm(int vehicleType, int direction);
int GetCurrentModelSeatForwardCm(int vehicleType);
int GetCurrentModelSeatHeightCm(int vehicleType);
int GetCurrentModelSeatSideCm(int vehicleType);
void AdjustCurrentModelSeatForwardCm(int vehicleType, int direction);
void AdjustCurrentModelSeatHeightCm(int vehicleType, int direction);
void AdjustCurrentModelSeatSideCm(int vehicleType, int direction);
int GetActiveSeatForwardCm();
int GetActiveSeatHeightCm();
int GetActiveSeatSideCm();
int GetActiveVehicleType();
int GetActiveVehicleModelId();
const char* GetActiveVehicleModelName();
bool HasCurrentModelForType(int vehicleType);
void ResetVehiclePreset(int vehicleType);

bool IsControlCalibrationAvailable(int vehicleType);
int GetControlCalibrationValue(int hand, int field);
void AdjustControlCalibrationValue(int hand, int field, int direction);
const char* ControlCalibrationFieldName(int field);

int GetWheelCalibrationValue(int field);
void AdjustWheelCalibrationValue(int field, int direction);
const char* WheelCalibrationFieldName(int field);

// Physical yoke pitch response (planes): percent of the default travel.
int GetYokeSensitivityPercent();
void AdjustYokeSensitivity(int direction);

bool IsWheelVisible();
void ToggleWheelVisible();
bool AreHandleHighlightsEnabled();
void ToggleHandleHighlights();
bool DoBikeHandsFollowTilt();
void ToggleBikeHandsFollowTilt();
bool IsBikeHorizonLocked();
void ToggleBikeHorizonLock();
int GetBikeVisualLeanPercent();
void AdjustBikeVisualLeanPercent(int direction);
bool KeepRiderOnFlipsEnabled();
void ToggleKeepRiderOnFlips();
// Read the same scaled visual frame used by the rendered bike and attached
// hands. The ordinary CEntity matrix deliberately contains no rendered lean.
bool GetActiveBikeVisualBasis(float right[3], float forward[3], float up[3]);
bool IsInteriorGlassHidden();
void ToggleInteriorGlass();
// Car camera motion: LEVEL (default) keeps the yaw-only comfort base with a
// level horizon; FULL TILT takes the whole car basis, so hills, jumps and
// body roll throw the view with the car (the aircraft FULL TILT of cars).
bool CarCameraTiltEnabled();
void ToggleCarCameraTilt();
// Boats (vehicle appearance 4): LEVEL horizon vs full-hull tilt.
bool BoatCameraTiltEnabled();
void ToggleBoatCameraTilt();
// DEFAULT-driving drive-by style. CLASSIC keeps the stock grip+B auto-aim;
// IMMERSIVE draws a one-handed sidearm and aims/fires it with the tracked hand
// (like immersive driving) while the car stays on ordinary stick control.
// Global toggle, persisted with the loadout.
bool IsDrivebyAimImmersive();
void ToggleDrivebyAimImmersive();
// True when the current vehicle should use the physical (hand-aimed) weapon
// system: immersive DRIVING, or any driving with the immersive drive-by option
// on. Weapon/fire/render gates read this; driving CONTROL never does.
bool VehicleWeaponsImmersive();
bool ShouldUseTrackedHands();
void SetMenuPreview(bool active, int calibrationHand = -1);

void UpdateInput(const xr::InputState& input, bool gameplay, bool blocked);
void ResetInteraction();
// Rebuild a bike's visual sockets after the current camera/body frame has been
// published, eliminating a one-frame mismatch during fast vehicle roll.
void RefreshVisualTracking();

bool GetWheelVisualState(WheelVisualState* out);
// Motion steering hand (0 left / 1 right), persisted as MotionSteeringHand.
int  GetMotionSteeringHand();
void ToggleMotionSteeringHand();
// True while MOTION steering owns the wheel (car/bike, mode = MOTION); the
// input pump must then feed the provider the motion value, not the stick.
bool MotionSteeringActive();
float MotionSteeringValue();

bool GetGrabbedHandVisual(int hand, float position[3], float right[3],
                          float up[3], float forward[3]);

} // namespace savr::driving
