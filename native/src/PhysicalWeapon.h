#pragma once

#include <cstdint>

namespace savr::xr { struct HandPose; }

// Physical ownership of the player's inventory weapons.
//
// This module deliberately owns no RenderWare/OpenGL objects.  It only advances
// the qbuild-style interaction state on the GameThread and publishes small,
// thread-safe snapshots for the existing stereo/depth renderer:
//
//   HOLSTER --fresh grip--> HELD(hand)
//   HELD --fresh grip at gun body--> HELD(other hand)
//   HELD --release at its socket--> HOLSTER
//   HELD --release elsewhere--> DROPPED(source hand)
//   DROPPED --closed free hand nearby--> HELD(catching hand)
//   DROPPED --1.8 seconds--> HOLSTER
//
// The game inventory is never removed or duplicated.  A state transition only
// changes where the renderer should draw that slot; grabbing also selects the
// corresponding CPed weapon slot so HUD/calibration/firing stay in sync.
namespace savr::physicalweapon {

constexpr int kHandCount = 2;

// A final weapon-model pose in OpenXR LOCAL space. The three basis vectors are
// stored explicitly because the tracked weapon matrix is reflected (det < 0): a
// quaternion cannot represent its handedness/cull winding. These are the FINAL
// calibrated model axes, not the raw grip pose. The renderer should publish its
// completed model pose every frame. Until the first matching publish,
// drop/transfer safely fall back to a proper raw-grip basis.
struct TrackingPose {
    float position[3]{};
    float right[3]{1.0f, 0.0f, 0.0f};
    float forward[3]{0.0f, 0.0f, -1.0f};
    float up[3]{0.0f, 1.0f, 0.0f};
};

void Init();

// Advance the state once per GameThread frame, before forwarding controller
// axes to the game. When interactionsBlocked is true (an actual VR menu), held
// ownership is preserved but no grab/drop/transfer is allowed and FireTrigger()
// returns zero. Calibration pages may request their spatial marker while the
// interactions themselves stay blocked. Holster preview follows the persistent
// marker toggle; weapon calibration forces its support socket so SUPPORT
// OFFSET/ROT can always be tuned without enabling gameplay hints first.
void Update(bool interactionsBlocked,
            bool showHolsterMarkersWhileBlocked = false,
            bool showSupportMarkerWhileBlocked = false);

// Return every transient held/dropped slot to its configured body socket.
// Loadout and runtime settings are preserved.
void ResetTransient();

// Exact controller snapshot consumed by this GameThread update. Weapon model,
// two-hand pivot/axis, stereo late hands and laser must all use this epoch rather
// than independently sampling the asynchronously updated XR poses.
bool GetHandPosesSnapshot(xr::HandPose out[kHandCount]);

// Per-hand ownership. hand: 0=LEFT, 1=RIGHT. Invalid hand returns -1.
int HeldSlot(int hand);
int HeldHandForSlot(int slot);

// One-handed guns a seated driver can draw and aim (the Vice City "immersive
// vehicle sidearm" rule). Everything else stays holstered while driving.
bool IsVehicleSidearmWeaponType(int weaponType);

// Per-frame guard: a seated bike rider must never keep a non-sidearm weapon
// selected (mounting with a longarm equipped bypasses the holster filter).
// Call from the GameThread input pump.
void EnforceBikeWeaponLimit();

// Free-fall convenience: with a parachute owned, empty tracked hands and
// ~0.7s of continuous airtime, the parachute auto-selects so the right
// trigger can open it — no manual equipping. Call from the input pump.
void AutoEquipParachute();
// Give mission gadgets (slot 9: spray/camera/extinguisher) a body point when
// none is configured, so scripted hand-outs are physically reachable.
void AutoAssignGadgetPoint();

// Put an inventory slot directly into a tracked hand (cheat/give flows). The
// weapon must exist in the ped inventory; no holster point is required. With
// the grip lock off, an open grip releases it on the next tick as usual.
bool ForceHold(int hand, int slot);

// Lock-free bitmask of hands currently holding a weapon (bit0 = LEFT,
// bit1 = RIGHT), refreshed once per Update. Safe from any lock context —
// Driving reads it inside its own state lock to keep a weapon hand off the
// steering wheel.
unsigned int HeldHandMaskRelaxed();

// PreferredHeldHand is the most recently grabbed/transferred/triggered hand.
// ActiveHeldSlot is its slot, or -1 if nothing is held.
int PreferredHeldHand();
int ActiveHeldSlot();

// qbuild-style two-handed ownership. A support hand never owns/duplicates the
// inventory slot: HeldSlot(support) remains -1 and the primary hand continues to
// drive selection, trigger and rendering. Only SA long guns return true here.
bool IsTwoHandedWeaponType(int weaponType);
int  SupportHand(int primaryHand);       // support hand for this primary, else -1
int  PrimaryHand(int supportHand);       // primary using this support, else -1
bool IsSupportHand(int hand);

// Live support-controller data in OpenXR LOCAL space. SupportWeight is the raw
// grip squeeze [0,1]. GetSupportHandPoseTracking returns the real controller
// pose; GetSupportAnchorTracking returns the calibrated foregrip pose attached
// to the latest final weapon-model matrix. The latter is also available before
// engagement, so the renderer can draw a reachable socket hint. Its basis is
// the calibrated support-wrist basis (right/forward/up), not a weapon basis.
float SupportWeight(int primaryHand);
bool GetSupportHandPoseTracking(int primaryHand, TrackingPose* out);
bool GetSupportAnchorTracking(int primaryHand, TrackingPose* out);

// While weapon calibration is open, return the held two-handed weapon whose
// free hand should be shown on the support socket. This is a visual-only menu
// preview: it never creates a support relation or changes inventory ownership.
// Returns -1 outside that blocked calibration state.
int SupportCalibrationPrimaryHand();

// One thread-safe shortest-arc snapshot shared by weapon rendering and its
// laser/fire ray. It rotates the calibrated expected foregrip vector onto the
// real support controller around the primary controller, capped at 90 degrees.
// Returns false unless a support grip is currently engaged.
bool GetTwoHandTransformTracking(int primaryHand, float pivotOut[3],
                                 float axisOut[3], float* angleRadiansOut);

// One ballistic record is retained per source hand, matching qbuild. A hand may
// catch either record. Returned pose includes velocity/gravity/spin at query time.
int  DroppedSlot(int sourceHand);
bool GetDroppedPoseTracking(int sourceHand, TrackingPose* out);
bool GetDroppedPoseForSlotTracking(int slot, TrackingPose* out);

// Bits are SA weapon-slot indices. Occupied means drawn in a hand or flying and
// therefore must not also be drawn at a holster socket.
std::uint32_t OccupiedSlotMask();
bool IsSlotHeld(int slot);
bool IsSlotDropped(int slot);

// Feed back the completed calibrated model transform from the renderer. This is
// used as the exact transfer target and the initial pose of a thrown weapon.
void PublishHeldVisualPoseTracking(int hand, int slot, const TrackingPose* pose);

// Copy the most recently published final rendered model pose. Physical melee
// samples this rather than reconstructing a second calibration transform, so
// its damaging shaft stays on the weapon the player actually sees.
bool GetHeldVisualPoseTracking(int hand, int slot, TrackingPose* out);

// Publish the current one-hand/base model pose BEFORE applying the transform
// returned by GetTwoHandTransformTracking. Keeping this separate from the final
// visual pose prevents the two-hand rotation accumulating frame over frame.
void PublishHeldBasePoseTracking(int hand, int slot, const TrackingPose* pose);

// Route the holding hand's physical trigger into the game's single fire axis.
// FiringHand is -1 and FireTrigger is 0 when no weapon is held or interactions
// are blocked. Update also keeps CPed's active slot on this hand.
int   FiringHand();
float FireTrigger();

// A consumed physical throwable leaves the hand immediately; if more ammo is
// available the same inventory slot becomes visible at its holster again. This
// is not a generic grip release and never creates a flying weapon-model record.
void ReleaseAfterUse(int hand, int slot);

// qbuild WeaponGripLock semantics: releasing away from the assigned socket keeps
// the weapon held; releasing at the socket still returns it. Runtime only here;
// the menu/settings owner may persist this through its existing settings file.
void SetGripLock(bool enabled);
bool GripLock();

// The qbuild-compatible default supports two independently held slots. A caller
// may temporarily disable dual hold while bringing up a single-clump renderer;
// single-weapon mode still permits either hand and hand-to-hand transfer.
void SetDualHoldEnabled(bool enabled);
bool DualHoldEnabled();

}  // namespace savr::physicalweapon
