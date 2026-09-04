#pragma once

// VR holster: seven configurable, body-relative weapon points. The point
// metadata deliberately contains no game types so the renderer can use it when
// building the actual weapon matrices.
namespace savr::holster {

enum Point : int {
    WAIST_LEFT = 0,
    WAIST_RIGHT,
    CHEST_LEFT,
    CHEST_RIGHT,
    CHEST_CENTER,
    BACK_LEFT,
    BACK_RIGHT,
    POINT_COUNT,
};

struct PointMetadata {
    const char* name;
    float lateral;   // metres along body-right
    float vertical;  // metres along world-up
    float depth;     // metres along body-forward
    bool behind;
};

// Loads /sdcard/.../vr_holsters.ini. All public accessors are also lazy-safe,
// but calling this once during native initialisation avoids first-use I/O in a
// render path.
void Init();

int PointCount();
const PointMetadata* Metadata(int point);
const char* PointName(int point);
bool IsPointFixed(int point);

// Per-socket visibility/interaction switch with three states (cycled from the
// holster menu). SHOWN renders the marker + on-body weapon model and can be
// grabbed; HIDDEN renders neither but can STILL be grabbed (for players who
// know their layout); OFF renders neither and cannot be grabbed at all. The
// slot assignment is preserved across all three.
enum PointVisibility : int {
    POINT_SHOWN  = 0,
    POINT_HIDDEN = 1,
    POINT_OFF    = 2,
};
int  PointVisibilityState(int point);
bool PointVisible(int point);          // == SHOWN (marker + on-body model)
bool PointGrabbable(int point);        // != OFF
const char* PointVisibilityName(int point);   // "SHOW" / "HIDE" / "OFF"
void CyclePointVisibility(int point, int dir);

// Configured SA eWeaponSlot for a body point. -1 means EMPTY. The centre chest
// point is always slot 8 (THROWABLE), even if an old/malformed file says
// otherwise.
int PointSlot(int point);
int FindPointForSlot(int slot);
const char* SlotName(int slot);

// Assigns EMPTY (-1) or an SA weapon category (slots 1..7) to a configurable
// point and persists it. If the requested category already lives on another
// point, the two point values are swapped in one persisted operation; this keeps the loadout
// duplicate-free while allowing a weapon to be moved without first clearing
// its old point. The fixed centre throwable point cannot be changed.
// Returns false for an invalid/fixed point or an invalid slot.
bool SetPointSlot(int point, int slot);

// Exchanges two configurable body points and persists the result. EMPTY is a
// normal value, so this also provides an explicit move-to-empty operation.
// Returns false if either point is invalid/fixed.
bool SwapPointSlots(int firstPoint, int secondPoint);

// Cycles only weapon categories the player currently owns and which are not
// assigned to another point. EMPTY is deliberately not a cycle choice: removing
// a configured category must be an explicit action. The result is persisted.
// Returns the resulting slot (or -1 for an invalid/empty point).
int CyclePointSlot(int point, int direction);

// Explicitly removes the category from a configurable point and persists the
// result. The fixed centre throwable point cannot be cleared.
bool ClearPointSlot(int point);

// Cyan grip/reach markers are a visual aid only: hiding them does not disable
// physical holster grabbing. The choice is persisted with the loadout.
bool GripMarkersEnabled();
void SetGripMarkersEnabled(bool enabled);
void ToggleGripMarkers();

// Reach for pulling a weapon off a body socket, player-adjustable from the
// loadout menu (persisted). Metres variant feeds the PhysicalWeapon grab test.
int   GrabRadiusCm();
float GrabRadiusMetres();
void  AdjustGrabRadiusCm(int direction);

// Vice City WeaponGripLock: with the lock on, opening the grip away from the
// item's socket keeps it in the hand (only a deliberate return at the socket
// puts it away). Persisted with the loadout; PhysicalWeapon reads it per tick.
bool GripLockEnabled();
void ToggleGripLock();

// Dedicated holstered-model calibration preview. Entering captures the current
// non-unarmed active weapon (falling back to the last real active weapon), keeps
// its type stable while the page is open, and identifies the body point where
// the renderer must force-draw it even though its inventory slot is active/held.
// CycleCalibrationPreviewWeapon is optional page UX for stepping through other
// owned weapons without losing the initial capture behaviour.
bool BeginCalibrationPreview();
void EndCalibrationPreview();
bool CalibrationPreviewActive();
bool GetCalibrationPreview(int* point, int* slot, int* weaponType);
bool CycleCalibrationPreviewWeapon(int direction);

// Physical-weapon ownership calls this whenever a real weapon enters a hand.
// It keeps submenu capture reliable even after the legacy active-slot switcher
// is replaced; invalid/unarmed values are ignored.
void RememberActiveWeapon(int slot, int weaponType);

// Call once per frame on the GameThread (after the game tick), in gameplay
// only. Publishes markers only for configured weapons the player actually owns;
// gripping one switches the active weapon to that configured slot.
void Update();

}  // namespace savr::holster
