#include "Weapon.h"

#include "Log.h"
#include "Symbols.h"
#include "Xr.h"

#include <cstdint>
#include <vector>

namespace savr::weapon {
namespace {

// CPed weapon inventory, all read straight out of CPed::SetCurrentWeapon(int)
// @0x58dcac and confirmed by CPed::ClearWeapons() @0x5876f8 (which Shutdowns a
// CWeapon at each of 0x730,0x750,...,0x8b0 — exactly 13 slots, stride 0x20):
//   m_aWeapons base   = ped + 0x730
//   CWeapon stride    = 0x20   (first field m_eWeapon = the weapon type)
//   slot count        = 13
//   m_nActiveWeapon   = int8 @ ped + 0x8DC
constexpr int kOffWeapons    = 0x730;
constexpr int kWeaponStride  = 0x20;
constexpr int kWeaponSlots   = 13;
constexpr int kOffActiveSlot = 0x8DC;

// RpGeometry / RpMorphTarget layout, every offset disassembly-verified against
// 2.11.311 arm64 libGame.so:
//   geom+0x18 numTriangles (loop count in RpGeometryUnlock)
//   geom+0x1c numVertices  (RpMorphTargetCalcBoundingSphere, RpAtomicInstance)
//   geom+0x38 RpTriangle[]  (stride 8: vidx u16 @0/2/4, matIdx s16 @6)
//   geom+0xa0 RpMorphTarget* (RpAtomicInstance reads it)
//   morph+0x00 parent RpGeometry (back-pointer, used as a self-check)
//   morph+0x18 RwV3d* vertices (12-byte stride {x,y,z})
constexpr int kOffGeomNumTris  = 0x18;
constexpr int kOffGeomNumVerts = 0x1C;
constexpr int kOffGeomTris     = 0x38;
constexpr int kOffGeomMorph    = 0xA0;
constexpr int kOffMorphParent  = 0x00;
constexpr int kOffMorphVerts   = 0x18;

uint32_t gLastSig       = 0xFFFFFFFFu;
int      gFrame         = 0;
int      gPublishedModel = -2;    // model id currently published to the renderer

template <typename T>
T Read(void* base, int off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

// weaponType -> RpGeometry, or nullptr. Fills numVerts/numTris + the model id.
void* WeaponGeometry(int type, int* modelIdOut, int* vertsOut, int* trisOut) {
    // skill MUST be 1 (STD): GetWeaponInfo indexes ms_aWeaponInfo as
    //   0(POOR)=type+25, 1(STD)=type, 2(PRO)=type+36, 3(SPECIAL)=type+47.
    // Only STD gives the direct type->info mapping (the game always calls it with
    // skill=1 when it wants the model). Passing 0 misresolves non-skill weapons.
    void* wi = g.CWeaponInfo_GetWeaponInfo(type, 1 /*STD*/);
    if (wi == nullptr) return nullptr;
    const int modelId = Read<int32_t>(wi, 0x0C /*m_nModelId1*/);
    *modelIdOut = modelId;
    if (modelId < 0) return nullptr;

    void* mi = g.CModelInfo_ms_modelInfoPtrs[modelId];
    if (mi == nullptr) return nullptr;
    void* clump = Read<void*>(mi, 0x40 /*m_pRwObject*/);
    if (clump == nullptr) return nullptr;      // not streamed in yet
    void* atomic = g.GetFirstAtomic(clump);
    if (atomic == nullptr) return nullptr;
    void* geom = Read<void*>(atomic, 0x30 /*RpGeometry*/);
    if (geom == nullptr) return nullptr;

    *vertsOut = Read<int32_t>(geom, kOffGeomNumVerts);
    *trisOut  = Read<int32_t>(geom, kOffGeomNumTris);
    return geom;
}

// Extract the active weapon's object-space positions + triangle indices and hand
// them to the renderer. Cached by model id so we only walk RW on a change.
void PublishActiveWeapon(int active, const int* types) {
    auto clear = [] {
        if (gPublishedModel != -1) { xr::SetWeaponGeometry(nullptr, 0, nullptr, 0); gPublishedModel = -1; }
    };
    if (active < 0 || active >= kWeaponSlots) { clear(); return; }
    const int type = types[active];
    if (type == 0) { clear(); return; }        // unarmed / fists

    int modelId = -1, numVerts = 0, numTris = 0;
    void* geom = WeaponGeometry(type, &modelId, &numVerts, &numTris);

    static int dbg = 0;
    const bool trace = (dbg < 4);
    if (trace) { ++dbg; LOGI("[wpn.dbg] active=%d type=%d geom=%p model=%d pub=%d nv=%d nt=%d",
                            active, type, geom, modelId, gPublishedModel, numVerts, numTris); }

    if (geom == nullptr) return;                // not ready — keep last publish
    if (modelId == gPublishedModel) return;     // already published
    if (numVerts <= 0 || numTris <= 0 || numVerts > 60000 || numTris > 200000) return;

    void* morph = Read<void*>(geom, kOffGeomMorph);
    void* back  = morph ? Read<void*>(morph, kOffMorphParent) : nullptr;
    void* vptr  = morph ? Read<void*>(morph, kOffMorphVerts) : nullptr;
    void* tptr  = Read<void*>(geom, kOffGeomTris);
    if (trace) LOGI("[wpn.dbg]   morph=%p back=%p(geom=%p) vptr=%p tptr=%p", morph, back, geom, vptr, tptr);

    if (morph == nullptr) return;
    if (back != geom) {                         // offsets moved on a new build
        LOGW("[wpn] morph parent %p != geom %p — extraction offsets changed", back, geom);
        return;
    }
    if (vptr == nullptr || tptr == nullptr) return;

    std::vector<float> ov;
    ov.reserve(static_cast<size_t>(numVerts) * 3);
    for (int i = 0; i < numVerts; ++i) {
        const float* p = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vptr) + static_cast<size_t>(i) * 12);
        ov.push_back(p[0]); ov.push_back(p[1]); ov.push_back(p[2]);
    }
    std::vector<unsigned short> oi;
    oi.reserve(static_cast<size_t>(numTris) * 3);
    for (int t = 0; t < numTris; ++t) {
        const uint8_t* tp = reinterpret_cast<uint8_t*>(tptr) + static_cast<size_t>(t) * 8;
        const unsigned short a = *reinterpret_cast<const uint16_t*>(tp + 0);
        const unsigned short b = *reinterpret_cast<const uint16_t*>(tp + 2);
        const unsigned short c = *reinterpret_cast<const uint16_t*>(tp + 4);
        if (a < numVerts && b < numVerts && c < numVerts) { oi.push_back(a); oi.push_back(b); oi.push_back(c); }
    }
    if (oi.empty()) return;

    xr::SetWeaponGeometry(ov.data(), numVerts, oi.data(), static_cast<int>(oi.size()));
    gPublishedModel = modelId;
    LOGI("[wpn] published active model=%d verts=%d tris=%d idx=%d", modelId, numVerts, numTris, static_cast<int>(oi.size()));
}

}  // namespace

void Update() {
    if (g.FindPlayerPed == nullptr || g.CWeaponInfo_GetWeaponInfo == nullptr ||
        g.CModelInfo_ms_modelInfoPtrs == nullptr || g.GetFirstAtomic == nullptr)
        return;

    void* ped = g.FindPlayerPed(-1);
    if (ped == nullptr) { xr::SetWeaponGeometry(nullptr, 0, nullptr, 0); gPublishedModel = -1; return; }

    int types[kWeaponSlots];
    uint32_t sig = 0;
    for (int s = 0; s < kWeaponSlots; ++s) {
        types[s] = Read<int32_t>(ped, kOffWeapons + s * kWeaponStride);
        sig = sig * 131u + static_cast<uint32_t>(types[s] + 1);
    }
    const int active = Read<int8_t>(ped, kOffActiveSlot);

    // Publish the active weapon every frame (self-throttled by the model cache),
    // so we catch a weapon switch or a model finishing streaming immediately.
    PublishActiveWeapon(active, types);

    // Throttled inventory log (change or ~every 150 frames), for diagnostics.
    if (sig == gLastSig && (++gFrame % 150 != 0)) return;
    gLastSig = sig;
    LOGI("[wpn] ==== inventory scan (active slot=%d) ====", active);
    for (int s = 0; s < kWeaponSlots; ++s) {
        const int type = types[s];
        if (type == 0) continue;
        int modelId = -1, verts = 0, tris = 0;
        void* geom = WeaponGeometry(type, &modelId, &verts, &tris);
        if (geom != nullptr)
            LOGI("[wpn]  slot=%d type=%d model=%d verts=%d tris=%d%s",
                 s, type, modelId, verts, tris, (s == active ? "  <== ACTIVE" : ""));
        else
            LOGW("[wpn]  slot=%d type=%d model=%d NOT READY", s, type, modelId);
    }
}

}  // namespace savr::weapon
