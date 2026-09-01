#pragma once

namespace savr::xr {
struct HandPose;
struct TwoHandVisualState;
}

namespace savr::vrcam {

// True while the player holds the VR gang-recruit gesture (R2, stick quiet).
bool RecruitGestureActive();

// Install the camera hook onto CCamera::CopyCameraMatrixToRWCam. Call once, after
// ResolveGameSymbols has succeeded. Returns false if the trampoline could not be
// built, in which case the game simply renders its own flat camera.
bool Install();

// Scope the exact retail game callback that precedes the large headset
// RenderScene pass. The camera hook profiles ConstructRenderList/ScanWorld and
// RenderQueue semaphore waits and owns the guarded deferred-Finish A/B: the
// exact retail completion is still consumed before the first large eye pass.
void BeginGameFrameTelemetry(double callbackStartMonoMs,
                             double callbackStartCpuMs);
void EndGameFrameTelemetry();

// True while the game is in real gameplay (a player exists, no cutscene running)
// and the head-tracked stereo camera is being driven. The render thread reads
// this to choose the stereo projection layer over the flat theater screen.
bool IsStereoActive();

// Re-evaluate the stereo/theater gate. Called once per frame from the input
// pump: the camera hook that normally maintains it stops running while the
// game's own pause menu owns the frame, which used to freeze the last stereo
// pair over the (invisible) menu.
void RefreshStereoGate();

// True while the engine's own flow-screen frontend (pause menu / main menu)
// is up, read from gMobileMenu's authoritative fields. The input pump uses it
// to hold the game's user pause for the whole pause-menu visit.
bool IsMobileMenuOpen();
// True during freefall before the deploy widget confirms the canopy. Physical
// weapon/throwable grabs are blocked while the deploy ring owns both hands.
bool ParachuteWeaponInteractionBlocked();

// Preserve the OpenXR-recommended per-eye raster size when the engine's flat
// SurfaceTexture uses a separate landscape size for Android menus/cutscenes.
void SetStereoBaseSize(int width, int height);

// Current HMD yaw relative to the recentered LOCAL origin. Bicycle MOTION mode
// uses it as the steering input, matching the PC GTA SA VR implementation.
bool GetLocalHeadYaw(float* yawOut);

// While true, the pad hook stops writing the left stick into CPad::NewState, so
// the stick drives a VR menu instead of walking the player. Set by the input
// layer whenever a VR menu (cheats or calibration) is open.
void SetInputBlocked(bool blocked);

// Body-fixed holster anchor in OpenXR LOCAL space. It uses the exact recentered
// body frame that places the RenderWare holstered model, so looking around does
// not make the cyan grab point orbit away from the visible weapon.
bool GetHolsterAnchorTracking(int point, float positionOut[3]);

// Controller-calibrated weapon ray in GTA world space. `hand` is the physical
// OpenXR hand (0 left, 1 right); calibration still comes from the one canonical
// RIGHT/master profile. This deliberately ignores the visible-laser toggle: a
// hidden sight must not make bullets fall back to camera aim.
bool GetWeaponFireRay(int hand, int weaponType,
                      float originOut[3], float directionOut[3]);

// The same model-bound ray in OpenXR LOCAL space, evaluated from the exact hand
// and two-hand snapshots baked into a stereo-ring slot.  The visible laser uses
// this entry point while GetWeaponFireRay converts the same calculation to GTA
// world space for hitscan.  AIM calibration is expressed in the final weapon
// model's barrel/top/lateral frame, so LEFT mirrors correctly and later WEAPON
// OFFSET/ROT edits carry an already-saved laser along with the gun.
bool GetWeaponRayTracking(const xr::HandPose& renderedPose, int hand,
                          int weaponType,
                          const xr::TwoHandVisualState& twoHand,
                          float originOut[3], float directionOut[3]);

// Convert one OpenXR LOCAL-space point through the exact recenter/body frame
// used by the stereo camera into GTA world coordinates. Physical melee uses
// this narrow bridge before passing tracked weapon sweeps to CWorld.
bool TrackingPointToWorld(const float trackingPoint[3], float worldPoint[3]);

// Inverse of TrackingPointToWorld for compositor-side world effects (for
// example a stereo bullet tracer). Converts a GTA world point into the same
// OpenXR LOCAL frame used by rendered hands, lasers and the current eye ring.
bool WorldPointToTracking(const float worldPoint[3], float trackingPoint[3]);

// Invalidate the yaw/position origin; the next live HMD pose becomes neutral.
void RequestRecenter();

// Vice City-style cutscene camera selector. During a stereo story/scripted
// scene R3 cycles director/actor cameras and L3 persists the current choice for
// that scene. The input layer calls these only on the GameThread.
bool CutsceneCameraControlsActive();
int  GetCutsceneCameraCount();
void CycleCutsceneCamera();
void RememberCutsceneCamera();

// Quest-safe eye render scale. The requested percentage is persisted
// immediately while a complete replacement stereo ring is prepared in the
// background and switched atomically once every texture is ready.
int  GetRenderScalePercent();
int  GetActiveRenderScalePercent();
bool RenderScaleChangePending();
void AdjustRenderScale(int direction);
bool AreWorldEffectsEnabled();
void SetWorldEffectsEnabled(bool enabled);
int  GetDynamicShadowMode();
const char* GetDynamicShadowModeName();
void AdjustDynamicShadowMode(int direction);
bool AreDynamicShadowsEnabled();
void SetDynamicShadowsEnabled(bool enabled);
bool AreNeonSignsEnabled();
void SetNeonSignsEnabled(bool enabled);
bool IsColorGradingEnabled();
void SetColorGradingEnabled(bool enabled);
bool IsHdWeaponsEnabled();
void SetHdWeaponsEnabled(bool enabled);
enum TracerColorMode {
    TRACER_COLOR_VICE_CITY = 0,
    TRACER_COLOR_GOLD = 1,
};
int GetTracerColorMode();
const char* GetTracerColorModeName();
void AdjustTracerColorMode(int direction);
int GetTracerSmokeSpreadPercent();
void AdjustTracerSmokeSpread(int direction);
int  GetGraphicsDistanceSettingCount();
const char* GetGraphicsDistanceSettingName(int field);
int  GetGraphicsDistanceSettingMeters(int field);
bool GraphicsDistanceSettingUsesRetail(int field);
bool GraphicsDistanceSettingNeedsRestart(int field);
void AdjustGraphicsDistanceSetting(int field, int direction);
void ResetGraphicsDefaults();

// Bounded traffic-population A/B. A guarded FOV105 target keeps real ambient
// cars alive farther out, where GTA's stock `_vlo` renderer draws them cheaply.
bool  IsTrafficShellActive();
float GetTrafficGenerationTarget();
float GetTrafficRetentionMeters();
int   GetTrafficCarCap();
float GetPedDensityScale();
bool  IsPedLifecycleRetentionActive();
float GetPedLifecycleRetentionMeters();
unsigned int GetPedLifecycleOverrideCount();
float GetBuildingDetailFloorMeters();

} // namespace savr::vrcam
