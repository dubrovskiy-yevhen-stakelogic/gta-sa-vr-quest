#pragma once

namespace savr::hud {

// The Quest Vice City terminology is retained deliberately: CLASSIC draws the
// original 2D HUD crops in front of the player; IMMERSIVE wears the same two
// real crops as wrist panels on the tracked hands (radar on the left forearm,
// the status block on the right), exactly like the Vice City Quest port.
enum Preset {
    IMMERSIVE = 0,
    CLASSIC   = 1,
};

// Objective indicators are independent from the flat HUD. ORIGINAL replays
// SA's own animated 3D marker model in each stereo eye; HIGHLIGHT draws a
// restrained depth-tested cage around the marked target; BOTH enables both.
enum ObjectiveMarkerMode {
    OBJECTIVE_OFF = 0,
    OBJECTIVE_ORIGINAL,
    OBJECTIVE_HIGHLIGHT,
    OBJECTIVE_BOTH,
    OBJECTIVE_MODE_COUNT
};

// The classic HUD is intentionally minimal: one real crop for the radar, one
// real crop containing the complete top-right status block, plus the live
// text channels rendered with the APK font. Weapon/ammo/armour/wanted/money
// are already inside HEALTH and must never be drawn as overlapping extra
// crops. TIMERS carries the mission clock/counters published from
// CUserDisplay::OnscnTimer.
enum Element {
    RADAR = 0,
    HEALTH,
    MESSAGES,
    HELP_TEXT,
    TIMERS,
    ELEMENT_COUNT
};

// Direct text layers render published strings with the APK font; they have no
// source crop in the capture and no wrist panel.
inline bool IsDirectTextElement(int element) {
    return element == MESSAGES || element == HELP_TEXT || element == TIMERS;
}

// Wrist/dash placement contexts. HAND wears the panel on the tracked wrist on
// foot; VEHICLE anchors it to the steering-wheel centre like the Vice City
// dashboard instrumentation. Each has its own saved calibration.
enum WristSlot {
    WRIST_SLOT_HAND = 0,
    WRIST_SLOT_VEHICLE,
    // Placement while that element's hand holds a weapon one-handed (the
    // panel slides beside the gripping hand)...
    WRIST_SLOT_WEAPON,
    // ...and while a weapon is held with BOTH hands: both panels anchor to
    // the primary hand with these per-element offsets.
    WRIST_SLOT_TWOHAND,
    WRIST_SLOT_COUNT
};

enum ElementField {
    ELEMENT_ENABLED = 0,
    SOURCE_X,
    SOURCE_Y,
    SOURCE_WIDTH,
    SOURCE_HEIGHT,
    SCREEN_X,
    SCREEN_Y,
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    ELEMENT_SCALE,
    ELEMENT_FIELD_COUNT
};

// IMMERSIVE wrist-panel placement, the same seven degrees of freedom the Vice
// City Quest port calibrates: millimetre (0.1 cm step) offsets ride the
// anchor's own axes (along the forearm / across it / off the skin — or, in
// the vehicle slot, toward the driver / across the dash / above the wheel),
// then three rotations turn the panel plane, then a width multiplier. Only
// RADAR and HEALTH wear panels; the text elements ignore these.
enum WristField {
    WRIST_ALONG_MM = 0,
    WRIST_ACROSS_MM,
    WRIST_LIFT_MM,
    WRIST_PITCH_DEG,
    WRIST_YAW_DEG,
    WRIST_ROLL_DEG,
    WRIST_SCALE_TENTHS,
    WRIST_FIELD_COUNT
};

struct WristSettings {
    int alongMm{}, acrossMm{}, liftMm{};
    int pitchDeg{}, yawDeg{}, rollDeg{};
    int scaleTenths{10}; // panel width multiplier, 10 == 1.0x
};

struct ElementSettings {
    bool enabled{};
    int sourceX{}, sourceY{}, sourceWidth{}, sourceHeight{}; // 1024x576 pixels
    int screenX{}, screenY{}, screenWidth{}, screenHeight{}; // 0..100 percent
    int scaleTenths{10}; // 1 == 0.1x; applied around the configured centre
};

void Init();

Preset CurrentPreset();
const char* PresetName();
void SetPreset(Preset preset);       // persists immediately
void TogglePreset();                 // persists immediately

bool GameplayHudEnabled();
void SetGameplayHudEnabled(bool enabled); // persists immediately
void ToggleGameplayHud();                  // persists immediately

// IMMERSIVE wrist/dashboard panels normally reveal only while the player
// looks at them. This option keeps the distance safety fade but disables the
// gaze-direction fade.
bool GazeAutoHideEnabled();
void SetGazeAutoHideEnabled(bool enabled); // persists immediately
void ToggleGazeAutoHide();                 // persists immediately

// Single predicate for the renderer.  Keeping the master switch separate from
// the preset matches reference Quest build and allows CLASSIC placement to stay selected while
// the player temporarily hides the HUD.
bool ShouldRenderClassicHud();

// IMMERSIVE preset: the radar and status crops become quads worn on the
// tracked wrists instead of screen-space overlays. Same master switch.
bool ShouldRenderWristHud();

ObjectiveMarkerMode GetObjectiveMarkerMode();
const char* ObjectiveMarkerModeName();
void SetObjectiveMarkerMode(ObjectiveMarkerMode mode); // persists immediately
void CycleObjectiveMarkerMode(int direction);          // persists immediately
bool ObjectiveMarkersIncludeOriginal();
bool ObjectiveMarkersIncludeHighlight();

const char* ElementName(int element);
ElementSettings GetElementSettings(int element);
// vehicleModel: for WRIST_SLOT_VEHICLE, the current vehicle's model id gives a
// per-vehicle override (created on first adjustment; the shared Dash* values
// are its defaults). Pass -1 (or any slot!=VEHICLE) for the shared values.
WristSettings GetWristSettings(int element, int slot, int vehicleModel);
int CalibrationElement();
void CycleCalibrationElement(int direction);
void SetElementEnabled(int element, bool enabled);
// direction carries magnitude: +N/-N applies N increments (menus accelerate
// while a value is held). Both persist immediately.
void AdjustElementField(int element, int field, int direction);
void AdjustWristField(int element, int field, int direction, int slot,
                      int vehicleModel);
void ResetElement(int element); // also resets both wrist placement slots

}  // namespace savr::hud
