#include "Symbols.h"

#include "Log.h"

#include <dlfcn.h>
#include <link.h>

#include <cstring>
#include <iterator>

namespace savr {

GameSymbols g{};

namespace {

// One row per symbol we need. `required` separates "the port cannot work without
// this" from "nice to have, tell me if it moved".
struct Entry {
    const char* mangled;
    void**      slot;
    bool        required;
};

bool ResolveOne(void* handle, const Entry& e) {
    dlerror();
    void* addr = dlsym(handle, e.mangled);

    if (addr != nullptr) {
        *e.slot = addr;
        LOGI("  %-54s %p", e.mangled, addr);
        return true;
    }

    const char* err = dlerror();
    if (e.required) {
        LOGE("  %-54s MISSING (%s)", e.mangled, err ? err : "no error text");
    } else {
        LOGW("  %-54s MISSING (%s)", e.mangled, err ? err : "no error text");
    }
    return !e.required;
}

// dl_iterate_phdr rather than dlinfo: dlinfo only exists from API 30, and the
// load base is wanted precisely on older devices too. Runtime address minus this
// gives the file offset printed in recon/gtasa211-dynsym.txt.
int MatchLibGame(dl_phdr_info* info, size_t, void* out) {
    if (info->dlpi_name == nullptr) {
        return 0;
    }
    const char* slash = std::strrchr(info->dlpi_name, '/');
    const char* name  = slash ? slash + 1 : info->dlpi_name;
    if (std::strcmp(name, "libGame.so") != 0) {
        return 0;
    }
    *static_cast<std::uintptr_t*>(out) = static_cast<std::uintptr_t>(info->dlpi_addr);
    return 1; // stop iterating
}

std::uintptr_t FindLoadBase() {
    std::uintptr_t base = 0;
    dl_iterate_phdr(MatchLibGame, &base);
    return base;
}

} // namespace

bool ResolveGameSymbols(void* handle) {
    const Entry table[] = {
        // engine
        {"_Z11RenderSceneb",                                                     reinterpret_cast<void**>(&g.RenderScene),              true },
        {"_Z4IdlePvb",                                                           reinterpret_cast<void**>(&g.Idle),                    true },
        {"renderQueue",                                                          reinterpret_cast<void**>(&g.RenderQueue_global),       false},
        {"_ZN11RenderQueue10ProcessAllEv",                                      reinterpret_cast<void**>(&g.RenderQueue_ProcessAll),   false},
        {"_ZN11RenderQueue4LockEv",                                              reinterpret_cast<void**>(&g.RenderQueue_Lock),         false},
        {"_ZN11RenderQueue6UnlockEv",                                            reinterpret_cast<void**>(&g.RenderQueue_Unlock),       false},
        {"_ZN11RenderQueue12Reset_LockedEv",                                    reinterpret_cast<void**>(&g.RenderQueue_ResetLocked),  false},
        {"_ZN13RQIndexBuffer7SetSafeEv",                                         reinterpret_cast<void**>(&g.RQIndexBuffer_SetSafe),    false},
        {"_ZN14RQVertexBuffer7SetSafeEv",                                        reinterpret_cast<void**>(&g.RQVertexBuffer_SetSafe),   false},
        {"_Z16OS_CanGameRenderv",                                                reinterpret_cast<void**>(&g.OS_CanGameRender),         false},

        // JNI boundary — the frame and the input path
        {"Java_com_rockstargames_oswrapper_GameNative_implOnDrawFrame",          reinterpret_cast<void**>(&g.implOnDrawFrame),          true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnSurfaceChanged",     reinterpret_cast<void**>(&g.implOnSurfaceChanged),     true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnSurfaceDestroyed",   reinterpret_cast<void**>(&g.implOnSurfaceDestroyed),   true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnSurfaceCreated",     reinterpret_cast<void**>(&g.implOnSurfaceCreated),     true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnResume",             reinterpret_cast<void**>(&g.implOnResume),             true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnPause",              reinterpret_cast<void**>(&g.implOnPause),              true },
        {"_Z19OS_RockstarShowGatei",                                             reinterpret_cast<void**>(&g.OS_RockstarShowGate),       true },
        {"_Z25OS_OnRockstarGateCompleteib",                                       reinterpret_cast<void**>(&g.OS_OnRockstarGateComplete), true },
        {"_Z22OS_RockstarShowInitialv",                                          reinterpret_cast<void**>(&g.OS_RockstarShowInitial),      true },
        {"_Z28OS_OnRockstarInitialCompletev",                                     reinterpret_cast<void**>(&g.OS_OnRockstarInitialComplete), true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnTouchStart",         reinterpret_cast<void**>(&g.implOnTouchStart),        true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnTouchMove",          reinterpret_cast<void**>(&g.implOnTouchMove),         true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnTouchEnd",           reinterpret_cast<void**>(&g.implOnTouchEnd),          true },
        {"_ZN15CAEStreamThread7ServiceEv",                                        reinterpret_cast<void**>(&g.CAEStreamThread_Service), true },
        {"_ZN17CAEMP3TrackLoader13GetDataStreamEj",                              reinterpret_cast<void**>(&g.CAEMP3_GetDataStream), true },
        {"_ZN12CAudioEngine20PreloadCutsceneTrackEsh",                          reinterpret_cast<void**>(&g.CAudioEngine_PreloadCutscene), true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnBackButtonPressed",   reinterpret_cast<void**>(&g.implOnBackButtonPressed), true },
        {"_ZN12CCutsceneMgr12SkipCutsceneEv",                                     reinterpret_cast<void**>(&g.CCutsceneMgr_Skip),      false},
        {"_ZN12CCutsceneMgr13StartCutsceneEv",                                    reinterpret_cast<void**>(&g.CCutsceneMgr_Start),     false},
        {"_ZN12CCutsceneMgr10ms_runningE",                                        reinterpret_cast<void**>(&g.CCutsceneMgr_ms_running), false},
        {"_ZN12CCutsceneMgr15ms_cutsceneNameE",                                  reinterpret_cast<void**>(&g.CCutsceneMgr_ms_cutsceneName), false},
        {"Java_com_rockstargames_oswrapper_GameNative_implOnGamepadConnected",   reinterpret_cast<void**>(&g.implOnGamepadConnected),   true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnGamepadAxesChanged", reinterpret_cast<void**>(&g.implOnGamepadAxesChanged), true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnGamepadButtonDown",  reinterpret_cast<void**>(&g.implOnGamepadButtonDown),  true },
        {"Java_com_rockstargames_oswrapper_GameNative_implOnGamepadButtonUp",    reinterpret_cast<void**>(&g.implOnGamepadButtonUp),    true },

        // camera / stereo injection
        {"_ZN7CCamera22CalculateDerivedValuesEbb",                               reinterpret_cast<void**>(&g.CCamera_CalculateDerivedValues),        true },
        {"_ZN7CCamera23CopyCameraMatrixToRWCamEb",                               reinterpret_cast<void**>(&g.CCamera_CopyCameraMatrixToRWCam),       true },
        {"_ZN7CCamera19GetScreenFadeStatusEv",                                   reinterpret_cast<void**>(&g.CCamera_GetScreenFadeStatus),           false},
        {"_ZN18CVisibilityPlugins19SetRenderWareCameraEP8RwCamera",             reinterpret_cast<void**>(&g.CVisibilityPlugins_SetRenderWareCamera), true },
        {"_Z19RwCameraBeginUpdateP8RwCamera",                                    reinterpret_cast<void**>(&g.RwCameraBeginUpdate),                   true },
        {"_Z17RwCameraEndUpdateP8RwCamera",                                      reinterpret_cast<void**>(&g.RwCameraEndUpdate),                     true },
        {"_Z13RwCameraClearP8RwCameraP6RwRGBAi",                                 reinterpret_cast<void**>(&g.RwCameraClear),                         true },
        {"_ZN7CClouds14RenderSkyPolysEv",                                        reinterpret_cast<void**>(&g.CClouds_RenderSkyPolys),                 false},
        {"_ZN7CClouds6RenderEv",                                                 reinterpret_cast<void**>(&g.CClouds_Render),                        false},
        {"_Z12DefinedStatev",                                                    reinterpret_cast<void**>(&g.DefinedState),                          false},
        {"_ZN4CHud4DrawEv",                                                     reinterpret_cast<void**>(&g.CHud_Draw),                             false},
        {"_ZN4CHud13DrawAfterFadeEv",                                          reinterpret_cast<void**>(&g.CHud_DrawAfterFade),                    false},
        {"_ZN15CTouchInterface7DrawAllEb",                                      reinterpret_cast<void**>(&g.CTouchInterface_DrawAll),               false},
        {"_ZN9CMessages7DisplayEh",                                             reinterpret_cast<void**>(&g.CMessages_Display),                     false},
        {"_ZN4CHud9m_MessageE",                                                reinterpret_cast<void**>(&g.CHud_m_Message),                        false},
        {"_ZN4CHud12m_BigMessageE",                                           reinterpret_cast<void**>(&g.CHud_m_BigMessage),                     false},
        {"_ZN15CWidgetHelpText11m_pInstanceE",                                reinterpret_cast<void**>(&g.CWidgetHelpText_m_pInstance),           false},
        {"_ZN5CFont16RenderFontBufferEv",                                       reinterpret_cast<void**>(&g.CFont_RenderFontBuffer),                false},
        {"_ZN5CFont12InitPerFrameEv",                                           reinterpret_cast<void**>(&g.CFont_InitPerFrame),                    false},
        {"SkipIntroCutscene",                                                   reinterpret_cast<void**>(&g.SkipIntroCutscene),                     false},
        {"gMobileMenu",                                                         reinterpret_cast<void**>(&g.gMobileMenu),                           false},
        {"_ZN18CVisibilityPlugins21RenderWeaponPedsForPCEv",                    reinterpret_cast<void**>(&g.CVisibilityPlugins_RenderWeaponPedsForPC), false},
        {"_ZN12CUserDisplay10OnscnTimerE",                                      reinterpret_cast<void**>(&g.CUserDisplay_OnscnTimer),               false},
        {"_Z29GetAnimHierarchyFromSkinClumpP7RpClump",                          reinterpret_cast<void**>(&g.GetAnimHierarchyFromSkinClump),         false},
        {"_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi",                           reinterpret_cast<void**>(&g.RpHAnimIDGetIndex),                     false},
        {"_Z30RpHAnimHierarchyGetMatrixArrayP16RpHAnimHierarchy",               reinterpret_cast<void**>(&g.RpHAnimHierarchyGetMatrixArray),        false},
        {"_Z17RwMatrixTranslateP11RwMatrixTagPK5RwV3d15RwOpCombineType",         reinterpret_cast<void**>(&g.RwMatrixTranslate),                     false},
        {"_Z14RwMatrixRotateP11RwMatrixTagPK5RwV3df15RwOpCombineType",           reinterpret_cast<void**>(&g.RwMatrixRotate),                        false},
        {"_ZN15CTouchInterface10m_pWidgetsE",                                   reinterpret_cast<void**>(&g.CTouchInterface_m_pWidgets),            false},
        {"_ZTV17CWidgetPlayerInfo",                                             reinterpret_cast<void**>(&g.CWidgetPlayerInfo_vtable),              false},
        {"_ZN17CWidgetPlayerInfo4DrawEv",                                       reinterpret_cast<void**>(&g.CWidgetPlayerInfo_Draw),                false},
        {"RsGlobal",                                                             reinterpret_cast<void**>(&g.RsGlobal),                              false},
        {"_ZN4CPed16SetCurrentWeaponEi",                                         reinterpret_cast<void**>(&g.CPed_SetCurrentWeaponSlot),             false},
        {"_ZN4CPed10GiveWeaponE11eWeaponTypejb",                                 reinterpret_cast<void**>(&g.CPed_GiveWeapon),                       false},
        {"_ZNK4CPed7IsAliveEv",                                                  reinterpret_cast<void**>(&g.CPed_IsAlive),                          false},
        {"_ZN20CPedGeometryAnalyser7IsInAirERK4CPed",                            reinterpret_cast<void**>(&g.CPedGeometryAnalyser_IsInAir),          false},
        {"_ZN16CPedIntelligence20SetTaskDuckSecondaryEt",                        reinterpret_cast<void**>(&g.CPedIntelligence_SetTaskDuckSecondary), false},
        {"_ZN16CPedIntelligence22ClearTaskDuckSecondaryEv",                      reinterpret_cast<void**>(&g.CPedIntelligence_ClearTaskDuckSecondary), false},
        {"_ZNK16CPedIntelligence11GetTaskDuckEb",                                reinterpret_cast<void**>(&g.CPedIntelligence_GetTaskDuck),          false},
        {"_ZN15CTouchInterface10IsReleasedENS_9WidgetIDsEP9CVector2Di",          reinterpret_cast<void**>(&g.CTouchInterface_IsReleased),            false},
        {"_ZN15CTouchInterface10IsHeldDownENS_9WidgetIDsEi",                     reinterpret_cast<void**>(&g.CTouchInterface_IsHeldDown),            false},
        {"_ZN15CTouchInterface13IsJustPressedENS_9WidgetIDsEP9CVector2Di",       reinterpret_cast<void**>(&g.CTouchInterface_IsJustPressed),         false},
        {"_ZN15CTouchInterface14IsDoubleTappedENS_9WidgetIDsEbi",                reinterpret_cast<void**>(&g.CTouchInterface_IsDoubleTapped),        false},
        {"_ZN15CTouchInterface9IsTouchedENS_9WidgetIDsEP9CVector2Di",            reinterpret_cast<void**>(&g.CTouchInterface_IsTouchedQuery),        false},
        {"_ZN9CPhysical14ApplyMoveForceE7CVector",                               reinterpret_cast<void**>(&g.CPhysical_ApplyMoveForce),              false},
        {"_ZN11CTheScripts11ScriptSpaceE",                                       reinterpret_cast<void**>(&g.CTheScripts_ScriptSpace),               false},
        {"_ZN11CTheScripts16ScriptsForBrainsE",                                  reinterpret_cast<void**>(&g.CTheScripts_ScriptsForBrains),          false},
        {"_ZN10CStreaming9LoadSceneERK7CVector",                                 reinterpret_cast<void**>(&g.CStreaming_LoadScene),                  false},
        {"_ZN6CWorld19FindGroundZForCoordEff",                                   reinterpret_cast<void**>(&g.CWorld_FindGroundZForCoord),            false},
        {"_ZN4CHID12GetInputTypeEv",                                             reinterpret_cast<void**>(&g.CHID_GetInputType),                     false},
        {"_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_",               reinterpret_cast<void**>(&g.RpClumpForAllAtomics),                  false},
        {"_Z25RpGeometryForAllMaterialsP10RpGeometryPFP10RpMaterialS2_PvES3_",   reinterpret_cast<void**>(&g.RpGeometryForAllMaterials),             false},
        {"_ZN4CPad15GetCarGunUpDownEbP11CAutomobilefb",                          reinterpret_cast<void**>(&g.CPad_GetCarGunUpDown),                  false},
        {"_ZN4CPad18GetCarGunLeftRightEbb",                                      reinterpret_cast<void**>(&g.CPad_GetCarGunLeftRight),               false},
        {"_ZN4CPad14GetCarGunFiredEbb",                                          reinterpret_cast<void**>(&g.CPad_GetCarGunFired),                   false},
        {"_ZN4CPad14CarGunJustDownEv",                                           reinterpret_cast<void**>(&g.CPad_CarGunJustDown),                   false},
        {"_ZN11CAutomobile11TankControlEv",                                      reinterpret_cast<void**>(&g.CAutomobile_TankControl),               false},
        {"_ZN4CPad13GetTurretLeftEv",                                            reinterpret_cast<void**>(&g.CPad_GetTurretLeft),                    false},
        {"_ZN4CPad14GetTurretRightEv",                                           reinterpret_cast<void**>(&g.CPad_GetTurretRight),                   false},
        {"_ZN4CPad12JumpJustDownEv",                                             reinterpret_cast<void**>(&g.CPad_JumpJustDown),                     false},
        {"_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponTypea",                       reinterpret_cast<void**>(&g.CWeaponInfo_GetWeaponInfo),             false},
        {"_ZN10CModelInfo16ms_modelInfoPtrsE",                                   reinterpret_cast<void**>(&g.CModelInfo_ms_modelInfoPtrs),           false},
        {"_ZN10CModelInfo12IsTrainModelEi",                                      reinterpret_cast<void**>(&g.CModelInfo_IsTrainModel),               false},
        {"_ZN10CModelInfo10IsBmxModelEi",                                        reinterpret_cast<void**>(&g.CModelInfo_IsBmxModel),                 false},
        {"_ZN10CModelInfo15IsQuadBikeModelEi",                                   reinterpret_cast<void**>(&g.CModelInfo_IsQuadBikeModel),            false},
        {"_Z14GetFirstAtomicP7RpClump",                                          reinterpret_cast<void**>(&g.GetFirstAtomic),                        false},
        {"_ZN10CStreaming12RequestModelEii",                                     reinterpret_cast<void**>(&g.CStreaming_RequestModel),               false},
        {"_ZN10CStreaming16ms_aInfoForModelE",                                   reinterpret_cast<void**>(&g.CStreaming_ms_aInfoForModel),            false},
        {"_ZN10CStreaming14AddImageToListEPKcb",                                 reinterpret_cast<void**>(&g.CStreaming_AddImageToList),             false},
        {"_ZN10CStreaming11RemoveModelEi",                                       reinterpret_cast<void**>(&g.CStreaming_RemoveModel),                false},
        {"_ZN10CStreaming13FlushChannelsEv",                                     reinterpret_cast<void**>(&g.CStreaming_FlushChannels),              false},
        {"_ZN10CStreaming8ms_filesE",                                            reinterpret_cast<void**>(&g.CStreaming_ms_files),                    false},
        {"_ZN10CModelInfo12GetModelInfoEPKcPi",                                  reinterpret_cast<void**>(&g.CModelInfo_GetModelInfoByName),         false},
        {"_ZN22TextureDatabaseRuntime4LoadEPKcb21TextureDatabaseFormat",         reinterpret_cast<void**>(&g.TextureDatabaseRuntime_Load),           false},
        {"_ZN22TextureDatabaseRuntime11GetDatabaseEPKc",                         reinterpret_cast<void**>(&g.TextureDatabaseRuntime_GetDatabase),    false},
        {"_ZN22TextureDatabaseRuntime11SortEntriesEb",                           reinterpret_cast<void**>(&g.TextureDatabaseRuntime_SortEntries),    false},
        {"_ZN12CCutsceneMgr19ms_pCutsceneObjectsE",                              reinterpret_cast<void**>(&g.CCutsceneMgr_ms_pCutsceneObjects),      false},
        {"_ZN12CCutsceneMgr18ms_numCutsceneObjsE",                               reinterpret_cast<void**>(&g.CCutsceneMgr_ms_numCutsceneObjs),       false},
        {"_ZN10CStreaming22ms_streamingBufferSizeE",                             reinterpret_cast<void**>(&g.CStreaming_ms_streamingBufferSize),     false},
        {"_ZN15CClumpModelInfo14CreateInstanceEv",                               reinterpret_cast<void**>(&g.CClumpModelInfo_CreateInstance),        false},
        {"_ZN14CBaseModelInfo6AddRefEv",                                         reinterpret_cast<void**>(&g.CBaseModelInfo_AddRef),                  false},
        {"_ZN14CBaseModelInfo9RemoveRefEv",                                      reinterpret_cast<void**>(&g.CBaseModelInfo_RemoveRef),               false},
        {"_Z14RpClumpDestroyP7RpClump",                                          reinterpret_cast<void**>(&g.RpClumpDestroy),                        false},
        {"_Z16RwFrameTransformP7RwFramePK11RwMatrixTag15RwOpCombineType",        reinterpret_cast<void**>(&g.RwFrameTransform),                      false},
        {"_Z27AtomicDefaultRenderCallBackP8RpAtomic",                            reinterpret_cast<void**>(&g.AtomicDefaultRenderCallBack),           false},
        {"_Z20RwFrameUpdateObjectsP7RwFrame",                                    reinterpret_cast<void**>(&g.RwFrameUpdateObjects),                  false},
        {"_Z13RpClumpRenderP7RpClump",                                           reinterpret_cast<void**>(&g.RpClumpRender),                         false},
        {"_ZN4CPed13SetupLightingEv",                                            reinterpret_cast<void**>(&g.CPed_SetupLighting),                    false},
        {"_ZN4CPed14RemoveLightingEb",                                           reinterpret_cast<void**>(&g.CPed_RemoveLighting),                   false},
        {"_Z24RwCameraSetNearClipPlaneP8RwCameraf",                              reinterpret_cast<void**>(&g.RwCameraSetNearClipPlane),              false},
        {"_Z23RwCameraSetFarClipPlaneP8RwCameraf",                               reinterpret_cast<void**>(&g.RwCameraSetFarClipPlane),               false},
        {"_ZN7CWeapon14FireInstantHitEP7CEntityP7CVectorS3_S1_S3_S3_bb",         reinterpret_cast<void**>(&g.CWeapon_FireInstantHit),                false},
        {"_ZN7CWeapon10FireSniperEP4CPedP7CEntityP7CVector",                     reinterpret_cast<void**>(&g.CWeapon_FireSniper),                    false},
        {"_ZN7CWeapon4FireEP7CEntityP7CVectorS3_S1_S3_S3_",                     reinterpret_cast<void**>(&g.CWeapon_Fire),                          false},
        {"_ZN7CWeapon14FireProjectileEP7CEntityP7CVectorS1_S3_f",                reinterpret_cast<void**>(&g.CWeapon_FireProjectile),                false},
        {"_ZN15CProjectileInfo13AddProjectileEP7CEntity11eWeaponType7CVectorfPS3_S1_", reinterpret_cast<void**>(&g.CProjectileInfo_AddProjectile), false},
        {"_ZN15CProjectileInfo15ms_apProjectileE",                               reinterpret_cast<void**>(&g.CProjectileInfo_ms_apProjectile),       false},
        {"_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb", reinterpret_cast<void**>(&g.CWorld_ProcessLineOfSight),         false},
        {"_ZN6CWorld22TestSphereAgainstWorldE7CVectorfP7CEntitybbbbbb",              reinterpret_cast<void**>(&g.CWorld_TestSphereAgainstWorld),   false},
        {"gaTempSphereColPoints",                                                     reinterpret_cast<void**>(&g.gaTempSphereColPoints),            false},
        {"_ZN6CWorld13pIgnoreEntityE",                                            reinterpret_cast<void**>(&g.CWorld_pIgnoreEntity),                  false},
        {"_ZN6CWorld16bIncludeDeadPedsE",                                         reinterpret_cast<void**>(&g.CWorld_bIncludeDeadPeds),                false},
        {"_ZN6CWorld16bIncludeCarTyresE",                                         reinterpret_cast<void**>(&g.CWorld_bIncludeCarTyres),                false},
        {"_ZN6CWorld14bIncludeBikersE",                                           reinterpret_cast<void**>(&g.CWorld_bIncludeBikers),                  false},
        {"_ZN4CPed14GetWeaponSkillE11eWeaponType",                                reinterpret_cast<void**>(&g.CPed_GetWeaponSkill),                    false},
        {"_ZN7CWeapon31CheckForShootingVehicleOccupantEPP7CEntityP9CColPoint11eWeaponTypeRK7CVectorS8_", reinterpret_cast<void**>(&g.CWeapon_CheckForShootingVehicleOccupant), false},
        {"_ZN7CWeapon14DoBulletImpactEP7CEntityS1_P7CVectorS3_P9CColPointi",      reinterpret_cast<void**>(&g.CWeapon_DoBulletImpact),                false},
        {"_ZN7CWeapon14DoWeaponEffectE7CVectorS0_",                               reinterpret_cast<void**>(&g.CWeapon_DoWeaponEffect),                 false},
        {"_ZN7CWeapon19GenerateDamageEventEP4CPedP7CEntity11eWeaponTypei14ePedPieceTypesi", reinterpret_cast<void**>(&g.CWeapon_GenerateDamageEvent), false},
        {"_ZN8CVehicle13InflictDamageEP7CEntity11eWeaponTypef7CVector",           reinterpret_cast<void**>(&g.CVehicle_InflictDamage),                false},
        {"_ZN4CPed17GetLocalDirectionERK9CVector2D",                              reinterpret_cast<void**>(&g.CPed_GetLocalDirection),                false},
        {"_ZNK7CEntity14GetBoundCentreER7CVector",                               reinterpret_cast<void**>(&g.CEntity_GetBoundCentre),                false},
        {"_ZN9CPhysical10ApplyForceE7CVectorS0_b",                               reinterpret_cast<void**>(&g.CPhysical_ApplyForce),                  false},
        {"_ZN9CPhysical15AddToMovingListEv",                                     reinterpret_cast<void**>(&g.CPhysical_AddToMovingList),             false},
        {"_ZN7CObject11SetIsStaticEb",                                           reinterpret_cast<void**>(&g.CObject_SetIsStatic),                    false},
        {"_ZN7CObject12ObjectDamageEfP7CVectorS1_P7CEntity11eWeaponType",        reinterpret_cast<void**>(&g.CObject_ObjectDamage),                   false},
        {"AudioEngine",                                                           reinterpret_cast<void**>(&g.AudioEngine),                           false},
        {"_ZN12CAudioEngine15ReportBulletHitEP7CEntityhR7CVectorf",              reinterpret_cast<void**>(&g.CAudioEngine_ReportBulletHit),          false},
        {"_ZN12CAudioEngine15ReportCollisionEP7CEntityS1_hhR7CVectorPS2_ffhh",   reinterpret_cast<void**>(&g.CAudioEngine_ReportCollision),          false},
        {"_ZN12CAudioEngine24ReportFrontendAudioEventEiff",                      reinterpret_cast<void**>(&g.CAudioEngine_ReportFrontendAudioEvent), false},
        {"_ZN13CEventGunShotC1EP7CEntity7CVectorS2_b",                            reinterpret_cast<void**>(&g.CEventGunShot_Construct),                false},
        {"_ZN13CEventGunShotD1Ev",                                                reinterpret_cast<void**>(&g.CEventGunShot_Destruct),                 false},
        {"_Z19GetEventGlobalGroupv",                                              reinterpret_cast<void**>(&g.GetEventGlobalGroup),                    false},
        {"_ZN11CEventGroup3AddER6CEventb",                                        reinterpret_cast<void**>(&g.CEventGroup_Add),                        false},
        {"_ZN7CEntity13GetIsOnScreenEv",                                         reinterpret_cast<void**>(&g.CEntity_GetIsOnScreen),                 false},
        {"_ZN7CCamera15IsSphereVisibleERK7CVectorf",                             reinterpret_cast<void**>(&g.CCamera_IsSphereVisible),               false},
        {"_ZN7CEntity16IsEntityOccludedEv",                                      reinterpret_cast<void**>(&g.CEntity_IsEntityOccluded),              false},
        {"_ZN18CVisibilityPlugins26InsertEntityIntoSortedListEP7CEntityf",        reinterpret_cast<void**>(&g.CVisibilityPlugins_InsertEntityIntoSortedList), false},
        {"_ZN18CVisibilityPlugins17m_alphaEntityListE",                          reinterpret_cast<void**>(&g.CVisibilityPlugins_m_alphaEntityList),   false},
        {"_ZN18CVisibilityPlugins27m_alphaUnderwaterEntityListE",                reinterpret_cast<void**>(&g.CVisibilityPlugins_m_alphaUnderwaterEntityList), false},
        {"_ZN11CWaterLevel22m_nNumOfWaterTrianglesE",                            reinterpret_cast<void**>(&g.CWaterLevel_m_nNumOfWaterTriangles), false},
        {"_ZN11CWaterLevel18m_nNumOfWaterQuadsE",                                reinterpret_cast<void**>(&g.CWaterLevel_m_nNumOfWaterQuads), false},
        {"_ZN11CWaterLevel12m_aTrianglesE",                                      reinterpret_cast<void**>(&g.CWaterLevel_m_aTriangles), false},
        {"_ZN11CWaterLevel8m_aQuadsE",                                           reinterpret_cast<void**>(&g.CWaterLevel_m_aQuads), false},
        {"_ZN9CRenderer26SetupBigBuildingVisibilityEP7CEntityRf",              reinterpret_cast<void**>(&g.CRenderer_SetupBigBuildingVisibility), false},
        {"_ZN9CRenderer24SetupMapEntityVisibilityEP7CEntityP14CBaseModelInfofb", reinterpret_cast<void**>(&g.CRenderer_SetupMapEntityVisibility), false},
        {"_ZN9CRenderer21ShouldModelBeStreamedEP7CEntityRK7CVectorf",             reinterpret_cast<void**>(&g.CRenderer_ShouldModelBeStreamed), false},
        {"_ZN9CRenderer16RenderOneNonRoadEP7CEntity",                            reinterpret_cast<void**>(&g.CRenderer_RenderOneNonRoad), false},
        {"_ZN18CVisibilityPlugins12RenderEntityEPvf",                            reinterpret_cast<void**>(&g.CVisibilityPlugins_RenderEntity), false},
        {"_ZN9CRenderer9ScanWorldEv",                                             reinterpret_cast<void**>(&g.CRenderer_ScanWorld), false},
        {"_ZN9CRenderer14ScanSectorListEii",                                      reinterpret_cast<void**>(&g.CRenderer_ScanSectorList), false},
        {"_ZN9CRenderer19ScanBigBuildingListEii",                                 reinterpret_cast<void**>(&g.CRenderer_ScanBigBuildingList), false},
        {"_ZN9CRenderer20ms_vecCameraPositionE",                                 reinterpret_cast<void**>(&g.CRenderer_ms_vecCameraPosition), false},
        {"_ZN9CRenderer17ms_fCameraHeadingE",                                    reinterpret_cast<void**>(&g.CRenderer_ms_fCameraHeading), false},
        {"_ZN18CVisibilityPlugins33ms_vehicleLod0RenderMultiPassDistE",           reinterpret_cast<void**>(&g.CVisibilityPlugins_ms_vehicleLod0RenderMultiPassDist), false},
        {"_ZN18CVisibilityPlugins18ms_vehicleLod0DistE",                          reinterpret_cast<void**>(&g.CVisibilityPlugins_ms_vehicleLod0Dist), false},
        {"_ZN18CVisibilityPlugins18ms_vehicleLod1DistE",                          reinterpret_cast<void**>(&g.CVisibilityPlugins_ms_vehicleLod1Dist), false},
        {"_ZN18CVisibilityPlugins21ms_bigVehicleLod0DistE",                       reinterpret_cast<void**>(&g.CVisibilityPlugins_ms_bigVehicleLod0Dist), false},
        {"_ZN18CVisibilityPlugins13ms_pedLodDistE",                              reinterpret_cast<void**>(&g.CVisibilityPlugins_ms_pedLodDist), false},
        {"_ZN18CVisibilityPlugins23RenderVehicleHiDetailCBEP8RpAtomic",           reinterpret_cast<void**>(&g.CVisibilityPlugins_RenderVehicleHiDetailCB), false},
        {"_ZN18CVisibilityPlugins28RenderVehicleHiDetailAlphaCBEP8RpAtomic",      reinterpret_cast<void**>(&g.CVisibilityPlugins_RenderVehicleHiDetailAlphaCB), false},
        {"gVehicleDistanceFromCamera",                                            reinterpret_cast<void**>(&g.gVehicleDistanceFromCamera), false},
        {"_ZN11CPopulation12ManageObjectEP7CObjectRK7CVector",                    reinterpret_cast<void**>(&g.CPopulation_ManageObject), false},
        {"_ZN11CPopulation19ConvertToRealObjectEP12CDummyObject",                 reinterpret_cast<void**>(&g.CPopulation_ConvertToRealObject), false},
        {"_ZN11CPopulation20ConvertToDummyObjectEP7CObject",                      reinterpret_cast<void**>(&g.CPopulation_ConvertToDummyObject), false},
        {"_ZN8CShadows19UpdateStaticShadowsEv",                                  reinterpret_cast<void**>(&g.CShadows_UpdateStaticShadows),          false},
        {"_ZN8CShadows25ShadowsStoredToBeRenderedE",                             reinterpret_cast<void**>(&g.CShadows_ShadowsStoredToBeRendered),     false},
        {"_ZN9CRenderer23ms_nNoOfVisibleEntitiesE",                              reinterpret_cast<void**>(&g.CRenderer_ms_nNoOfVisibleEntities),      false},
        {"_ZN9CRenderer19ms_nNoOfVisibleLodsE",                                  reinterpret_cast<void**>(&g.CRenderer_ms_nNoOfVisibleLods),          false},
        {"_ZN9CRenderer24ms_nNoOfVisibleSuperLodsE",                             reinterpret_cast<void**>(&g.CRenderer_ms_nNoOfVisibleSuperLods),     false},
        {"_ZN9CRenderer15ms_lodDistScaleE",                                      reinterpret_cast<void**>(&g.CRenderer_ms_lodDistScale),              false},
        {"_ZN9CRenderer18ms_lowLodDistScaleE",                                   reinterpret_cast<void**>(&g.CRenderer_ms_lowLodDistScale),           false},
        {"FadeDistMult",                                                         reinterpret_cast<void**>(&g.FadeDistMult),                           false},
        {"_ZN9CRenderer16ms_fFarClipPlaneE",                                     reinterpret_cast<void**>(&g.CRenderer_ms_fFarClipPlane),             false},
        {"_ZN10CStreaming21ms_numModelsRequestedE",                              reinterpret_cast<void**>(&g.CStreaming_ms_numModelsRequested),       false},
        {"_ZN10CStreaming19ms_disableStreamingE",                                reinterpret_cast<void**>(&g.CStreaming_ms_disableStreaming),         false},
        {"_ZN10CStreaming22ms_numPriorityRequestsE",                             reinterpret_cast<void**>(&g.CStreaming_ms_numPriorityRequests),      false},
        {"_ZN10CStreaming13ms_memoryUsedE",                                      reinterpret_cast<void**>(&g.CStreaming_ms_memoryUsed),               false},
        {"_ZN10CStreaming18ms_memoryAvailableE",                                 reinterpret_cast<void**>(&g.CStreaming_ms_memoryAvailable),          false},
        {"_ZN8CCarCtrl13NumRandomCarsE",                                         reinterpret_cast<void**>(&g.CCarCtrl_NumRandomCars),                 false},
        {"_ZN8CCarCtrl18NumLawEnforcerCarsE",                                   reinterpret_cast<void**>(&g.CCarCtrl_NumLawEnforcerCars),             false},
        {"_ZN8CCarCtrl14NumMissionCarsE",                                        reinterpret_cast<void**>(&g.CCarCtrl_NumMissionCars),                false},
        {"_ZN8CCarCtrl13NumParkedCarsE",                                         reinterpret_cast<void**>(&g.CCarCtrl_NumParkedCars),                 false},
        {"_ZN8CCarCtrl20NumPermanentVehiclesE",                                 reinterpret_cast<void**>(&g.CCarCtrl_NumPermanentVehicles),           false},
        {"_ZN8CCarCtrl19NumAmbulancesOnDutyE",                                  reinterpret_cast<void**>(&g.CCarCtrl_NumAmbulancesOnDuty),            false},
        {"_ZN8CCarCtrl19NumFireTrucksOnDutyE",                                  reinterpret_cast<void**>(&g.CCarCtrl_NumFireTrucksOnDuty),            false},
        {"_ZN8CCarCtrl26LastTimeLawEnforcerCreatedE",                           reinterpret_cast<void**>(&g.CCarCtrl_LastTimeLawEnforcerCreated),     false},
        {"_ZN8CCarCtrl20CarDensityMultiplierE",                                 reinterpret_cast<void**>(&g.CCarCtrl_CarDensityMultiplier),           false},
        {"_ZN8CCarCtrl20MaxNumberOfCarsInUseE",                                 reinterpret_cast<void**>(&g.CCarCtrl_MaxNumberOfCarsInUse),           false},
        {"_ZN8CCarCtrl20GenerateOneRandomCarEv",                                reinterpret_cast<void**>(&g.CCarCtrl_GenerateOneRandomCar),          false},
        {"_Z16FindPlayerWantedi",                                                reinterpret_cast<void**>(&g.FindPlayerWanted),                        false},
        {"_ZN11CPopulation15AddToPopulationEffff",                              reinterpret_cast<void**>(&g.CPopulation_AddToPopulation),             false},
        {"_ZN11CPopulation25PedCreationDistMultiplierEv",                       reinterpret_cast<void**>(&g.CPopulation_PedCreationDistMultiplier),    false},
        {"_ZN11CPopulation20PedDensityMultiplierE",                             reinterpret_cast<void**>(&g.CPopulation_PedDensityMultiplier),        false},
        {"_ZN11CPopulation20MaxNumberOfPedsInUseE",                             reinterpret_cast<void**>(&g.CPopulation_MaxNumberOfPedsInUse),        false},
        {"_ZN11CPopulation13ms_nTotalPedsE",                                    reinterpret_cast<void**>(&g.CPopulation_ms_nTotalPeds),               false},
        {"_ZN11CPopulation16ms_nTotalCivPedsE",                                 reinterpret_cast<void**>(&g.CPopulation_ms_nTotalCivPeds),            false},
        {"_ZN11CPopulation17ms_nTotalGangPedsE",                                reinterpret_cast<void**>(&g.CPopulation_ms_nTotalGangPeds),           false},
        {"_ZN11CPopulation20ms_nTotalMissionPedsE",                             reinterpret_cast<void**>(&g.CPopulation_ms_nTotalMissionPeds),        false},
        {"_ZN11CPopulation25ms_nTotalCarPassengerPedsE",                        reinterpret_cast<void**>(&g.CPopulation_ms_nTotalCarPassengerPeds),   false},
        {"_ZN6CPools15ms_pVehiclePoolE",                                        reinterpret_cast<void**>(&g.CPools_ms_pVehiclePool),                  false},
        {"_Z15RwIm3DTransformP18RxObjSpace3DVertexjP11RwMatrixTagj",             reinterpret_cast<void**>(&g.RwIm3DTransform),                       false},
        {"_Z28RwIm3DRenderIndexedPrimitive15RwPrimitiveTypePti",                 reinterpret_cast<void**>(&g.RwIm3DRenderIndexedPrimitive),          false},
        {"_Z9RwIm3DEndv",                                                        reinterpret_cast<void**>(&g.RwIm3DEnd),                             false},
        {"_Z16RwRenderStateSet13RwRenderStatePv",                                reinterpret_cast<void**>(&g.RwRenderStateSet),                      false},
        {"_Z14RwRasterCreateiiii",                                               reinterpret_cast<void**>(&g.RwRasterCreate),                        false},
        {"_Z15RwRasterDestroyP8RwRaster",                                        reinterpret_cast<void**>(&g.RwRasterDestroy),                       false},
        {"RasterExtOffset",                                                      reinterpret_cast<void**>(&g.RasterExtOffset),                       false},
        {"_ZN5CDraw6SetFOVEfb",                                                  reinterpret_cast<void**>(&g.CDraw_SetFOV),                          true },
        {"_Z13FindPlayerPedi",                                                   reinterpret_cast<void**>(&g.FindPlayerPed),                         true },
        {"_Z17FindPlayerVehicleib",                                              reinterpret_cast<void**>(&g.FindPlayerVehicle),                     false},
        {"_Z19PlayerIsEnteringCarv",                                             reinterpret_cast<void**>(&g.PlayerIsEnteringCar),                   false},
        {"_ZN4CPed15GetBonePositionER5RwV3djb",                                  reinterpret_cast<void**>(&g.CPed_GetBonePosition),                   false},
        {"_ZNK8CVehicle20GetVehicleAppearanceEv",                                reinterpret_cast<void**>(&g.CVehicle_GetVehicleAppearance),          false},
        {"_ZNK8CVehicle8IsDriverEPK4CPed",                                       reinterpret_cast<void**>(&g.CVehicle_IsDriver),                      false},
        {"_ZNK18CEventKnockOffBike10AffectsPedEP4CPed",                          reinterpret_cast<void**>(&g.CEventKnockOffBike_AffectsPed),           false},
        {"_ZNK5CBike18IsComponentPresentEi",                                    reinterpret_cast<void**>(&g.CBike_IsComponentPresent),              false},
        {"_ZN5CBike25GetComponentWorldPositionEiR7CVector",                     reinterpret_cast<void**>(&g.CBike_GetComponentWorldPosition),       false},
        {"_ZN5CBike19CalculateLeanMatrixEv",                                    reinterpret_cast<void**>(&g.CBike_CalculateLeanMatrix),             false},
        {"_ZN5CBike9PreRenderEv",                                               reinterpret_cast<void**>(&g.CBike_PreRender),                       false},
        {"_ZN4CBmx9PreRenderEv",                                                reinterpret_cast<void**>(&g.CBmx_PreRender),                        false},
        {"_Z16GetFrameNodeNameP7RwFrame",                                      reinterpret_cast<void**>(&g.GetFrameNodeName),                      false},
        {"_Z15FindPlayerCoorsi",                                                 reinterpret_cast<void**>(&g.FindPlayerCoors),                       true },
        {"_Z17FindPlayerHeadingi",                                               reinterpret_cast<void**>(&g.FindPlayerHeading),                     true },
        {"_ZN4CPad6GetPadEi",                                                    reinterpret_cast<void**>(&g.CPad_GetPad),                           true },
        {"_ZN4CPad10UpdatePadsEv",                                               reinterpret_cast<void**>(&g.CPad_UpdatePads),                       true },
        {"_ZN4CPad13GetAccelerateEv",                                            reinterpret_cast<void**>(&g.CPad_GetAccelerate),                    false},
        {"_ZN4CPad8GetBrakeEv",                                                  reinterpret_cast<void**>(&g.CPad_GetBrake),                         false},
        {"_ZN4CPad20GetSteeringLeftRightEv",                                     reinterpret_cast<void**>(&g.CPad_GetSteeringLeftRight),             false},
        {"_ZN4CPad17GetSteeringUpDownEv",                                        reinterpret_cast<void**>(&g.CPad_GetSteeringUpDown),                false},
        {"_ZN4CPad12GetHandBrakeEv",                                             reinterpret_cast<void**>(&g.CPad_GetHandBrake),                     false},
        {"_ZN4CPad7GetHornEb",                                                   reinterpret_cast<void**>(&g.CPad_GetHorn),                          false},
        {"_ZN6CStats14StatTypesFloatE",                                          reinterpret_cast<void**>(&g.CStats_StatTypesFloat),                 false},
        {"_ZN4CHud18bDrawingVitalStatsE",                                        reinterpret_cast<void**>(&g.CHud_bDrawingVitalStats),               false},
        {"_ZN10CPlayerPed21ProcessGroupBehaviourEP4CPad",                        reinterpret_cast<void**>(&g.CPlayerPed_ProcessGroupBehaviour),      false},
        {"_ZN10CPlayerPed23MakeThisPedJoinOurGroupEP4CPed",                      reinterpret_cast<void**>(&g.CPlayerPed_MakeThisPedJoinOurGroup),    false},
        {"_ZN4CPad22GetGroupControlForwardEbb",                                  reinterpret_cast<void**>(&g.CPad_GetGroupControlForward),           false},
        {"_ZN4CPad9GetSprintEi",                                                 reinterpret_cast<void**>(&g.CPad_GetSprint),                        false},
        {"_ZN4CPad17NextStationJustUpEv",                                        reinterpret_cast<void**>(&g.CPad_NextStationJustUp),                 false},
        {"_ZN4CPad17LastStationJustUpEv",                                        reinterpret_cast<void**>(&g.CPad_LastStationJustUp),                 false},
        {"_ZN12CAudioEngine24GetCurrentRadioStationIDEv",                        reinterpret_cast<void**>(&g.CAudioEngine_GetCurrentRadioStationID),  false},
        {"_ZN4CPad19ExitVehicleJustDownEbP8CVehiclebRK7CVector",                  reinterpret_cast<void**>(&g.CPad_ExitVehicleJustDown),              false},
        {"_ZN4CPad9GetWeaponEP4CPedb",                                           reinterpret_cast<void**>(&g.CPad_GetWeapon),                       false},
        {"Scene",                                                                reinterpret_cast<void**>(&g.Scene),                                 true },

        // globals
        {"TheCamera",                                                            reinterpret_cast<void**>(&g.TheCamera),                true },
        {"FrontEndMenuManager",                                                  reinterpret_cast<void**>(&g.FrontEndMenuManager),      true },
        {"_ZN11CTheScripts11bDisplayHudE",                                       reinterpret_cast<void**>(&g.CTheScripts_bDisplayHud), false},
        {"_ZN5CDraw7ms_fFOVE",                                                   reinterpret_cast<void**>(&g.CDraw_ms_fFOV),            true },
        {"_ZN5CDraw13ms_fNearClipZE",                                            reinterpret_cast<void**>(&g.CDraw_ms_fNearClipZ),      true },
        {"_ZN5CDraw12ms_fFarClipZE",                                             reinterpret_cast<void**>(&g.CDraw_ms_fFarClipZ),       true },
        {"_ZN6CTimer12ms_fTimeStepE",                                            reinterpret_cast<void**>(&g.CTimer_ms_fTimeStep),      false},
        {"_ZN6CTimer22ms_fTimeStepNonClippedE",                                  reinterpret_cast<void**>(&g.CTimer_ms_fTimeStepNonClipped), false},
        {"_ZN6CTimer8game_FPSE",                                                 reinterpret_cast<void**>(&g.CTimer_game_FPS),          false},
        {"_ZN6CTimer14m_FrameCounterE",                                          reinterpret_cast<void**>(&g.CTimer_m_FrameCounter),    false},
        {"_ZN6CTimer22m_snTimeInMillisecondsE",                                 reinterpret_cast<void**>(&g.CTimer_m_snTimeInMilliseconds), false},
        {"_ZN6CTimer21bSkipProcessThisFrameE",                                  reinterpret_cast<void**>(&g.CTimer_bSkipProcessThisFrame), false},
        {"skipFrame",                                                            reinterpret_cast<void**>(&g.skipFrame),                false},
        {"_ZN6CTimer11m_UserPauseE",                                             reinterpret_cast<void**>(&g.CTimer_m_UserPause),       false},
        {"_ZN6CTimer11m_CodePauseE",                                             reinterpret_cast<void**>(&g.CTimer_m_CodePause),       false},
        {"_ZN6CTimer14StartUserPauseEv",                                         reinterpret_cast<void**>(&g.CTimer_StartUserPause),    false},
        {"_ZN6CTimer12EndUserPauseEv",                                           reinterpret_cast<void**>(&g.CTimer_EndUserPause),      false},
        {"_Z13AndroidPausedv",                                                   reinterpret_cast<void**>(&g.AndroidPaused),            false},
        {"_Z16SetAndroidPausedi",                                                reinterpret_cast<void**>(&g.SetAndroidPaused),         false},
    };

    g.LoadBase = FindLoadBase();
    LOGI("libGame.so load base %p, resolving %zu symbols:",
         reinterpret_cast<void*>(g.LoadBase), std::size(table));

    bool ok = true;
    for (const Entry& e : table) {
        ok = ResolveOne(handle, e) && ok;
    }

    if (!ok) {
        LOGE("symbol resolution FAILED — wrong game version?");
        return false;
    }

    LOGI("all required symbols resolved");

    // Prove these really are the game's globals and not a coincidence: the
    // renderer sets them up long before the first frame, so the values must be
    // sane. A garbage FOV here means the symbol moved and nothing else is safe.
    LOGI("sanity: FOV=%.2f near=%.3f far=%.1f",
         static_cast<double>(*g.CDraw_ms_fFOV),
         static_cast<double>(*g.CDraw_ms_fNearClipZ),
         static_cast<double>(*g.CDraw_ms_fFarClipZ));
    return true;
}

} // namespace savr
