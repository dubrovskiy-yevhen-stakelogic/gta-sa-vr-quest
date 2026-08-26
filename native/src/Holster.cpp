#include "Holster.h"

#include "Calib.h"
#include "Log.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace savr::holster {
namespace {

constexpr int kDefaultSlots[POINT_COUNT] = {4, 2, 3, 1, 8, 7, 5};
constexpr int kCenterThrowableSlot = 8;
constexpr int kEmptySlot = -1;

// CPed::m_aWeapons is a 13-entry CWeapon array on this Android build. CWeapon's
// first int32 is m_eWeaponType; zero is unarmed / no weapon in that slot.
constexpr int kOffWeapons = 0x730;
constexpr int kWeaponStride = 0x20;
constexpr int kWeaponSlotCount = 13;
constexpr int kOffActiveWeaponSlot = 0x8DC;

const char* kSettingsPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_holsters.ini";

const PointMetadata kMetadata[POINT_COUNT] = {
    {"WAIST LEFT",              -0.27f, -0.58f,  0.07f, false},
    {"WAIST RIGHT",              0.27f, -0.58f,  0.07f, false},
    {"CHEST LEFT",              -0.21f, -0.30f,  0.12f, false},
    {"CHEST RIGHT",              0.21f, -0.30f,  0.12f, false},
    {"CHEST CENTER THROWABLE",   0.00f, -0.36f,  0.14f, false},
    {"BACK LEFT",                0.24f, -0.14f, -0.23f, true },
    {"BACK RIGHT",              -0.24f, -0.14f, -0.23f, true },
};

std::atomic<int> gPointSlots[POINT_COUNT] = {4, 2, 3, 1, 8, 7, 5};
std::atomic<bool> gGripMarkersEnabled{true};
// Reach for pulling a weapon off a body socket. The old fixed 27cm sphere
// kept catching a holster during ordinary hand movement, so it is smaller by
// default and player-adjustable from the loadout menu.
constexpr int kDefaultGrabRadiusCm = 18;
constexpr int kMinGrabRadiusCm = 8;
constexpr int kMaxGrabRadiusCm = 35;
std::atomic<int> gGrabRadiusCm{kDefaultGrabRadiusCm};
std::atomic<bool> gGripLockEnabled{false};
std::once_flag gInitOnce;
std::mutex gSettingsMutex;
bool gGripDown[2] = {false, false};

std::atomic<bool> gCalibrationPreviewActive{false};
std::mutex gCalibrationPreviewMutex;
int gCalibrationPreviewPoint = -1;
int gCalibrationPreviewSlot = -1;
int gCalibrationPreviewType = 0;
std::atomic<int> gLastActiveSlot{-1};
std::atomic<int> gLastActiveType{0};

bool ValidPoint(int point) { return point >= 0 && point < POINT_COUNT; }
bool SelectableSlot(int slot) { return slot == kEmptySlot || (slot >= 1 && slot <= 7); }
int WeaponTypeInSlot(void* ped, int slot);

// gSettingsMutex must be held. When `slot` is already assigned, exchange the
// point values rather than briefly publishing a duplicate loadout. The old
// owner is cleared before either value moves, so lock-free render/game readers
// can momentarily see a category unassigned but can never see it twice.
void AssignPointSlotLocked(int point, int slot) {
    const int previous = gPointSlots[point].load(std::memory_order_acquire);
    if (previous == slot) return;

    int occupiedPoint = -1;
    if (slot >= 0) {
        for (int other = 0; other < POINT_COUNT; ++other) {
            if (other == point || other == CHEST_CENTER) continue;
            if (gPointSlots[other].load(std::memory_order_acquire) == slot) {
                occupiedPoint = other;
                break;
            }
        }
    }

    if (occupiedPoint >= 0)
        gPointSlots[occupiedPoint].store(kEmptySlot, std::memory_order_release);
    gPointSlots[point].store(slot, std::memory_order_release);
    if (occupiedPoint >= 0)
        gPointSlots[occupiedPoint].store(previous, std::memory_order_release);
}

int DefaultPreviewPoint(int slot) {
    switch (slot) {
        case 1: return CHEST_RIGHT;
        case 2: return WAIST_RIGHT;
        case 3: return CHEST_LEFT;
        case 4: return WAIST_LEFT;
        case 5: return BACK_RIGHT;
        case 6: return BACK_RIGHT;  // sniper is the one category omitted by defaults
        case 7: return BACK_LEFT;
        case 8: return CHEST_CENTER;
        default: return -1;
    }
}

int PreviewPointForSlot(int slot) {
    const int configured = FindPointForSlot(slot);
    if (configured >= 0) return configured;
    // An explicitly EMPTY point is the least surprising temporary preview home.
    // If all points are occupied, override the category's conventional point for
    // the preview only; the persistent loadout is never modified.
    for (int point = 0; point < POINT_COUNT; ++point)
        if (point != CHEST_CENTER && PointSlot(point) == kEmptySlot) return point;
    return DefaultPreviewPoint(slot);
}

bool SetCalibrationPreview(void* ped, int slot) {
    if (ped == nullptr || slot <= 0 || slot > kCenterThrowableSlot) return false;
    const int type = WeaponTypeInSlot(ped, slot);
    const int point = PreviewPointForSlot(slot);
    if (type == 0 || point < 0) return false;

    {
        std::lock_guard<std::mutex> lock(gCalibrationPreviewMutex);
        gCalibrationPreviewPoint = point;
        gCalibrationPreviewSlot = slot;
        gCalibrationPreviewType = type;
    }
    gLastActiveSlot.store(slot, std::memory_order_relaxed);
    gLastActiveType.store(type, std::memory_order_relaxed);
    gCalibrationPreviewActive.store(true, std::memory_order_release);
    LOGI("[holster.calib] preview %s type=%d slot=%d at %s%s",
         calib::WeaponName(type), type, slot, PointName(point),
         FindPointForSlot(slot) == point ? "" : " (temporary point)");
    return true;
}

void ResetRuntimeState() {
    gGripDown[0] = false;
    gGripDown[1] = false;
    xr::SetHolsterMarkers(nullptr, 0);
}

void LoadSettings() {
    int proposed[POINT_COUNT];
    for (int i = 0; i < POINT_COUNT; ++i) proposed[i] = kDefaultSlots[i];
    bool markers = true;
    int grabRadiusCm = kDefaultGrabRadiusCm;
    bool gripLock = false;

    FILE* f = std::fopen(kSettingsPath, "r");
    const bool hadFile = f != nullptr;
    if (f != nullptr) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            int enabled = 1;
            if (std::sscanf(line, "grip_markers %d", &enabled) == 1) {
                markers = enabled != 0;
                continue;
            }
            int radius = 0;
            if (std::sscanf(line, "grab_radius_cm %d", &radius) == 1) {
                grabRadiusCm = std::clamp(radius, kMinGrabRadiusCm,
                                          kMaxGrabRadiusCm);
                continue;
            }
            int lock = 0;
            if (std::sscanf(line, "grip_lock %d", &lock) == 1) {
                gripLock = lock != 0;
                continue;
            }
            int point = -1;
            int slot = kEmptySlot;
            if (std::sscanf(line, "p %d %d", &point, &slot) != 2) continue;
            if (!ValidPoint(point) || point == CHEST_CENTER || !SelectableSlot(slot)) continue;
            proposed[point] = slot;
        }
        std::fclose(f);
    }
    gGrabRadiusCm.store(grabRadiusCm, std::memory_order_release);
    gGripLockEnabled.store(gripLock, std::memory_order_release);

    // Reject duplicate configured slots deterministically. EMPTY may occur more
    // than once. The dedicated throwable point is never configurable.
    bool used[9] = {};
    used[kCenterThrowableSlot] = true;
    proposed[CHEST_CENTER] = kCenterThrowableSlot;
    for (int point = 0; point < POINT_COUNT; ++point) {
        if (point == CHEST_CENTER) continue;
        int slot = proposed[point];
        if (!SelectableSlot(slot) || (slot >= 0 && used[slot])) slot = kEmptySlot;
        proposed[point] = slot;
        if (slot >= 0) used[slot] = true;
    }
    for (int i = 0; i < POINT_COUNT; ++i)
        gPointSlots[i].store(proposed[i], std::memory_order_release);
    gGripMarkersEnabled.store(markers, std::memory_order_release);

    if (hadFile) LOGI("[holster] loaded %s", kSettingsPath);
    else         LOGI("[holster] no loadout file, using Vice City defaults");
}

void SaveSettingsLocked() {
    FILE* f = std::fopen(kSettingsPath, "w");
    if (f == nullptr) {
        LOGW("[holster] cannot write %s", kSettingsPath);
        return;
    }
    std::fputs("# vr_holsters v2  p <point 0..6> <SA weapon slot, -1=empty>\n", f);
    std::fprintf(f, "grip_markers %d\n",
                 gGripMarkersEnabled.load(std::memory_order_acquire) ? 1 : 0);
    std::fprintf(f, "grab_radius_cm %d\n",
                 gGrabRadiusCm.load(std::memory_order_acquire));
    std::fprintf(f, "grip_lock %d\n",
                 gGripLockEnabled.load(std::memory_order_acquire) ? 1 : 0);
    for (int point = 0; point < POINT_COUNT; ++point) {
        const int slot = point == CHEST_CENTER
            ? kCenterThrowableSlot
            : gPointSlots[point].load(std::memory_order_acquire);
        std::fprintf(f, "p %d %d\n", point, slot);
    }
    std::fclose(f);
}

int WeaponTypeInSlot(void* ped, int slot) {
    if (ped == nullptr || slot <= 0 || slot >= kWeaponSlotCount) return 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(ped);
    return *reinterpret_cast<const int32_t*>(bytes + kOffWeapons + slot * kWeaponStride);
}

}  // namespace

void Init() { std::call_once(gInitOnce, LoadSettings); }

int PointCount() { return POINT_COUNT; }

const PointMetadata* Metadata(int point) {
    return ValidPoint(point) ? &kMetadata[point] : nullptr;
}

const char* PointName(int point) {
    const PointMetadata* metadata = Metadata(point);
    return metadata != nullptr ? metadata->name : "UNKNOWN";
}

bool IsPointFixed(int point) { return point == CHEST_CENTER; }

int PointSlot(int point) {
    Init();
    if (!ValidPoint(point)) return kEmptySlot;
    if (point == CHEST_CENTER) return kCenterThrowableSlot;
    return gPointSlots[point].load(std::memory_order_acquire);
}

int FindPointForSlot(int slot) {
    Init();
    if (slot < 0) return -1;
    for (int point = 0; point < POINT_COUNT; ++point)
        if (PointSlot(point) == slot) return point;
    return -1;
}

const char* SlotName(int slot) {
    switch (slot) {
        case -1: return "EMPTY";
        case 1:  return "MELEE";
        case 2:  return "HANDGUN";
        case 3:  return "SHOTGUN";
        case 4:  return "SMG";
        case 5:  return "RIFLE";
        case 6:  return "SNIPER";
        case 7:  return "HEAVY";
        case 8:  return "THROWABLE";
        default: return "UNKNOWN";
    }
}

bool SetPointSlot(int point, int slot) {
    Init();
    if (!ValidPoint(point) || point == CHEST_CENTER || !SelectableSlot(slot)) return false;

    std::lock_guard<std::mutex> lock(gSettingsMutex);
    const int previous = gPointSlots[point].load(std::memory_order_acquire);
    if (previous == slot) return true;

    int displacedPoint = -1;
    if (slot >= 0) {
        for (int other = 0; other < POINT_COUNT; ++other) {
            if (other != point && other != CHEST_CENTER &&
                gPointSlots[other].load(std::memory_order_acquire) == slot) {
                displacedPoint = other;
                break;
            }
        }
    }
    AssignPointSlotLocked(point, slot);
    SaveSettingsLocked();
    if (displacedPoint >= 0) {
        LOGI("[holster] swapped %s=%s with %s=%s",
             PointName(point), SlotName(slot), PointName(displacedPoint), SlotName(previous));
    } else {
        LOGI("[holster] %s -> %s (slot %d)", PointName(point), SlotName(slot), slot);
    }
    return true;
}

bool SwapPointSlots(int firstPoint, int secondPoint) {
    Init();
    if (!ValidPoint(firstPoint) || !ValidPoint(secondPoint) ||
        firstPoint == CHEST_CENTER || secondPoint == CHEST_CENTER) return false;
    if (firstPoint == secondPoint) return true;

    std::lock_guard<std::mutex> lock(gSettingsMutex);
    const int first = gPointSlots[firstPoint].load(std::memory_order_acquire);
    const int second = gPointSlots[secondPoint].load(std::memory_order_acquire);
    if (first == second) return true;
    // Clear one side first so lock-free readers never observe a duplicated
    // category during the exchange.
    gPointSlots[firstPoint].store(kEmptySlot, std::memory_order_release);
    gPointSlots[secondPoint].store(first, std::memory_order_release);
    gPointSlots[firstPoint].store(second, std::memory_order_release);
    SaveSettingsLocked();
    LOGI("[holster] swapped %s=%s with %s=%s",
         PointName(firstPoint), SlotName(second), PointName(secondPoint), SlotName(first));
    return true;
}

int CyclePointSlot(int point, int direction) {
    Init();
    if (!ValidPoint(point)) return kEmptySlot;
    if (point == CHEST_CENTER || direction == 0) return PointSlot(point);

    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (ped == nullptr) return PointSlot(point);

    const int current = gPointSlots[point].load(std::memory_order_acquire);
    const int step = direction < 0 ? -1 : 1;
    // There are seven configurable categories. Start after the current value
    // and stop after one full pass. This keeps EMPTY out of the cycle and also
    // avoids silently stealing/swapping a category assigned to another point.
    const int start = current >= 1 && current <= 7
        ? current
        : (step > 0 ? 0 : 8);
    for (int attempt = 1; attempt <= 7; ++attempt) {
        const int selected = 1 + (start - 1 + step * attempt + 14) % 7;
        if (WeaponTypeInSlot(ped, selected) == 0) continue;
        if (FindPointForSlot(selected) >= 0) continue;
        SetPointSlot(point, selected);
        break;
    }
    return PointSlot(point);
}

bool ClearPointSlot(int point) {
    return SetPointSlot(point, kEmptySlot);
}

bool GripMarkersEnabled() {
    Init();
    return gGripMarkersEnabled.load(std::memory_order_acquire);
}

void SetGripMarkersEnabled(bool enabled) {
    Init();
    if (gGripMarkersEnabled.exchange(enabled, std::memory_order_acq_rel) == enabled) return;
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    SaveSettingsLocked();
    if (!enabled) xr::SetHolsterMarkers(nullptr, 0);
    LOGI("[holster] grip markers %s", enabled ? "shown" : "hidden");
}

void ToggleGripMarkers() { SetGripMarkersEnabled(!GripMarkersEnabled()); }

int GrabRadiusCm() {
    Init();
    return gGrabRadiusCm.load(std::memory_order_acquire);
}

float GrabRadiusMetres() {
    return static_cast<float>(GrabRadiusCm()) * 0.01f;
}

bool GripLockEnabled() {
    Init();
    return gGripLockEnabled.load(std::memory_order_acquire);
}

void ToggleGripLock() {
    Init();
    const bool next = !gGripLockEnabled.load(std::memory_order_acquire);
    gGripLockEnabled.store(next, std::memory_order_release);
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    SaveSettingsLocked();
    LOGI("[holster] grip lock %s", next ? "ON" : "OFF");
}

void AdjustGrabRadiusCm(int direction) {
    if (!direction) return;
    Init();
    const int current = gGrabRadiusCm.load(std::memory_order_acquire);
    const int next = std::clamp(current + (direction < 0 ? -1 : 1),
                                kMinGrabRadiusCm, kMaxGrabRadiusCm);
    if (next == current) return;
    gGrabRadiusCm.store(next, std::memory_order_release);
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    SaveSettingsLocked();
    LOGI("[holster] grab radius %dcm", next);
}

void RememberActiveWeapon(int slot, int weaponType) {
    if (slot <= 0 || slot > kCenterThrowableSlot || weaponType == 0) return;
    gLastActiveSlot.store(slot, std::memory_order_relaxed);
    gLastActiveType.store(weaponType, std::memory_order_relaxed);
}

bool BeginCalibrationPreview() {
    Init();
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (ped != nullptr) {
        // A physical hand selection is more authoritative than CPed's currently
        // active slot, which can remain set after that weapon was put away.
        if (SetCalibrationPreview(ped, gLastActiveSlot.load(std::memory_order_relaxed))) return true;
        const int activeSlot = *reinterpret_cast<const int8_t*>(
            reinterpret_cast<const uint8_t*>(ped) + kOffActiveWeaponSlot);
        if (SetCalibrationPreview(ped, activeSlot)) return true;

        // First entry before Update() has observed a weapon: choose the first
        // owned category, but only after attempting the real active weapon.
        for (int slot = 1; slot <= kCenterThrowableSlot; ++slot)
            if (SetCalibrationPreview(ped, slot)) return true;
    }
    EndCalibrationPreview();
    LOGW("[holster.calib] no owned holster weapon available for preview");
    return false;
}

void EndCalibrationPreview() {
    gCalibrationPreviewActive.store(false, std::memory_order_release);
}

bool CalibrationPreviewActive() {
    return gCalibrationPreviewActive.load(std::memory_order_acquire);
}

bool GetCalibrationPreview(int* point, int* slot, int* weaponType) {
    if (!CalibrationPreviewActive()) return false;
    std::lock_guard<std::mutex> lock(gCalibrationPreviewMutex);
    if (!gCalibrationPreviewActive.load(std::memory_order_relaxed) ||
        gCalibrationPreviewPoint < 0 || gCalibrationPreviewType == 0) return false;
    if (point) *point = gCalibrationPreviewPoint;
    if (slot) *slot = gCalibrationPreviewSlot;
    if (weaponType) *weaponType = gCalibrationPreviewType;
    return true;
}

bool CycleCalibrationPreviewWeapon(int direction) {
    Init();
    void* ped = g.FindPlayerPed ? g.FindPlayerPed(-1) : nullptr;
    if (ped == nullptr) return false;
    int currentSlot = gLastActiveSlot.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(gCalibrationPreviewMutex);
        if (gCalibrationPreviewSlot > 0) currentSlot = gCalibrationPreviewSlot;
    }
    if (currentSlot < 1 || currentSlot > kCenterThrowableSlot) currentSlot = 1;
    const int step = direction < 0 ? -1 : 1;
    for (int attempt = 1; attempt <= kCenterThrowableSlot; ++attempt) {
        const int slot = 1 + (currentSlot - 1 + step * attempt +
                              kCenterThrowableSlot * 2) % kCenterThrowableSlot;
        if (SetCalibrationPreview(ped, slot)) return true;
    }
    return false;
}

void Update() {
    Init();
    if (g.FindPlayerPed == nullptr || g.CPed_SetCurrentWeaponSlot == nullptr) {
        ResetRuntimeState();
        return;
    }
    void* ped = g.FindPlayerPed(-1);
    if (ped == nullptr || (g.FindPlayerVehicle && g.FindPlayerVehicle(-1, false))) {
        ResetRuntimeState();
        return;
    }

    float anchors[POINT_COUNT][3];
    bool available[POINT_COUNT] = {};
    float markers[POINT_COUNT][3];
    int markerCount = 0;
    const int activeSlot = *reinterpret_cast<const int8_t*>(
        reinterpret_cast<const uint8_t*>(ped) + kOffActiveWeaponSlot);
    const int activeType = WeaponTypeInSlot(ped, activeSlot);
    RememberActiveWeapon(activeSlot, activeType);
    for (int point = 0; point < POINT_COUNT; ++point) {
        if (!vrcam::GetHolsterAnchorTracking(point, anchors[point])) {
            ResetRuntimeState();
            return;
        }

        const int slot = PointSlot(point);
        available[point] = slot > 0 && slot != activeSlot && WeaponTypeInSlot(ped, slot) != 0;
        if (available[point]) {
            for (int c = 0; c < 3; ++c) markers[markerCount][c] = anchors[point][c];
            ++markerCount;
        }
    }
    if (GripMarkersEnabled())
        xr::SetHolsterMarkers(markerCount > 0 ? markers : nullptr, markerCount);
    else
        xr::SetHolsterMarkers(nullptr, 0);
    if (markerCount == 0) {
        gGripDown[0] = false;
        gGripDown[1] = false;
        return;
    }

    xr::HandPose hands[2];
    xr::GetHandPoses(hands);
    for (int hand = 0; hand < 2; ++hand) {
        if (!hands[hand].valid) {
            gGripDown[hand] = false;
            continue;
        }
        const float grip = hands[hand].grip;
        if (grip >= 0.65f && !gGripDown[hand]) {
            int bestPoint = -1;
            float bestDistance = 0.24f;
            for (int point = 0; point < POINT_COUNT; ++point) {
                if (!available[point]) continue;
                const float dx = hands[hand].gripPos[0] - anchors[point][0];
                const float dy = hands[hand].gripPos[1] - anchors[point][1];
                const float dz = hands[hand].gripPos[2] - anchors[point][2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestPoint = point;
                }
            }
            if (bestPoint >= 0) {
                const int slot = PointSlot(bestPoint);
                g.CPed_SetCurrentWeaponSlot(ped, slot);
                LOGI("[holster] hand %d grabbed %s: %s (slot %d)",
                     hand, PointName(bestPoint), SlotName(slot), slot);
            }
        }
        if (grip <= 0.30f) gGripDown[hand] = false;
        else if (grip >= 0.65f) gGripDown[hand] = true;
    }
}

}  // namespace savr::holster
