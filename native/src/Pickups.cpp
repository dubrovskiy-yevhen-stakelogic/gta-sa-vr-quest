// VR world-pickup interaction (layer 1: physical rise).
//
// CPickup::Update(CPlayerPed*, CVehicle*, int) runs once per near-the-player
// pickup each frame and refreshes the visible object's world position from the
// compressed CPickup::m_vecPos. We trampoline-hook it, let the original run,
// then lift the object while the player stands inside the pickup's zone so map
// items rise up to be grabbed. A later layer turns the rise into a hand-grab
// collection.
//
// Verified Android (GTA:SA 2.11.311 arm64) CPickup layout, from the retail
// libGame.so disassembly of CPickup::Update/GiveUsAPickUpObject/GetRidOfObjects:
//   +0x04  CObject*  m_pObject          (packed 8-byte pointer)
//   +0x14  int16     m_vecPos.x         (world = value / 8)
//   +0x16  int16     m_vecPos.y
//   +0x18  int16     m_vecPos.z
//   +0x1c  int16     m_nModelIndex
//   +0x20  uint8     m_nPickupType
// CEntity matrix pointer at entity+0x18; its translation at matrix+0x30/34/38.
#include "Pickups.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Log.h"
#include "PhysicalWeapon.h"
#include "VrCamera.h"
#include "Xr.h"

namespace savr::pickups {
namespace {

constexpr int kObjOff        = 0x04;   // CPickup::m_pObject
constexpr int kPosXOff       = 0x14;   // CPickup::m_vecPos.x (int16 /8)
constexpr int kPosYOff       = 0x16;
constexpr int kPosZOff       = 0x18;
constexpr int kEntityMatrix  = 0x18;   // CEntity::m_matrix pointer
constexpr int kMatrixPos     = 0x30;   // CMatrix translation x (y +4, z +8)

constexpr float kRiseZoneM   = 3.5f;   // player distance where the rise begins
constexpr float kRiseHeightM = 0.80f;  // full lift when the player is on top
constexpr float kBobM        = 0.06f;  // gentle bob amplitude

// Layer 2: hand-grab collection. A squeezed grip within this range of the
// risen item collects it through the RETAIL pickup handler (money, ammo,
// weapon rules, sounds, regeneration all stay stock).
constexpr float kGrabRangeM  = 0.32f;
constexpr float kGrabGrip    = 0.60f;

using UpdateFn = bool (*)(void* self, void* player, void* vehicle, int playerId);
UpdateFn g_origUpdate = nullptr;
const std::uint32_t* g_timeMs = nullptr;   // CTimer::m_snTimeInMilliseconds
// Weapon-swap gate: standing on a weapon pickup with that slot occupied only
// collects while this engine global is armed (the flat game arms it from the
// "collect weapon" button; VR arms it from the physical grab).
std::int32_t* g_collectBuffer = nullptr;   // ::CollectPickupBuffer
int (*g_weaponForModel)(int modelId) = nullptr;   // CPickups::WeaponForModel

// A weapon grabbed by hand should end up IN that hand. The game grants the
// weapon a few frames after collection (GiveDelayedWeapon), so the ForceHold
// is retried from Tick() until it lands or times out.
std::atomic<int>           g_pendingHoldHand{-1};
std::atomic<int>           g_pendingHoldSlot{-1};
std::atomic<std::uint32_t> g_pendingHoldUntilMs{0};

// SA weapon-slot layout (eWeaponType -> ped weapon slot) — same table the
// cheat give-to-hand path uses.
int SlotForWeaponType(int type) {
    if (type==0||type==1) return 0;
    if (type>=2&&type<=9) return 1;
    if (type>=10&&type<=15) return 10;
    if (type==16||type==17||type==18||type==39) return 8;
    if (type>=22&&type<=24) return 2;
    if (type>=25&&type<=27) return 3;
    if (type==28||type==29||type==32) return 4;
    if (type==30||type==31) return 5;
    if (type==33||type==34) return 6;
    if (type>=35&&type<=38) return 7;
    if (type==40) return 12;
    if (type>=41&&type<=43) return 9;
    if (type>=44&&type<=46) return 11;
    return -1;
}

float ReadPos16(const void* base, int off) {
    return static_cast<float>(*reinterpret_cast<const std::int16_t*>(
               reinterpret_cast<const std::uint8_t*>(base) + off)) / 8.0f;
}

void* MatrixOf(const void* entity) {
    if (!entity) return nullptr;
    return *reinterpret_cast<void* const*>(
        reinterpret_cast<const std::uint8_t*>(entity) + kEntityMatrix);
}

bool OnPickupUpdate(void* self, void* player, void* vehicle, int playerId) {
    const bool result = g_origUpdate
        ? g_origUpdate(self, player, vehicle, playerId)
        : false;
    if (!self || !player) return result;

    // The visible object is null while the pickup is out of range or waiting to
    // regenerate; nothing to lift then.
    void* obj = *reinterpret_cast<void* const*>(
        reinterpret_cast<const std::uint8_t*>(self) + kObjOff);
    void* objMat = MatrixOf(obj);
    if (!objMat) return result;

    void* plMat = MatrixOf(player);
    if (!plMat) return result;
    const float* plT = reinterpret_cast<const float*>(
        reinterpret_cast<std::uint8_t*>(plMat) + kMatrixPos);

    const float px = ReadPos16(self, kPosXOff);
    const float py = ReadPos16(self, kPosYOff);
    const float pz = ReadPos16(self, kPosZOff);
    const float dx = plT[0] - px, dy = plT[1] - py, dz = plT[2] - pz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(dist) || dist > kRiseZoneM) return result;

    // Deeper inside the zone -> higher lift, plus a small time-based bob.
    const float t = 1.0f - dist / kRiseZoneM;
    float bob = 0.0f;
    if (g_timeMs) {
        const float sec = static_cast<float>(*g_timeMs) * 0.001f;
        bob = std::sin(sec * 3.0f) * kBobM;
    }
    const float lift = t * kRiseHeightM + t * bob;

    // The original wrote objMat.z = pickup.z this frame; add the lift for render.
    float* objZ = reinterpret_cast<float*>(
        reinterpret_cast<std::uint8_t*>(objMat) + kMatrixPos + 8);
    *objZ += lift;

    // ---- layer 2: hand grab ------------------------------------------------
    // A squeezed grip touching the risen item collects it. Mechanism: the
    // retail CPickup::Update collects when the PLAYER stands inside the
    // pickup radius, so for one extra call the pickup's compressed position
    // is moved onto the player and then restored. Collection, payment
    // checks, sounds and the regeneration timer all stay retail; restoring
    // the position keeps the respawn point (and the save state) at the
    // authored spot even if the retail handler declines to collect.
    xr::HandPose hands[2]{};
    if (!physicalweapon::GetHandPosesSnapshot(hands)) return result;
    const float* objT = reinterpret_cast<const float*>(
        reinterpret_cast<std::uint8_t*>(objMat) + kMatrixPos);
    int grabHand = -1;
    for (int hand = 0; hand < 2 && grabHand < 0; ++hand) {
        if (!hands[hand].valid || hands[hand].grip < kGrabGrip) continue;
        float world[3]{};
        if (!vrcam::TrackingPointToWorld(hands[hand].gripPos, world)) break;
        const float hx = world[0] - objT[0];
        const float hy = world[1] - objT[1];
        const float hz = world[2] - objT[2];
        const float handDist = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (std::isfinite(handDist) && handDist <= kGrabRangeM) grabHand = hand;
    }
    if (grabHand < 0 || !g_origUpdate) return result;

    const std::int16_t model = *reinterpret_cast<const std::int16_t*>(
        reinterpret_cast<const std::uint8_t*>(self) + 0x1c);

    auto* posWords = reinterpret_cast<std::int16_t*>(
        reinterpret_cast<std::uint8_t*>(self) + kPosXOff);
    const std::int16_t saved[3] = {posWords[0], posWords[1], posWords[2]};
    posWords[0] = static_cast<std::int16_t>(plT[0] * 8.0f);
    posWords[1] = static_cast<std::int16_t>(plT[1] * 8.0f);
    posWords[2] = static_cast<std::int16_t>(plT[2] * 8.0f);
    // Weapon-swap pickups (bat in the slot, grabbing a shovel) refuse to
    // collect until the engine's collect button is armed - the physical
    // grab IS that press. The engine decrements the buffer on its own.
    if (g_collectBuffer) *g_collectBuffer = 6;
    const bool collected = g_origUpdate(self, player, vehicle, playerId);
    posWords[0] = saved[0];
    posWords[1] = saved[1];
    posWords[2] = saved[2];

    if (collected) {
        // Land the collected weapon in the hand that grabbed it. The grant
        // is delayed (GiveDelayedWeapon), so Tick() retries the hold.
        const int weaponType = g_weaponForModel ? g_weaponForModel(model) : 0;
        const int slot = weaponType > 0 ? SlotForWeaponType(weaponType) : -1;
        if (slot > 0 && g_timeMs) {
            g_pendingHoldHand.store(grabHand, std::memory_order_relaxed);
            g_pendingHoldSlot.store(slot, std::memory_order_relaxed);
            g_pendingHoldUntilMs.store(*g_timeMs + 1500u,
                                       std::memory_order_release);
        }
    }
    static unsigned grabLogCount = 0;
    if (++grabLogCount <= 20 || (grabLogCount % 50) == 0) {
        const int pickupType = *reinterpret_cast<const std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>(self) + 0x20);
        // type 18 = PROPERTY_FORSALE: the retail handler buys the house on
        // this same armed-collect path (money/mission checks stay stock).
        LOGI("[pickups] hand grab: hand=%d model=%d type=%d collected=%d",
             grabHand, model, pickupType, collected ? 1 : 0);
    }
    return collected;
}

bool InstallUpdateHook(void* target) {
    if (g_origUpdate) return true;
    // CPickup::Update prologue on 2.11.311 arm64 (must match before we patch).
    constexpr std::uint32_t kExpected[4] = {
        0xd10283ffu, 0xfd0013eau, 0x6d0323e9u, 0xa9047bfdu};
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    std::uint32_t observed[4]{};
    std::memcpy(observed, code, sizeof(observed));
    if (std::memcmp(observed, kExpected, sizeof(observed)) != 0) {
        LOGE("[pickups] CPickup::Update prologue mismatch: %08x %08x %08x %08x",
             observed[0], observed[1], observed[2], observed[3]);
        return false;
    }

    const long pageLong = sysconf(_SC_PAGESIZE);
    if (pageLong <= 0) return false;
    const std::size_t pageSize = static_cast<std::size_t>(pageLong);
    void* tramp = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return false;
    auto* t = reinterpret_cast<std::uint32_t*>(tramp);
    std::memcpy(t, code, 16);
    t[4] = 0x58000051u;   // LDR X17, resume literal
    t[5] = 0xD61F0220u;   // BR X17
    *reinterpret_cast<void**>(t + 6) = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 32);

    const std::uintptr_t start =
        reinterpret_cast<std::uintptr_t>(code) & ~(pageSize - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(code) + 16 + pageSize - 1) &
        ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(tramp, pageSize);
        return false;
    }
    // Trampoline first, then the literal, then arm the branch: a call landing
    // mid-patch must never reach the hook with a null original, nor branch
    // through a literal slot that still holds the original instructions.
    g_origUpdate = reinterpret_cast<UpdateFn>(tramp);
    *reinterpret_cast<void**>(code + 2) = reinterpret_cast<void*>(&OnPickupUpdate);
    code[1] = 0xD61F0220u;   // BR X17
    __atomic_store_n(&code[0], 0x58000051u, __ATOMIC_RELEASE);   // LDR X17, #8
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    LOGI("[pickups] CPickup::Update rise hook installed");
    return true;
}

}  // namespace

void Install(void* handle) {
    if (!handle) return;
    void* target = dlsym(handle, "_ZN7CPickup6UpdateEP10CPlayerPedP8CVehiclei");
    if (!target) {
        LOGE("[pickups] CPickup::Update symbol not found");
        return;
    }
    g_timeMs = reinterpret_cast<const std::uint32_t*>(
        dlsym(handle, "_ZN6CTimer21m_snTimeInMillisecondsE"));
    g_collectBuffer = reinterpret_cast<std::int32_t*>(
        dlsym(handle, "CollectPickupBuffer"));
    g_weaponForModel = reinterpret_cast<int (*)(int)>(
        dlsym(handle, "_ZN8CPickups14WeaponForModelEi"));
    if (!g_collectBuffer)
        LOGW("[pickups] CollectPickupBuffer missing - weapon swaps stay gated");
    InstallUpdateHook(target);
}

void Tick() {
    const int hand = g_pendingHoldHand.load(std::memory_order_relaxed);
    const int slot = g_pendingHoldSlot.load(std::memory_order_relaxed);
    if (hand < 0 || slot <= 0 || !g_timeMs) return;
    if (*g_timeMs > g_pendingHoldUntilMs.load(std::memory_order_acquire)) {
        g_pendingHoldHand.store(-1, std::memory_order_relaxed);
        g_pendingHoldSlot.store(-1, std::memory_order_relaxed);
        return;
    }
    if (physicalweapon::ForceHold(hand, slot)) {
        LOGI("[pickups] grabbed weapon now held: hand=%d slot=%d", hand, slot);
        g_pendingHoldHand.store(-1, std::memory_order_relaxed);
        g_pendingHoldSlot.store(-1, std::memory_order_relaxed);
    }
}

}  // namespace savr::pickups
