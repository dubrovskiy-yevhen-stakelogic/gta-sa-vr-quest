#pragma once

#include <cstdint>

namespace savr {

// Resolved entry points and globals of libGame.so.
//
// The mobile build exports 22357 dynamic symbols with C++ names intact, so every
// address below comes from a plain dlsym on the mangled name. No offset tables,
// no version-specific address databases, and a new game build only breaks us if
// a symbol is actually renamed.
struct GameSymbols {
    // Plain three-float game vector. On arm64 a by-value Vec3 is an HFA and is
    // therefore passed in s-registers, exactly matching the exported C++ ABI.
    struct Vec2 { float x, y; };
    struct Vec3 { float x, y, z; };

    // --- engine ---
    void  (*RenderScene)(bool);          // _Z11RenderSceneb
    void  (*Idle)(void*, bool);          // _Z4IdlePvb

    // Android RenderWare records commands on GameThread and replays them on a
    // dedicated RenderQueue thread. Retail ProcessAll holds the queue mutex
    // while every GLES command executes, serialising the producer and consumer.
    // The VR hook keeps the mutex only around pointer publication while retaining
    // the stock command handlers and vertex/index-buffer safety transitions.
    void** RenderQueue_global;            // RenderQueue* renderQueue
    bool  (*RenderQueue_ProcessAll)(void* queue);
    void  (*RenderQueue_Lock)(void* queue);
    void  (*RenderQueue_Unlock)(void* queue);
    void  (*RenderQueue_ResetLocked)(void* queue);
    void  (*RQIndexBuffer_SetSafe)();
    void  (*RQVertexBuffer_SetSafe)();
    bool  (*OS_CanGameRender)();

    // --- JNI boundary (com.rockstargames.oswrapper.GameNative) ---
    void  (*implOnDrawFrame)(void* env, void* clazz, float dt);
    void  (*implOnSurfaceChanged)(void* env, void* clazz, void* surface, int w, int h);
    void  (*implOnSurfaceDestroyed)(void* env, void* clazz);
    void  (*implOnSurfaceCreated)(void* env, void* clazz);
    void  (*implOnResume)(void* env, void* clazz);
    void  (*implOnPause)(void* env, void* clazz);

    // Rockstar Social Club gate. The engine calls ShowGate to display the login
    // and waits; GateComplete lets it proceed. We hook ShowGate to call
    // GateComplete(passed) so the login never appears.
    void  (*OS_RockstarShowGate)(int gateId);
    void  (*OS_OnRockstarGateComplete)(int gateId, bool passed);
    void  (*OS_RockstarShowInitial)();
    void  (*OS_OnRockstarInitialComplete)();
    void  (*implOnTouchStart)(void* env, void* clazz, int id, float x, float y);
    void  (*implOnTouchMove)(void* env, void* clazz, int id, float x, float y);
    void  (*implOnTouchEnd)(void* env, void* clazz, int id, float x, float y);
    void  (*CAEStreamThread_Service)();
    void* (*CAEMP3_GetDataStream)(void* self, unsigned int id);
    void  (*CAudioEngine_PreloadCutscene)(void* self, short a, unsigned char b);
    void  (*implOnBackButtonPressed)(void* env, void* clazz);
    void  (*CCutsceneMgr_Skip)();
    void  (*CCutsceneMgr_Start)();
    bool* CCutsceneMgr_ms_running;
    void  (*implOnGamepadConnected)(void* env, void* clazz, int pad);
    void  (*implOnGamepadAxesChanged)(void* env, void* clazz, int pad,
                                      float lx, float ly, float rx, float ry,
                                      float lt, float rt);
    void  (*implOnGamepadButtonDown)(void* env, void* clazz, int pad, int button);
    void  (*implOnGamepadButtonUp)(void* env, void* clazz, int pad, int button);

    // --- camera / stereo injection ---
    // The scene is rendered from CCamera::m_mCameraMatrix. On this build that
    // matrix lives at TheCamera + 0x970 (four CVectors: right/forward/up/pos,
    // 0x10 stride) — proven by disassembling CopyCameraMatrixToRWCam and
    // CalculateDerivedValues, both of which touch exactly those offsets.
    void  (*CCamera_CalculateDerivedValues)(void* self, bool a, bool b);
    void  (*CCamera_CopyCameraMatrixToRWCam)(void* self, bool alsoPlayerCam);
    int   (*CCamera_GetScreenFadeStatus)(void* self);
    void  (*CVisibilityPlugins_SetRenderWareCamera)(void* rwCamera);
    void  (*RwCameraBeginUpdate)(void* rwCamera);
    void  (*RwCameraEndUpdate)(void* rwCamera);
    void  (*RwCameraClear)(void* rwCamera, void* rgba, int clearFlags);

    // The sky gradient is drawn by CClouds::RenderSkyPolys BEFORE RenderScene into
    // the main surface, so our per-eye RenderScene pass misses it (black sky). We
    // re-issue it into each eye raster. DefinedState restores default RW states.
    void  (*CClouds_RenderSkyPolys)();
    // CClouds::Render draws the moon + low clouds INSIDE RenderScene with the depth
    // test OFF, so it paints over anything drawn before RenderScene. We hook it to
    // draw the weapon at the END of it — after the backdrop, before world geometry —
    // so the weapon lands opaque in the shared depth buffer.
    void  (*CClouds_Render)();
    void  (*DefinedState)();
    // Original flat gameplay HUD. CLASSIC issues it while each stereo eye
    // camera is still updating, without guessing any CHud field offsets.
    void  (*CHud_Draw)();
    void  (*CHud_DrawAfterFade)();
    // Android's gameplay HUD is split between CHud and the mobile touch
    // partitions. DrawAll(false) owns player status; CMessages::Display owns
    // tutorial/mission text in the two stock before/after-fade passes.
    void  (*CTouchInterface_DrawAll)(bool afterFade);
    void  (*CMessages_Display)(unsigned char beforeFade);
    // Fully formatted GXT buffers populated by CMessages::Display. Reading
    // these gives VR the actual mission/subtitle strings, including inserted
    // numbers, without OCR or pixel crops.
    std::int16_t* CHud_m_Message;       // GxtChar[400]
    std::int16_t* CHud_m_BigMessage;    // GxtChar[7][128]
    // Mobile tutorial/help text lives in CWidgetHelpText instead of CHud.
    // The exported global stores the current singleton pointer; its queue uses
    // ten 0x334-byte entries beginning at instance+0xcc.
    void** CWidgetHelpText_m_pInstance;
    // CHud::Draw queues text into CFont's shared buffer. Flush it before ending
    // the current eye update so money/ammo/wanted/help text lands in that eye
    // instead of leaking into the later centre-camera 2D pass.
    void  (*CFont_RenderFontBuffer)();
    // Resets the shared font render-state pointer without drawing (plus a
    // handful of per-frame font defaults and the empty sprite buffer). Used by
    // the HUD capture pass to DISCARD text CHud::Draw queued — script texts,
    // subtitles and area names must never reach the VR crop texture; the VR
    // layer renders its own text from the published strings instead.
    void  (*CFont_InitPerFrame)();
    // Exported one-byte global. The two ANONYMOUS bytes directly after it are
    // the engine's suspend/resume one-shots polled by OS_ApplicationTick:
    // [+1] runs the resume tick (CTimer::Update, audio unpause), [+2] calls
    // MobileMenu::InitForPause on that tick. Their stock writers are
    // OS_ApplicationEvent's pause/resume handlers (disasm 2.11 @0x36b95c /
    // @0x36b9ac, consumed @0x36b820/@0x36b86c). Setting both from the
    // GameThread opens the game's own pause menu — the mapping for the left
    // controller's Menu button, since neither gamepad Start nor
    // CPad::NewState can pause the mobile build.
    std::uint8_t* SkipIntroCutscene;
    // The MobileMenu singleton object (2.11 flow-screen frontend). The engine
    // treats the menu as OPEN when int32 at +0x24 is non-zero OR the pointer
    // at +0x30 (current FlowScreen) is non-null — the exact gates
    // OS_ApplicationTick and OS_ApplicationEvent use (GOT slot 0x8373f0
    // relocates to this symbol; offsets disasm-verified @0x36b830/@0x36b988).
    // The stereo gate must see the pause menu as "not gameplay", or the
    // compositor keeps presenting the last frozen eye pair over it.
    void* gMobileMenu;
    // Deferred NPC held-weapon pass: RenderScene fills ms_weaponPedsForPC and
    // the stock Idle() draws + resets the list only AFTER our render hook
    // returned — onto the flat Android surface. Each stereo eye must draw the
    // list itself right after its RenderScene, like the PC VR port does.
    void  (*CVisibilityPlugins_RenderWeaponPedsForPC)();
    // Mission clock/counters blob (COnscreenTimer). The flat pass keeps
    // formatting its ASCII display strings each frame; the VR capture reads
    // them for the APK-font TIMERS layer. Layout is the PC one, confirmed by
    // the Android DrawMissionTimers disasm (display flag at +0x150).
    void* CUserDisplay_OnscnTimer;
    // Skinned-ped bone access for drawing NPC weapon clumps at the hand bone,
    // replicating RenderWeaponPedsForPC per stereo eye (its own draws happen
    // after our hook and never reach the eyes).
    void* (*GetAnimHierarchyFromSkinClump)(void* clump);
    int   (*RpHAnimIDGetIndex)(void* hierarchy, int boneId);
    void* (*RpHAnimHierarchyGetMatrixArray)(void* hierarchy);
    // Android SA draws player status through a mobile widget rather than the
    // desktop CHud path. CLASSIC renders only this widget, never touch buttons.
    void** CTouchInterface_m_pWidgets;
    void*  CWidgetPlayerInfo_vtable;
    void   (*CWidgetPlayerInfo_Draw)(void* widget);
    // Android CSprite::CalcScreenCoors projects world sprites with the active
    // screen width/height stored at RsGlobal +0x08/+0x0c. Stereo eye rasters can
    // be smaller than the main surface, so the render hook scopes these values
    // to each eye and restores them before HUD/menu rendering.
    void* RsGlobal;

    // VR holster: switch the player's active weapon to a slot (reach + grip).
    void  (*CPed_SetCurrentWeaponSlot)(void* ped, int slot);
    int   (*CPed_GiveWeapon)(void* ped, int weaponType, unsigned int ammo, bool like);
    bool  (*CPed_IsAlive)(void* ped);
    // Static analyser predicate; used for the auto-equip-parachute skydive check.
    bool  (*CPedGeometryAnalyser_IsInAir)(const void* ped);

    // CVector is a 3-float HFA: identical registers to three float params.
    // Used for the VR aircraft climb boost (left grip + trigger).
    void  (*CPhysical_ApplyMoveForce)(void* physical, float x, float y, float z);

    // Mobile touch-widget query behind script opcode 0x0A52 — the exact call
    // player_parachute.scm polls (widget 187) for "open the parachute".
    // Raw address — hooked, not called.
    void* CTouchInterface_IsReleased;
    void* CTouchInterface_IsHeldDown;
    void* CTouchInterface_IsJustPressed;
    void* CTouchInterface_IsDoubleTapped;
    void* CTouchInterface_IsTouchedQuery;

    // SCM global variable pool. Globals are 32-bit at their byte offset;
    // player_parachute.scm keeps its phase in $2D0C (3 = deploy-ready).
    void* CTheScripts_ScriptSpace;

    // Synchronous scene stream-in around a point (the game's teleport warm-up).
    // Used once on bail-out so the world below a skydive is already resident.
    void  (*CStreaming_LoadScene)(const Vec3* point);
    float (*CWorld_FindGroundZForCoord)(float x, float y);

    // RW iterators for the interior-glass material pass (side windows are
    // MATERIALS inside door atomics, not separate frames like the windscreen).
    void* (*RpClumpForAllAtomics)(void* clump,
                                  void* (*cb)(void*, void*), void* data);
    void* (*RpGeometryForAllMaterials)(void* geometry,
                                       void* (*cb)(void*, void*), void* data);

    // Right-stick vehicle accessors (Hydra nozzles, heli rudder, tank turret
    // path). Raw addresses — hooked with verified trampolines; in touch mode
    // ([pad+0x118]!=0) their bodies read CHID analog axes and never NewState,
    // so pad writes cannot reach them.
    void* CPad_GetCarGunUpDown;
    void* CPad_GetCarGunLeftRight;
    void* CPad_GetCarGunFired;
    void* CPad_CarGunJustDown;
    void* CAutomobile_TankControl;
    void* CPad_GetTurretLeft;
    void* CPad_GetTurretRight;
    void* CPad_JumpJustDown;

    // Mobile HID input-mode query (0=touch, 1=joystick/gamepad). Patched, not
    // called: CPad::GetCarGunUpDown/LeftRight only read NewState right-stick
    // values when this reports a gamepad — the Hydra nozzle gate.
    void* CHID_GetInputType;

    // VR weapon rendering (Approach B: extract the loaded model's geometry and
    // draw it with our own GL at the hand / holster anchor). The runtime chain,
    // all offsets disassembly-verified against 2.11.311 arm64 libGame.so:
    //   type   = *(int32*)(ped + 0x730 + activeSlot*0x20)   (activeSlot = int8 @ ped+0x8DC)
    //   wi     = GetWeaponInfo(type, 1 /*STD; index=type. 0=type+25 is WRONG*/)
    //   modelId = *(int32*)(wi + 0x0C)   (CWeaponInfo stride 0x70)
    //   mi     = ms_modelInfoPtrs[modelId]                    (stride 8)
    //   clump  = *(RpClump**)(mi + 0x40)   (m_pRwObject)
    //   atomic = GetFirstAtomic(clump);   geom = *(void**)(atomic + 0x30)
    //   numTris = *(int32*)(geom + 0x18);  numVerts = *(int32*)(geom + 0x1C)
    void* (*CWeaponInfo_GetWeaponInfo)(int weaponType, signed char skill);
    void** CModelInfo_ms_modelInfoPtrs;   // base of the CBaseModelInfo* array
    bool  (*CModelInfo_IsTrainModel)(int modelId);
    bool  (*CModelInfo_IsBmxModel)(int modelId);
    bool  (*CModelInfo_IsQuadBikeModel)(int modelId);
    void* (*GetFirstAtomic)(void* clump);
    // Keep configured holster models resident. The request is asynchronous; the
    // renderer simply skips a slot until modelInfo->m_pRwObject becomes available.
    void  (*CStreaming_RequestModel)(int modelId, int streamingFlags);
    // Retail streaming state table. Each entry is 0x14 bytes and byte +0x10 is
    // eStreamingLoadState (0 not loaded, 1 loaded, 2 requested, 3 reading,
    // 4 finishing). The linked-LOD prefetch uses it only as an idempotence and
    // request-budget witness; the game remains the sole owner of the table.
    std::uint8_t* CStreaming_ms_aInfoForModel;
    // HD weapon model set: extra CD image registration + texture database in
    // the engine's own loose-PNG text format (format 0).
    int   (*CStreaming_AddImageToList)(const char* name, bool notPlayerImg);
    void  (*CStreaming_RemoveModel)(int modelId);
    void  (*CStreaming_FlushChannels)();
    std::uint8_t* CStreaming_ms_files;   // stride 0x30, 8 slots, +0x2c handle
    void* (*CModelInfo_GetModelInfoByName)(const char* name, int* outIndex);
    void* (*TextureDatabaseRuntime_Load)(const char* name, bool preload,
                                         int format);
    void* (*TextureDatabaseRuntime_GetDatabase)(const char* name);
    void* TextureDatabaseRuntime_SortEntries;  // hook target, see HdWeapons
    // Cutscene actors: pointer array + count. CJ's actor is the entry whose
    // CEntity::m_nModelIndex (u16 at +0x32) is 0 — the player model.
    std::uint8_t* CCutsceneMgr_ms_pCutsceneObjects;
    std::int32_t* CCutsceneMgr_ms_numCutsceneObjs;
    // Per-model streaming read buffer size (sectors). A model larger than this
    // overflows ms_pStreamingBuffer when streamed. LoadCdDirectory grows it at
    // boot for the base archive; direct HD repoints must respect the value.
    std::int32_t* CStreaming_ms_streamingBufferSize;
    void* (*CClumpModelInfo_CreateInstance)(void* modelInfo);
    void  (*CBaseModelInfo_AddRef)(void* modelInfo);
    void  (*CBaseModelInfo_RemoveRef)(void* modelInfo);
    void* (*RpClumpDestroy)(void* clump);

    // Approach A weapon render: the CPU geometry is freed after GPU instancing, so
    // instead of extracting verts we draw the game's own weapon RpAtomic in our
    // per-eye pass. We temporarily write our hand world-matrix into the atomic's
    // RwFrame, render, then restore (all offsets disassembly-verified):
    //   frame = *(RwFrame**)(atomic + 0x08)   (RwObject.parent)
    //   RwFrameTransform(frame, &rwMatrix, 0 /*rwCOMBINEREPLACE*/) installs it
    //   AtomicDefaultRenderCallBack(atomic) renders (reuses cached GPU instance data)
    //   RwMatrix is 64 bytes: right@0x00, flags@0x0c, up@0x10, at@0x20, pos@0x30
    //   frame modelling matrix @ frame+0x20
    void  (*RwFrameTransform)(void* frame, const void* matrix, int combineOp);
    void* (*AtomicDefaultRenderCallBack)(void* atomic);
    // The game's own weapon render (RenderWeaponPedsForPC) writes the bone matrix
    // into the frame's LOCAL matrix (frame+0x20), then RwFrameUpdateObjects marks
    // it dirty so RwFrameGetLTM recomputes the LTM (frame+0x60) with correct flags.
    // We mirror that instead of writing the LTM directly.
    void* (*RwFrameUpdateObjects)(void* frame);
    // RpClumpRender is how the game actually draws a weapon (RenderWeaponPedsForPC):
    // it iterates the clump's atomics, syncs each frame's LTM and calls each
    // atomic's render callback (atomic+0x70) — NOT the +0xa0 pipeline (which is null
    // on weapon atomics here), which is why the bare AtomicDefaultRenderCallBack
    // rendered nothing. clump frame at clump+0x08.
    void* (*RpClumpRender)(void* clump);
    // Vanilla/PC-VR weapon lighting.  Rendering the player weapon clump outside
    // the normal ped pass without this pair leaves textured atomics almost black.
    bool  (*CPed_SetupLighting)(void* ped);
    void  (*CPed_RemoveLighting)(void* ped, bool lightingSetUp);
    // Near clip plane of the Scene RwCamera (stored at camera+0xA8). The game's
    // default clips the close weapon grip in VR; drop it to ~0.05m for the eye pass
    // (matches the PC mod's VR_FIRST_PERSON_NEAR_CLIP), then restore.
    void  (*RwCameraSetNearClipPlane)(void* camera, float nearZ);
    // Far clip plane of the Scene RwCamera (camera+0xAC).  Visibility list
    // admission alone is insufficient: RenderWare still clips submitted geometry
    // against this projection plane, so stereo eye passes must raise it in lockstep
    // with the headset visibility floor and restore it afterwards.
    void  (*RwCameraSetFarClipPlane)(void* camera, float farZ);

    // Controller-directed player hitscan. We hook only FireInstantHit and leave
    // CWeapon::Fire itself in charge of ammo, timing, state, audio and stats.
    bool  (*CWeapon_FireInstantHit)(void* weapon, void* firingEntity,
                                    Vec3* origin, Vec3* muzzlePos,
                                    void* targetEntity, Vec3* target,
                                    Vec3* driveByOrigin, bool arg6, bool muzzle);
    bool  (*CWeapon_FireSniper)(void* weapon, void* shooter,
                                void* targetEntity, Vec3* target);
    bool  (*CWeapon_Fire)(void* weapon, void* shooter,
                          Vec3* startPos, Vec3* barrelPos,
                          void* targetEntity, Vec3* target,
                          Vec3* driveByOrigin);
    bool  (*CWeapon_FireProjectile)(void* weapon, void* shooter,
                                    Vec3* origin, void* targetEntity,
                                    Vec3* target, float force);
    // Launcher shots cannot use CWeapon::FireProjectile for the local VR
    // player: stock SA rejects them outside its legacy rocket-camera modes and
    // derives their direction from that camera. The verified allocator export
    // lets the narrow launcher hook preserve native projectile construction
    // while supplying the physical barrel velocity directly.
    bool  (*CProjectileInfo_AddProjectile)(void* creator, int projectileType,
                                            Vec3 origin, float force,
                                            Vec3* direction, void* target);
    // The native projectile allocator is left in charge of object creation,
    // fuse, collision and effects. Throwable.cpp replaces only the freshly
    // created CPhysical move speed with the tracked controller launch vector.
    void** CProjectileInfo_ms_apProjectile; // CProjectile*[32]
    bool  (*CWorld_ProcessLineOfSight)(const Vec3* origin, const Vec3* target,
                                       void* outColPoint, void** outEntity,
                                       bool buildings, bool vehicles, bool peds,
                                       bool objects, bool dummies, bool seeThrough,
                                       bool cameraIgnore, bool shootThrough);
    // Physical melee uses the same narrow world queries as the native fight
    // task: an overlapping sphere supplies weapon thickness, then an exact LOS
    // verifies that a wall did not sit between the controller and the target.
    void* (*CWorld_TestSphereAgainstWorld)(Vec3 centre, float radius,
                                            void* ignoreEntity,
                                            bool buildings, bool vehicles,
                                            bool peds, bool objects, bool dummies,
                                            bool seeThrough);
    void* gaTempSphereColPoints; // CColPoint[32], element size 0x2c on 2.11 arm64
    void** CWorld_pIgnoreEntity;
    bool*  CWorld_bIncludeDeadPeds;
    bool*  CWorld_bIncludeCarTyres;
    bool*  CWorld_bIncludeBikers;
    std::uint8_t (*CPed_GetWeaponSkill)(void* ped, int weaponType);
    bool   (*CWeapon_CheckForShootingVehicleOccupant)(void** entity,
                                                       void* colPoint,
                                                       int weaponType,
                                                       const Vec3* origin,
                                                       const Vec3* target);
    void   (*CWeapon_DoBulletImpact)(void* weapon, void* firedBy, void* victim,
                                     Vec3* origin, Vec3* target,
                                     void* colPoint, int incrementalHit);
    void   (*CWeapon_DoWeaponEffect)(void* weapon, Vec3 origin, Vec3 direction);

    // Native SA damage endpoints used after a tracked melee sweep resolves a
    // real actor/vehicle. GenerateDamageEvent retains health/armour, reactions,
    // crime and death handling; InflictDamage retains vehicle proof/damage rules.
    bool   (*CWeapon_GenerateDamageEvent)(void* victimPed, void* creator,
                                          int weaponType, int damage,
                                          int pedPiece, int localDirection);
    void   (*CVehicle_InflictDamage)(void* vehicle, void* creator,
                                     int weaponType, float damage,
                                     Vec3 impactPosition);
    int    (*CPed_GetLocalDirection)(void* ped, const Vec2* relativePoint);
    void   (*CEntity_GetBoundCentre)(void* entity, Vec3* outCentre);

    // Physical melee response for dynamic props (the gym punching bags).
    // Both CVector parameters are 3-float HFAs, flattened to scalar floats
    // the same way as ApplyMoveForce above (disasm-verified: force arrives
    // in s0-s2, the contact offset in s3-s5, the bool in w1).
    void   (*CPhysical_ApplyForce)(void* physical,
                                   float forceX, float forceY, float forceZ,
                                   float offsetX, float offsetY, float offsetZ,
                                   bool updateTurnSpeed);
    void   (*CPhysical_AddToMovingList)(void* physical);
    void   (*CObject_SetIsStatic)(void* object, bool isStatic);
    void   (*CObject_ObjectDamage)(void* object, float damage,
                                   Vec3* fxPosition, Vec3* fxDirection,
                                   void* damager, int weaponType);

    // Native collision audio used for physical fist/melee contacts. Passing the
    // real CColPoint surface id keeps metal/glass/body material selection intact.
    void* AudioEngine;
    void  (*CAudioEngine_ReportBulletHit)(void* audioEngine, void* entity,
                                          unsigned char surface,
                                          Vec3* position, float angleDegrees);
    // Physical contact thud for props: the same call CPhysical::ApplyCollision
    // makes when CJ's body pushes an object, so a punch on the gym bag picks
    // the authored AE_SURFACE_TYPE_PUNCHBAG sound instead of a bullet impact.
    void  (*CAudioEngine_ReportCollision)(void* audioEngine, void* entityA,
                                          void* entityB,
                                          unsigned char surfaceA,
                                          unsigned char surfaceB,
                                          Vec3* position, Vec3* normal,
                                          float impulseForce, float relVelSq,
                                          unsigned char forceOneShot,
                                          unsigned char forceLooping);

    // Vanilla gunshot event so nearby pedestrians still react to a custom ray.
    void   (*CEventGunShot_Construct)(void* storage, void* shooter,
                                      Vec3 origin, Vec3 target, bool silent);
    void   (*CEventGunShot_Destruct)(void* storage);
    void*  (*GetEventGlobalGroup)();
    void*  (*CEventGroup_Add)(void* group, void* event, bool valid);

    // VR culling widening: the game's single frustum visibility test drives the
    // render list AND population despawn at a FOV narrower than the reprojected
    // headset view. In stereo we force near entities visible through these.
    bool  (*CEntity_GetIsOnScreen)(void* entity);
    bool  (*CCamera_IsSphereVisible)(void* cam, const void* origin, float radius);
    bool  (*CEntity_IsEntityOccluded)(void* entity);

    // Stereo draw-list reuse guards. RenderEverythingBarRoads inserts fading
    // entities while RenderScene is recording; without a guard the second eye
    // inserts the same entities into the still-live alpha lists a second time.
    bool  (*CVisibilityPlugins_InsertEntityIntoSortedList)(void* entity, float distance);
    void* CVisibilityPlugins_m_alphaEntityList;
    void* CVisibilityPlugins_m_alphaUnderwaterEntityList;

    // RenderWater consumes (clears) bit 0 in every marked polygon after drawing
    // it. Stereo snapshots these marks before the left eye and restores them for
    // the right eye so both calls receive the same water geometry.
    std::uint32_t* CWaterLevel_m_nNumOfWaterTriangles;
    std::uint32_t* CWaterLevel_m_nNumOfWaterQuads;
    void* CWaterLevel_m_aTriangles;
    void* CWaterLevel_m_aQuads;

    // LOD handoff diagnostics plus the request-only linked-map prefetch A/B.
    // SetupMapEntityVisibility owns the linked building/detail-model decision;
    // ShouldModelBeStreamed is used only by GTA's request scan and does not add
    // an entity to the render list. Vehicle values are squared camera-distance
    // thresholds published by SetRenderWareCamera.
    int   (*CRenderer_SetupBigBuildingVisibility)(void* entity,
                                                  float* distanceOut);
    int   (*CRenderer_SetupMapEntityVisibility)(void* entity, void* modelInfo,
                                                 float distance, bool timeInRange);
    bool  (*CRenderer_ShouldModelBeStreamed)(void* entity, const void* point,
                                             float farClip);
    // Read-only end-to-end visibility witness. RenderOneNonRoad consumes the
    // opaque list while RenderEntity consumes alpha/fading entries.
    void  (*CRenderer_RenderOneNonRoad)(void* entity);
    void  (*CVisibilityPlugins_RenderEntity)(void* entity, float distance);
    // Retail ScanWorld builds a narrow monitor-camera wedge.  The VR nearby
    // sector A/B keeps that stock pass, then asks ScanSectorList to visit the
    // still-unseen sectors in a bounded circle around the headset.  Retail's
    // per-frame scan code makes the overlap free of duplicate entity work.
    void  (*CRenderer_ScanWorld)();
    void  (*CRenderer_ScanSectorList)(int sectorX, int sectorY);
    // Authored 200 m BIG-building LOD grid.  The aircraft horizon pass calls
    // this retail walker directly so it retains GTA's parent/child handoff,
    // time models, fading and streaming behavior.
    void  (*CRenderer_ScanBigBuildingList)(int sectorX, int sectorY);
    float* CRenderer_ms_vecCameraPosition; // three floats, GameThread-owned
    float* CRenderer_ms_fCameraHeading;    // radians, scoped during VR scan
    float* CVisibilityPlugins_ms_vehicleLod0RenderMultiPassDist;
    float* CVisibilityPlugins_ms_vehicleLod0Dist;
    float* CVisibilityPlugins_ms_vehicleLod1Dist;
    float* CVisibilityPlugins_ms_bigVehicleLod0Dist;
    // Squared hard draw cutoff consumed directly by RenderPedCB.  Kept
    // separate from population generation/removal distances.
    float* CVisibilityPlugins_ms_pedLodDist;
    // Normal-vehicle atomic callback and its squared current camera distance.
    // A guarded trampoline samples the vehicle nearest the high/low LOD0 edge;
    // it never changes the callback's render decision.
    void* (*CVisibilityPlugins_RenderVehicleHiDetailCB)(void* atomic);
    void* (*CVisibilityPlugins_RenderVehicleHiDetailAlphaCB)(void* atomic);
    float* gVehicleDistanceFromCamera;

    // Static breakable props (palms, lamp posts, signs) are represented by a
    // cheap CDummyObject outside the population collision radius and promoted
    // to a physical CObject near the player.  Retail hard-codes 80 m in
    // ManagePopulation, independently of every renderer/IDE draw distance.
    void  (*CPopulation_ManageObject)(void* object, const Vec3* playerPosition);
    void  (*CPopulation_ConvertToRealObject)(void* dummyObject);
    void  (*CPopulation_ConvertToDummyObject)(void* object);

    // UpdateStaticShadows mutates eye-independent lifetime state. It must execute
    // once for the stereo pair, while the actual shadow draw remains per eye.
    void  (*CShadows_UpdateStaticShadows)();

    // Per-eye consumption fix: RenderScene draws (and clears the count of) stored
    // vehicle/ped shadows on the FIRST eye, leaving the second eye without them —
    // a one-eye shadow ghost that reads as the car/ped "doubling". Re-arm per eye.
    unsigned short* CShadows_ShadowsStoredToBeRendered;

    // Exact render-list sizes for the profiler (set by ConstructRenderList each
    // frame, before RenderScene) — the true "how many entities/LODs get drawn".
    int* CRenderer_ms_nNoOfVisibleEntities;
    int* CRenderer_ms_nNoOfVisibleLods;
    int* CRenderer_ms_nNoOfVisibleSuperLods;

    // Bounded draw-distance A/B and its low-cost runtime witnesses. The LOD
    // scale is the user-quality lever consumed by CCamera::Process; far clip is
    // read only so world/LOD distance can be separated from timecycle fog.
    float* CRenderer_ms_lodDistScale;
    float* CRenderer_ms_lowLodDistScale;
    float* FadeDistMult;
    float* CRenderer_ms_fFarClipPlane;
    int* CStreaming_ms_numModelsRequested;
    bool* CStreaming_ms_disableStreaming;
    std::uint32_t* CStreaming_ms_numPriorityRequests;
    std::uint32_t* CStreaming_ms_memoryUsed;
    std::uint32_t* CStreaming_ms_memoryAvailable;

    // Read-only population witnesses for the 1 Hz profiler. These are exported
    // globals in retail 2.11; telemetry samples them on the GameThread and never
    // changes the values. Some CCarCtrl counters are signed in the original game
    // even though a valid live count is non-negative.
    std::int32_t*  CCarCtrl_NumRandomCars;
    std::uint32_t* CCarCtrl_NumLawEnforcerCars;
    std::int32_t*  CCarCtrl_NumMissionCars;
    std::uint32_t* CCarCtrl_NumParkedCars;
    std::int32_t*  CCarCtrl_NumPermanentVehicles;
    std::uint32_t* CCarCtrl_NumAmbulancesOnDuty;
    std::uint32_t* CCarCtrl_NumFireTrucksOnDuty;
    std::uint32_t* CCarCtrl_LastTimeLawEnforcerCreated;
    float*         CCarCtrl_CarDensityMultiplier;
    std::uint32_t* CCarCtrl_MaxNumberOfCarsInUse;
    void           (*CCarCtrl_GenerateOneRandomCar)();
    void*          (*FindPlayerWanted)(int playerId);
    bool           (*CPopulation_AddToPopulation)(float, float, float, float);
    float          (*CPopulation_PedCreationDistMultiplier)();
    float*         CPopulation_PedDensityMultiplier;
    std::uint32_t* CPopulation_MaxNumberOfPedsInUse;
    std::uint32_t* CPopulation_ms_nTotalPeds;
    std::uint32_t* CPopulation_ms_nTotalCivPeds;
    std::uint32_t* CPopulation_ms_nTotalGangPeds;
    std::uint32_t* CPopulation_ms_nTotalMissionPeds;
    std::uint32_t* CPopulation_ms_nTotalCarPassengerPeds;

    // Address of the retail CPool<CVehicle>* global. A separate read-only
    // census validates the exact 2.11 pool layout before touching any slot.
    void**         CPools_ms_pVehiclePool;

    // RenderWare immediate mode — draw the VR hands (procedural geometry) into the
    // eye view. Vertices are RxObjSpace3DVertex (see VrCamera.cpp); world-space, so
    // the transform matrix is null.
    void* (*RwIm3DTransform)(void* verts, unsigned int numVerts, void* ltm, unsigned int flags);
    int   (*RwIm3DRenderIndexedPrimitive)(int primType, unsigned short* indices, int numIndices);
    int   (*RwIm3DEnd)();
    int   (*RwRenderStateSet)(int state, void* value);

    // Per-eye stereo: offscreen camera-texture rasters. Redirecting the Scene
    // RwCamera's colour raster (RwCamera+0x80) and depth raster (+0x88) to an eye
    // raster reroutes the whole recorded pass into that eye's FBO. RasterExtOffset
    // is the byte offset of the GL extension inside an RwRaster (holds the FBO/tex).
    void* (*RwRasterCreate)(int width, int height, int depth, int flags);
    void  (*RwRasterDestroy)(void* raster);
    int*   RasterExtOffset;
    void  (*CDraw_SetFOV)(float fov, bool b);
    void* (*FindPlayerPed)(int playerId);   // non-null only during real gameplay
    void* (*FindPlayerVehicle)(int playerId, bool includeRemote);
    bool  (*PlayerIsEnteringCar)();
    // Stable world-space player anchor while seated. Unlike CEntity::m_matrix,
    // the skinned head follows the authored seat transform in every vehicle.
    void  (*CPed_GetBonePosition)(void* ped, Vec3* outPosition,
                                  unsigned int bone, bool updateSkinBones);
    // Exported by Android SA 2.11.311. These let the immersive cockpit reject
    // passengers and every non-automobile without relying on fragile class
    // offsets or vtable slots.
    int   (*CVehicle_GetVehicleAppearance)(const void* vehicle);
    bool  (*CVehicle_IsDriver)(const void* vehicle, const void* ped);
    // CEventKnockOffBike owns rider ejection. The event stores its source bike
    // and fall type, allowing VR to suppress only ordinary flip falls while
    // preserving collision and explosion knock-offs.
    bool  (*CEventKnockOffBike_AffectsPed)(const void* event, void* ped);
    bool  (*CBike_IsComponentPresent)(const void* bike, int componentId);
    void  (*CBike_GetComponentWorldPosition)(void* bike, int componentId,
                                              Vec3* outPosition);
    void  (*CBike_CalculateLeanMatrix)(void* bike);
    void  (*CBike_PreRender)(void* bike);
    void  (*CBmx_PreRender)(void* bike);
    const char* (*GetFrameNodeName)(void* frame);
    void*  Scene;                            // CScene { RpWorld* @0x0; RwCamera* @0x8 }

    // Player anchor for the first-person VR camera. FindPlayerCoors returns the
    // CVector as an HFA in s0/s1/s2 (verified by disassembly), so a plain 3-float
    // POD return matches the ABI.
    Vec3  (*FindPlayerCoors)(int playerId);
    float (*FindPlayerHeading)(int playerId);

    // Player movement: the forwarded gamepad axes land in a dead-end provider that
    // on-foot movement never polls. Writing CPad::GetPad(0)->NewState.LeftStickX/Y
    // (int16, +-127, forward = negative Y) directly, right after UpdatePads rebuilds
    // NewState, is what actually walks the player.
    void* (*CPad_GetPad)(int padNumber);   // CPad*, NewState at CPad+0x00
    void  (*CPad_UpdatePads)();             // hook target
    int   (*CPad_GetAccelerate)(void* pad);
    int   (*CPad_GetBrake)(void* pad);
    int   (*CPad_GetSteeringLeftRight)(void* pad);
    int   (*CPad_GetSteeringUpDown)(void* pad);
    int   (*CPad_GetHandBrake)(void* pad);
    void* CPad_GetHorn;
    float* CStats_StatTypesFloat;   // stat array, indexed by stat id
    unsigned char* CHud_bDrawingVitalStats;
    void* CPlayerPed_ProcessGroupBehaviour;
    void* CPlayerPed_MakeThisPedJoinOurGroup;
    void* CPad_GetGroupControlForward;
    bool  (*CPad_GetSprint)(void* pad, int sprintType);
    bool  (*CPad_NextStationJustUp)(void* pad);
    // Mobile SA's enter/exit query polls the touch ENTER_CAR widget rather than
    // CControllerState.ButtonTriangle. Hooking this exact exported query lets
    // Touch Y behave like Vice City's Triangle without synthesising screen taps.
    bool  (*CPad_ExitVehicleJustDown)(void* pad, bool onFoot, void* vehicle,
                                      bool arg3, const Vec3* position);
    // Android's on-foot fire path does not consume GamepadProvider axes (or
    // CPad::NewState.ButtonCircle). It polls touch widgets directly inside
    // CPad::GetWeapon, so the VR trigger is merged at this narrow query.
    int   (*CPad_GetWeapon)(void* pad, void* ped, bool allowPassive);

    // --- globals ---
    void*  TheCamera;                    // CCamera; m_mCameraMatrix at +0x970
    void*  FrontEndMenuManager;
    // Script-owned master HUD gate checked by CWidgetPlayerInfo::Draw. The VR
    // capture temporarily opens it while drawing the stock player-info widget.
    bool*  CTheScripts_bDisplayHud;
    float* CDraw_ms_fFOV;
    float* CDraw_ms_fNearClipZ;
    float* CDraw_ms_fFarClipZ;
    float* CTimer_ms_fTimeStep;
    float* CTimer_ms_fTimeStepNonClipped;
    float* CTimer_game_FPS;
    std::uint32_t* CTimer_m_FrameCounter;
    std::uint32_t* CTimer_m_snTimeInMilliseconds;
    bool*  CTimer_bSkipProcessThisFrame;
    std::uint32_t* skipFrame;
    bool*  CTimer_m_UserPause;
    bool*  CTimer_m_CodePause;
    void   (*CTimer_StartUserPause)();
    void   (*CTimer_EndUserPause)();
    bool   (*AndroidPaused)();
    void   (*SetAndroidPaused)(int paused);

    // Base address libGame.so is mapped at, for turning a runtime address back
    // into the file offset seen in the symbol dumps under recon/.
    std::uintptr_t LoadBase;
};

extern GameSymbols g;

// Resolve everything above out of an already-loaded libGame.so. Logs one line
// per symbol and returns false if any required symbol is missing, so a broken
// game version fails loudly at startup instead of crashing later in a hook.
bool ResolveGameSymbols(void* libGameHandle);

} // namespace savr
