#pragma once

#include <jni.h>

#include <cstdint>

namespace savr::xr {

// Bring up the OpenXR loader, instance and system. Safe to call once the JavaVM
// and the game Activity are known; no GL context is required yet.
bool Initialize(JavaVM* vm, jobject activity);

// Create the session on top of the EGL context that is current on the calling
// thread. Must be called from the game's render thread, after the game has made
// its own context current, because the runtime binds to exactly that context.
bool CreateSession();

// Run one compositor frame. Called from the render thread once per game frame.
// Does nothing until the session is running, so it is safe to call every frame
// from the moment the hook is installed.
void RenderFrame(double consumerUpdateMs, double consumerUpdateCpuMs);

bool IsSessionRunning();
bool IsSessionFocused();

// Latest controller state, mapped later onto the game's gamepad. Sticks are
// [-1,1], triggers [0,1], buttons are momentary.
struct InputState {
    float leftStick[2]{};
    float rightStick[2]{};
    // Capacitive thumbrest touch per hand (Quest Touch). While a thumb
    // rests on either pad, the right stick becomes the D-pad (UEVR parity).
    bool  thumbrest[2]{};
    float triggers[2]{};   // left, right
    float grip[2]{};       // left, right squeeze [0,1]
    bool  a{}, b{}, x{}, y{}, menu{};
    bool  l3{}, r3{};      // thumbstick clicks (left, right)

    // Laser pointer hit on the theater screen, in [0,1] (u from left, v from top).
    // pointerValid is false when the ray misses the screen. pointerPressed is the
    // right trigger, used as a tap.
    bool  pointerValid{};
    bool  pointerPressed{};
    float pointerU{};
    float pointerV{};
};

// Copy out the most recent controller state. Cheap; safe to call every frame.
void GetInput(InputState& out);

// Fire a controller vibration pulse on one hand (0 left, 1 right).
void TriggerHaptic(int hand, float amplitude, float frequencyHz,
                   float durationMs);

// Publish the already-formatted stock text buffers after CMessages::Display.
// The compositor renders these strings with the APK's own font files onto
// transparent VR layers, so BIG MESSAGES and HELP TEXT no longer sample crops
// from the 1024x576 mobile HUD image.
void PublishHudText(const std::int16_t* brief, int briefCapacity,
                    const std::int16_t* bigMessages, int bigStyleCount,
                    int bigStyleCapacity,
                    const std::int16_t* help, int helpCapacity);

// Formatted mission clock/counter lines (ASCII, '\n'-separated), read from
// CUserDisplay::OnscnTimer each capture pass. Empty string clears the layer.
// Rendered with the APK font as the TIMERS element.
void PublishMissionTimersText(const char* text);
// Transient radio-station line, shown in the TIMERS text layer while the
// station changes (empty string clears it).
void PublishRadioText(const char* text);

// Per-hand tracked pose for rendering VR hands. grip drives the hand model; aim
// gives the pointing axis. Poses are in the session LOCAL space (same as the head
// pose), so the game thread recenters + converts them exactly like the head.
struct HandPose {
    bool  valid{false};
    bool  aimValid{false};
    float gripPos[3]{};
    float gripOri[4]{0, 0, 0, 1};   // x,y,z,w
    float aimPos[3]{};
    float aimOri[4]{0, 0, 0, 1};
    float grip{};                   // squeeze [0,1]
    float trigger{};                // trigger [0,1]
};
// Copy out both hands' poses (index 0 = left, 1 = right). Safe every frame.
void GetHandPoses(HandPose out[2]);

// Publish the exact GameThread hand snapshot used to build this frame's
// RenderWare weapon matrices. SetStereoEyeTextures copies it into the matching
// ring slot instead of resampling the asynchronously updated XR poses.
void SetRenderedHandPoses(const HandPose poses[2]);

// Basketball palm lock: while the ball is held, the rendered hand(s) are
// pinned onto the ball surface with an open palm (wheel-grab pattern), so
// ball and hand can never visibly drift apart. Positions are in tracking
// space. An all-false struct clears the lock.
struct BallHandLock {
    bool  locked[2]{false, false};
    float pos[2][3]{};
    // Palm basis (tracking space): `up` faces from the hand INTO the ball
    // (the mesh palm normal), `fwd` is the finger direction tangent to the
    // ball surface. Zero vectors = keep the controller orientation.
    float up[2][3]{};
    float fwd[2][3]{};
};
void SetBallHandLock(const BallHandLock& lock);

// Two-hand visual correction captured alongside the stereo eye textures.  All
// vectors are in OpenXR LOCAL space.  `pivot`, `axis` and `angleRadians` are the
// shortest-arc correction which turns the one-hand weapon towards the real
// support controller.  The support anchor/basis, when present, describe the
// calibrated wrist pose BEFORE that correction; the compositor applies the
// same rotation to the weapon laser, primary hand and support wrist so nothing
// can be sampled from a different frame.
//
// supportRight/Forward/Up are a conventional wrist basis (not UltimateXR mesh
// axes).  supportGestureGrip/Trigger select the baked hand poses; when no
// explicit gesture is supplied the support hand uses the closed magazine grip.
struct TwoHandVisualState {
    bool  active{false};
    int   primaryHand{-1};
    int   supportHand{-1};
    float pivot[3]{};
    float axis[3]{0.0f, 1.0f, 0.0f};
    float angleRadians{};

    bool  supportAnchorValid{false};
    float supportAnchor[3]{};
    bool  supportBasisValid{false};
    float supportRight[3]{1.0f, 0.0f, 0.0f};
    float supportForward[3]{0.0f, 0.0f, -1.0f};
    float supportUp[3]{0.0f, 1.0f, 0.0f};

    bool  supportGestureValid{false};
    float supportGestureGrip{1.0f};
    float supportGestureTrigger{1.0f};
};

// Publish once for every rendered stereo frame, including an inactive state
// after a support grip is released.  SetStereoEyeTextures snapshots it into the
// exact ring slot containing that frame's pixels and tracked hands.
void SetTwoHandVisualState(const TwoHandVisualState& state);

// Latest head pose in the session's local space, as located by the compositor
// for the frame it is presenting. Position is metres, orientation is an
// (x,y,z,w) quaternion. Returns false until a valid pose has been located.
// Read from the game thread to drive the in-world camera; publishing happens on
// the render thread, so the two never share a GL context.
bool GetHeadPose(float positionOut[3], float orientationOut[4]);

// The horizontal field of view (degrees) the game should render its mono frame
// at so the flat blit fills each eye's frustum without zoom mismatch. Derived
// from the runtime's recommended eye FOV; a sane default until one is located.
float RecommendedGameFovXDegrees();

// Both eye poses in the session's local space, as located for the frame being
// presented. Used by the game thread to build the two per-eye cameras (the IPD
// between the two positions is what produces real depth). false until valid.
bool GetEyePoses(float positionOut[2][3], float orientationOut[2][4]);

// Runtime-provided hidden-area triangle mesh for one physical eye. Vertices are
// normalized view coordinates in the OpenXR convention; indices address the
// returned vertex array. The storage is immutable for the lifetime of the XR
// instance and therefore may be read by the GameThread without copying.
struct HiddenAreaMaskView {
    const float* verticesXY{};              // vertexCount pairs: x,y
    const unsigned short* indices{};        // triangle-list indices
    int vertexCount{};
    int indexCount{};
};
bool GetHiddenAreaMask(int eye, HiddenAreaMaskView& out);

// ---------------------------------------------------------------------------
// Stereo eye-texture bridge.
//
// The engine renders the world twice per frame (once per eye) on the GameThread
// and hands the two finished eye images to the compositor thread through here.
// The textures are created on the engine's GL context and sampled on the XR
// context, which is why that context is created sharing the engine's.
//
// All Stereo* producer calls run on the GameThread with the engine context
// current. The consumer half is driven internally by RenderFrame.
// ---------------------------------------------------------------------------

// Dedicated transparent gameplay HUD target, matching the working Vice City
// compositor-layer architecture. The physical raster renders at 2x the logical
// layout so radar tiles and font glyphs survive being magnified onto wrist
// panels and eye composites; all saved calibration values, menu fields and
// crop rectangles stay in the original 1024x576 logical units.
constexpr int kGameplayHudWidth = 2048;
constexpr int kGameplayHudHeight = 1152;
constexpr int kGameplayHudLogicalWidth = 1024;
constexpr int kGameplayHudLogicalHeight = 576;

// Tell the bridge whether the XR context shares the engine context. When it does
// not, stereo eye textures cannot cross contexts, so the bridge disables itself
// and the game falls back to the mono path. Call once, before any Stereo* use.
void SetStereoContextShared(bool shared);

// Claim the ring slot for `seq` before the GameThread records either eye into it.
// The compositor pins a slot while copying both eyes, so a failed claim means the
// producer must leave that slot untouched and retry the same sequence next frame.
bool TryBeginStereoEyeWrite(int seq);
void CancelStereoEyeWrite(int seq);

// The experiment remains available for dedicated builds, while normal builds
// compile this call to a constant false and retain the accepted hot path.
#if defined(SAVR_EXPERIMENTAL_BACKPRESSURE)
bool ShouldThrottleStereoProducer(int nextSequence);
#else
inline bool ShouldThrottleStereoProducer(int) { return false; }
#endif

// Stereo: publish this frame's eye textures and their matching depth renderbuffers.
// Both arrays are [4][2] row-major GL object ids from the shared engine context —
// one left/right pair per ring buffer. `seq` is a monotonically increasing counter; the
// compositor samples a lagged pair and pins that slot until both eyes are copied;
// the lag protects RenderQueue completion while the pin prevents ring wrap.
// Published by the GameThread each stereo frame.
void SetStereoEyeTextures(const unsigned int* tex, const unsigned int* depth,
                          const unsigned int* hudTex,
                          const float* nearZ, const float* farZ,
                          int seq, int width, int height);

// RenderQueue command 46 normally presents the hidden Android surface through
// eglSwapBuffers.  Once a direct stereo generation is pending, the RenderQueue
// thread may replace that redundant present with a share-group GL fence.  The
// XR context inserts a server-side wait before sampling the matching ring slot;
// failures keep the retail swap path intact.
bool TryPublishStereoProducerFenceFromRenderQueue();
void NotifyStereoRetailSwapCompletedFromRenderQueue();

// Publish the game camera's water probe on the GameThread. The values are
// copied into the same ring slot as the eye textures so the compositor grades
// the exact frame whose world pixels it presents.
void SetUnderwaterState(float underWaterness, float waterDepth);

// Retail Android's final MobileRender colour transform for the current game
// frame. The GameThread publishes it before the matching eye-texture pair; the
// compositor applies it inside the existing resolve (no extra full-screen pass).
// mode: 0 = identity, 1 = ContrastMult/Add, 2 = RGB grading rows.
struct MobileColorState {
    int mode{};
    float contrastMult[3]{1.0f, 1.0f, 1.0f};
    float contrastAdd[3]{};
    float redGrade[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float greenGrade[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float blueGrade[4]{0.0f, 0.0f, 1.0f, 0.0f};
};
void SetMobileColorState(const MobileColorState& state);

// The half-tangents of the (symmetric) frustum the game rendered the eyes with.
// The compositor builds each projection view's fov from these so the presented
// projection layer matches the render exactly (no scale/stretch). Published by
// the GameThread each stereo frame.
void SetStereoRenderFov(float tanX, float tanY);

// Count one rendered game frame, for the on-screen FPS readout. Called once per
// stereo frame by the GameThread.
void NotifyGameFrame();

// Per-frame profiler stats for the debug panel: the two-eye render-block time in
// ms, and how many visible entities were static (buildings/map), dynamic
// (peds/vehicles), and dynamic-behind-the-camera (360deg over-render). Published
// once per frame by the GameThread.
void SetProfileStats(int recMs, int sceneMs, int skyMs, int endMs, int entCount, int dynCount);

// The whole game-frame work time (ms), from timing implOnDrawFrame. Lets the
// profiler split our stereo render cost (REC) from the game's own cost.
void SetFrameMs(double ms);

// VR cheat-menu state, published by the GameThread; the present thread draws the
// list. visible shows the menu (over the profiler panel); selection is the
// highlighted row; count is the number of cheats.
// category < 0 draws the cheat category root; otherwise it draws that
// category's items. count includes the explicit BACK row.
void SetMenuState(bool visible, int selection, int count, int category);

// Publish whether ANY VR menu page is open (menuPage != PG_NONE), each frame
// from the menu update. IsMenuVisible() reads this.
void SetAnyMenuOpen(bool open);

// True while ANY VR menu page is open. Other subsystems query this to keep
// button presses used for menu navigation (e.g. B) from also firing gameplay
// actions such as dropping the jetpack.
bool IsMenuVisible();

// VR holster marker anchors (XR world space), published by the GameThread; the
// present thread draws a small cube at each. Up to 8.
void SetHolsterMarkers(const float positions[][3], int count);

// Rhythm-minigame direction prompt (lowrider dance). dir: 0 none, 1 UP, 2 DOWN,
// 3 LEFT, 4 RIGHT; msUntil counts down to the beat. Drawn head-locked because
// the stock SCM arrow sprites never reach the VR eyes.
void SetBeatArrow(int dir, int msUntil);

// Immersive jetpack REALISTIC mode: when on, the present thread draws a jet
// flame from each controller's nozzle (scaled by that hand's trigger). Publish
// each frame from the GameThread; false hides both flames.
void SetJetpackThrusterMode(bool on);

// IronMan jetpack sub-mode: when on, the present thread draws Iron Man repulsor
// gauntlets in place of the procedural turbines (both still emit palm flame).
void SetJetpackGloveMode(bool on);

// Bitmask (bit h) of hands currently holding a weapon while the jetpack is worn;
// the present thread skips the turbine/flame on those hands so the drawn weapon
// shows instead. Lets the player fly one-handed and shoot with the other.
void SetJetpackWeaponHandMask(unsigned int mask);

// Parachute brake toggles (canopy open): two handles and their upper riser
// anchors in LOCAL tracking space. visible=false hides the complete assembly.
void SetParachuteToggles(const float positions[2][3],
                         const float anchors[2][3], const bool grabbed[2],
                         bool visible);

// Freefall canopy-deploy ring and its short tether in LOCAL tracking space.
void SetParachuteDeployRing(const float position[3], const float anchor[3],
                            const float forward[3], bool grabbed, bool visible);

// Active weapon's object-space geometry, published by the GameThread (extracted
// from the loaded RpAtomic). The present thread transforms it to the right hand's
// pose and draws it with our own GL, in the same eye FBO / mvp / depth pass as the
// hand — so weapon and hand stay perfectly aligned. Stage 1 = positions only.
//   verts: numVerts * 3 floats (x,y,z), object space
//   idx:   numIdx unsigned shorts (triangle list into verts)
// Pass nullptr / 0 to clear (unarmed). Copies internally; caller keeps ownership.
void SetWeaponGeometry(const float* verts, int numVerts,
                       const unsigned short* idx, int numIdx);

// The hand (0/1) currently holding a game-rendered weapon, or -1 for none. It is
// hidden only as a fallback when the game eye depth cannot be shared with GL hands.
void SetWeaponHeldHand(int hand);
// Full physical ownership for the depth-copy fallback. `mask` uses bit 0=LEFT,
// bit 1=RIGHT; preferredHand selects the one calibrated laser/fire ray.
void SetWeaponHeldHands(unsigned int mask, int preferredHand);

// Publish one physical hitscan path in OpenXR LOCAL space. The present thread
// keeps a short-lived stereo copy and draws it against the same copied world
// depth as the hands/laser; the stock mobile bullet-trace pass runs after our
// eye capture and is therefore not visible in the headset.
void AddBulletTracer(const float start[3], const float end[3], int weaponType);

// Refresh one marker captured from PlaceMarkerCone. The point and dimensions
// are in OpenXR LOCAL space; entries expire automatically when the original
// game stops issuing the marker. stockCone=false publishes the radar
// objective's HIGHLIGHT cage; stockCone=true publishes a stock C3dMarkers
// cone (mission points, building entrances) drawn as a translucent cylinder —
// the engine's own marker render happens after the eye captures and never
// reaches the headset.
void UpdateObjectiveMarker(std::uint64_t id, const float center[3],
                           float radius, float height,
                           unsigned char red, unsigned char green,
                           unsigned char blue, bool stockCone);

// Per-hand physical throwable preview in OpenXR LOCAL space. Points are copied
// and ring-snapshotted with the eye textures; count<=1 clears that hand's arc.
void SetThrowableTrajectory(int hand, const float points[][3], int count,
                            bool hit);

// CPU/GPU performance hint levels (0=POWER SAVE,1=SUS LOW,2=SUS HIGH,3=BOOST),
// driven by the VR menu Graphics section. GetX return the current index; the name
// is for the panel. Register the heavy GameThread TID so the runtime pins it to a
// big core (the actual frame-rate lever; a clock hint alone barely helps).
void SetPerfLevels(int cpuIdx, int gpuIdx);
int  GetCpuPerfIdx();
int  GetGpuPerfIdx();
const char* PerfLevelName(int idx);
void SetGameThreadTid(unsigned int tid);

// A consumer timeout may prove that the exact next producer publication landed
// just beyond the presentation boundary.  The XR side calibrates that phase
// error over several matching samples and offers one bounded advance to the
// existing 72 Hz GameThread pacer.  Consuming an advance never changes the
// steady frame period; it only shortens one otherwise-idle sleep.  The caller
// reports only the advance retained in the next pacer deadline, so diagnostics
// can distinguish a request from a phase change that actually reached the
// producer timeline after scheduler overshoot.
std::uint64_t ConsumeStereoPacerAdvanceNs(std::uint64_t maxNs);
void ReportStereoPacerAdvanceApplied(std::uint64_t appliedNs);

// VR menu calibration page. RIGHT is the single master profile; LEFT consumes the
// same values through its mirrored hand basis, so only one hand is calibrated.
void SetCalibPage(bool active, int selection, int hand, int weaponType);

// Root VR menu, holster loadout and Graphics submenu. Only one menu page is active
// at a time; the GameThread publishes whichever is current each frame.
void SetMainMenu(bool active, int selection);
void SetHolsterMenu(bool active, int selection);
void SetHolsterCalibMenu(bool active, int selection);
void SetDrivingMenu(bool active, int selection, int vehicleType);
void SetDrivingCalibrationMenu(bool active, int selection, int hand);
void SetLocomotionMenu(bool active, int selection);
void SetJetpackMenu(bool active, int selection);
void SetVehicleCameraMenu(bool active, int selection);
void SetBasketballMenu(bool active, int selection);
void SetBasketballCalibMenu(bool active, int selection);
// page: 0 = HUD root (presets/element/enable), 1 = sprite-crop + screen
// placement submenu, 2 = wrist placement submenu (on-foot slot), 3 = wrist
// submenu editing the VEHICLE/dash slot. Any active page keeps the
// calibration state on (live capture, sample text, always-visible panels).
void SetHudMenu(bool active, int selection, int page);
// Temporarily sweep the selected real crop across the complete 1024x576 HUD
// source. This is preview-only and never overwrites the saved crop.
void StartHudSourceScan();
bool HudSourceScanActive();
// Keep the offscreen HUD/text source alive while calibration is open, even
// when the classic gameplay HUD itself is disabled.
bool HudCalibrationActive();
void SetGraphicsMenu(bool active, int selection);
void SetControlsMenu(bool active, int selection);
void SetControlsTipsMenu(bool active);
void SetAboutMenu(bool active, bool firstRun);
void SetGraphicsDistanceMenu(bool active, int selection);

// Stop the compositor consuming the current RenderWare eye-texture generation.
// VrCamera calls this immediately before atomically switching to a newly-sized
// four-slot ring; publication resumes after the replacement ring has warmed up.
void ResetStereoEyeTextures();

// Drain compositor-side source-read fences while the XR GL context is still
// current. Call once before the consumer thread releases that context.
void DrainStereoEyeReads();

// The old RenderWare raster ring may be destroyed only after every row of the
// replacement epoch has been published. Claiming each row proves any old
// CPU/GPU reader of that slot completed before its replacement was recorded.
bool StereoEyeRetiredRingCanBeDestroyed();

// Ensure the four-buffered eye textures exist at this per-eye size. Cheap when
// already sized; recreates on a size change. Returns false if allocation failed
// or the XR context is not shared.
bool StereoEnsure(int width, int height);

// Pick the buffer to render this frame's eyes into (the one not on display), or
// -1 if the bridge is not ready. Call once per stereo frame.
int StereoBeginFrame();

// The eye texture and the GameThread-side framebuffer that wraps it, for buffer
// `idx` and eye `e` (0 = left, 1 = right).
unsigned int StereoEyeTex(int idx, int e);
unsigned int StereoEyeFbo(int idx, int e);

// Publish buffer `idx` as the newest complete pair, fencing its GPU work so the
// consumer can wait on it before sampling.
void StereoCommit(int idx);

// The horizontal FOV (degrees) the game rendered the current eye pair at. The
// compositor presents the eyes with the matching symmetric frustum so render and
// present agree. Published by the GameThread each stereo frame.
void SetStereoRenderFovX(float degrees);

// The exact head pose the game rendered the current frame with. Presenting the
// layer at THIS pose (not a fresher one) lets the compositor reproject it to the
// real display-time head pose, which removes the head-motion jitter that comes
// from showing a frame rendered at one pose as if it were at another.
void SetRenderHeadPose(const float position[3], const float orientation[4]);

// Recommended per-eye render size, available as soon as Initialize has run.
// The game is handed a surface of exactly this size so it never renders at the
// wrong resolution for the headset.
bool RecommendedEyeSize(int& width, int& height);

// Wide-menu crop: while the mobile menu renders into a 16:9 logical band of
// the (taller) game surface, keep the theater pointer in that same band. (0,0)
// clears the crop and the quad shows the full frame again.
void SetTheaterCrop(int width, int height);

// Hand over the engine's finished frame for the compositor to show. The texture
// is an external OES texture in the render thread's context; transform is the 4x4
// matrix SurfaceTexture reports for it. Passing 0 falls back to the test colours.
void SetGameFrame(unsigned int texture, const float* transform);

} // namespace savr::xr
