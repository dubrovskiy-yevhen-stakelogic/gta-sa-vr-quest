#include "Calib.h"

#include "Log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <unistd.h>

namespace savr::calib {
namespace {

constexpr int kMaxWeapons = 64;      // SA has ~47 weapon types; round up
constexpr int kHands      = 2;       // 0 = LEFT, 1 = RIGHT
constexpr int kMasterHand = 1;       // RIGHT is canonical; LEFT mirrors it

WeaponCalib      g_calib[kHands][kMaxWeapons];
HolsterCalib     g_holsterCalib[kMaxWeapons];
bool             g_supportConfigured[kMaxWeapons]{};
std::atomic<int> g_active{0};
std::mutex       g_calibMutex;
std::atomic<bool> g_laserEnabled{false};
std::atomic<bool> g_laserLocked[kMaxWeapons]{};
// Per-weapon visible-laser override: 0 = follow the global toggle,
// 1 = always on for this weapon, 2 = always off for this weapon.
std::atomic<int> g_laserMode[kMaxWeapons]{};

const char* kPath = "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_calib.ini";
const char* kTempPath = "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_calib.ini.tmp";

int ClampType(int t) { return (t < 0) ? 0 : (t >= kMaxWeapons ? kMaxWeapons - 1 : t); }

// Field -> member. Order MUST match the Field enum. Returns nullptr for a bad
// index (clamp-to-noop; never silently alias onto another field).
int16_t* FieldPtr(WeaponCalib& c, int f) {
    switch (f) {
        case F_AIM_OX: return &c.aimOffX; case F_AIM_OY: return &c.aimOffY; case F_AIM_OZ: return &c.aimOffZ;
        case F_AIM_RX: return &c.aimRotX; case F_AIM_RY: return &c.aimRotY; case F_AIM_RZ: return &c.aimRotZ;
        case F_WPN_OX: return &c.wpnOffX; case F_WPN_OY: return &c.wpnOffY; case F_WPN_OZ: return &c.wpnOffZ;
        case F_WPN_RX: return &c.wpnRotX; case F_WPN_RY: return &c.wpnRotY; case F_WPN_RZ: return &c.wpnRotZ;
        case F_SUP_OX: return &c.supOffX; case F_SUP_OY: return &c.supOffY; case F_SUP_OZ: return &c.supOffZ;
        case F_SUP_RX: return &c.supRotX; case F_SUP_RY: return &c.supRotY; case F_SUP_RZ: return &c.supRotZ;
        case F_SUP_STYLE: return &c.supStyle;
        default: return nullptr;
    }
}

int16_t* HolsterFieldPtr(HolsterCalib& c, int f) {
    switch (f) {
        case H_OFF_X: return &c.offX; case H_OFF_Y: return &c.offY; case H_OFF_Z: return &c.offZ;
        case H_ROT_X: return &c.rotX; case H_ROT_Y: return &c.rotY; case H_ROT_Z: return &c.rotZ;
        default: return nullptr;
    }
}

const int16_t* HolsterFieldPtr(const HolsterCalib& c, int f) {
    return HolsterFieldPtr(const_cast<HolsterCalib&>(c), f);
}

const char* const kLabels[F_COUNT] = {
    "AIM OFFSET X", "AIM OFFSET Y", "AIM OFFSET Z",
    "AIM ROT X",    "AIM ROT Y",    "AIM ROT Z",
    "WEAPON OFFSET X", "WEAPON OFFSET Y", "WEAPON OFFSET Z",
    "WEAPON ROT X",    "WEAPON ROT Y",    "WEAPON ROT Z",
    "SUPPORT OFFSET X","SUPPORT OFFSET Y","SUPPORT OFFSET Z",
    "SUPPORT ROT X",   "SUPPORT ROT Y",   "SUPPORT ROT Z",
    "SUPPORT STYLE",
};

const char* const kHolsterLabels[H_COUNT] = {
    "OFFSET X", "OFFSET Y", "OFFSET Z",
    "ROTATION X", "ROTATION Y", "ROTATION Z",
};

// true for rotation fields (degrees), false for offset fields (centimetres).
bool IsRot(int f) {
    return (f >= F_AIM_RX && f <= F_AIM_RZ) ||
           (f >= F_WPN_RX && f <= F_WPN_RZ) ||
           (f >= F_SUP_RX && f <= F_SUP_RZ);
}

// Every slot copies into the file in Field-enum order; keep in sync with FieldPtr.
constexpr int kFieldOrder[F_COUNT] = {
    F_AIM_OX, F_AIM_OY, F_AIM_OZ, F_AIM_RX, F_AIM_RY, F_AIM_RZ,
    F_WPN_OX, F_WPN_OY, F_WPN_OZ, F_WPN_RX, F_WPN_RY, F_WPN_RZ,
    F_SUP_OX, F_SUP_OY, F_SUP_OZ, F_SUP_RX, F_SUP_RY, F_SUP_RZ,
    F_SUP_STYLE,
};

struct WeaponDefaultRow {
    int type;
    WeaponCalib calibration;
    bool laserLocked;
    bool supportConfigured;
};

// Author calibration captured from the release Quest on 2026-08-26. Values
// retain the native v6 field order documented in Calib.h.
constexpr WeaponDefaultRow kWeaponDefaults[] = {
    {2,  {0, 0, 0, 0, 0, 1, -16, -4, -10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {3,  {0, 0, 0, 0, 0, 0, 24, -8, -8, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {4,  {0, 0, 0, 0, 0, 0, 2, -8, -10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {5,  {0, 0, 0, 0, 0, 0, -6, -4, -9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {6,  {0, 0, 0, 0, 0, 0, 3, -5, -9, 0, 36, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {7,  {0, 0, 0, 0, 0, 0, 0, -5, -10, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {8,  {0, 0, 0, 0, 0, 0, 15, -7, -9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {9,  {0, 0, 0, 0, 0, 0, 0, -9, -14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {10, {0, 0, 0, 0, 0, 0, -2, -8, -10, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {11, {0, 0, 0, 0, 0, 0, 10, -8, -11, 0, 51, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {12, {0, 0, 0, 0, 0, 0, -11, -9, -11, 0, 37, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {13, {0, 0, 0, 0, 0, 0, 1, -6, -11, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {14, {0, 0, 0, 0, 0, 0, -2, -4, -13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {15, {0, 0, 0, 0, 0, 0, 3, -6, -6, 0, 41, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {16, {0, 0, 0, 0, 0, 0, 2, -2, -14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {17, {0, 0, 0, 0, 0, 0, 0, 0, -15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {18, {0, 0, 0, 0, 0, 0, 28, -6, -12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {22, {17, 6, -12, 7, 20, 0, -3, -3, -8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true, false},
    {23, {21, 7, 0, 6, 18, 0, 0, 0, -10, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true, false},
    {24, {16, 4, 1, -5, 0, 0, 0, -7, -9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {25, {26, 2, 10, -12, 15, 0, 0, -2, -10, 0, 0, -7, -16, 85, -8, 0, -360, -360, 1}, true, true},
    {26, {27, 3, 67, -4, 3, 0, 0, -1, -9, 0, 0, 0, -15, 70, -4, 0, 360, -320, 1}, false, true},
    {27, {28, 5, 0, -13, 17, 0, -1, -1, -10, 0, 0, 0, -15, 74, -9, 0, 360, -298, 1}, false, true},
    {28, {17, 2, -8, -12, 8, 0, 2, 1, -6, 0, 0, 0, 0, 60, -10, 0, 360, 0, 0}, false, true},
    {29, {30, 1, 0, -14, 15, 0, -10, -10, -8, 0, 0, 0, -26, 60, -9, 0, 360, -334, 1}, false, true},
    {30, {21, -2, 39, -10, 6, 0, 0, 0, -9, 0, 0, 0, -11, 79, -9, 0, 360, -329, 1}, false, true},
    {31, {22, 0, 21, -11, 1, 1, 0, 0, -6, 0, 1, 1, -11, 82, -9, -5, 360, 332, 1}, true, true},
    {32, {21, 6, 0, -5, 3, 0, -1, -7, -10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true, false},
    {33, {21, 5, -8, -13, 17, 0, -3, 0, -11, 0, 0, 0, -19, 76, -10, -19, 360, -169, 0}, true, true},
    {34, {21, 3, 0, -11, 11, 0, -2, -3, -9, 0, 0, 0, -16, 60, -10, 0, 360, -337, 1}, false, true},
    {35, {0, 0, 0, 0, 0, 0, -2, 25, -12, 0, 0, 0, 0, 17, -1, 0, 360, 0, 0}, false, true},
    {36, {0, 0, 0, 0, 0, 0, 1, 29, -12, -1, 0, 0, 9, 23, 0, 0, 360, 0, 0}, false, true},
    {37, {0, 0, 0, 0, 0, 0, 11, -5, -12, 0, 65, 0, -22, 58, -10, 0, 360, 0, 0}, false, true},
    {38, {-30, 15, 0, -11, 58, 0, 11, -3, -12, 1, 76, 0, -36, 44, -3, -31, 209, 33, 1}, true, true},
    {39, {0, 0, 0, 0, 0, 0, 45, -20, 0, 0, 0, 0, 0, 60, -10, 0, 360, 0, 0}, false, true},
    {41, {0, 0, 0, 0, 0, 0, 15, 0, -10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {42, {0, 0, 0, 0, 0, 0, 80, 18, -18, -2, 141, -5, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {43, {0, 0, 0, 0, 0, 0, 3, 0, -33, 0, 69, 0, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {44, {0, 0, 0, 0, 0, 0, 0, -3, -13, -171, -3, -134, 0, 0, 0, 0, 0, 0, 0}, false, false},
    {45, {0, 0, 0, 0, 0, 0, -9, 0, -9, -160, 0, -132, 0, 0, 0, 0, 0, 0, 0}, false, false},
};

struct HolsterDefaultRow {
    int type;
    HolsterCalib calibration;
};

constexpr HolsterDefaultRow kHolsterDefaults[] = {
    {5,  {-15, -9, 0, 0, 180, 0}},
    {18, {-14, -13, 18, 0, -180, 0}},
    {22, {-13, -5, 0, 180, 0, 0}},
};

WeaponCalib DefaultWeaponCalibration(int type) {
    for (const WeaponDefaultRow& row : kWeaponDefaults)
        if (row.type == type) return row.calibration;
    return WeaponCalib{};
}

HolsterCalib DefaultHolsterCalibration(int type) {
    for (const HolsterDefaultRow& row : kHolsterDefaults)
        if (row.type == type) return row.calibration;
    return HolsterCalib{};
}

bool DefaultLaserLocked(int type) {
    for (const WeaponDefaultRow& row : kWeaponDefaults)
        if (row.type == type) return row.laserLocked;
    return false;
}

bool DefaultSupportConfigured(int type) {
    for (const WeaponDefaultRow& row : kWeaponDefaults)
        if (row.type == type) return row.supportConfigured;
    return false;
}

bool IsDefault(const WeaponCalib& c, int type) {
    const WeaponCalib d = DefaultWeaponCalibration(type);
    return std::memcmp(&c, &d, sizeof(WeaponCalib)) == 0;
}

void ApplyEffectiveSupportDefaults(WeaponCalib& c, bool configured) {
    // Profiles written before two-hand support contain explicit zeroes in all
    // SUPPORT columns. Present the Vice City Quest port's generic SA-safe socket until the user
    // touches those rows, then seed the same values into storage so the first
    // adjustment cannot make the foregrip jump back to the weapon origin.
    if (configured) return;
    if (c.supOffX == 0 && c.supOffY == 0 && c.supOffZ == 0) {
        c.supOffY = 60;   // 30 cm along the model-bound barrel axis
        c.supOffZ = -10;  // 5 cm below the top axis
    }
    if (c.supRotX == 0 && c.supRotY == 0 && c.supRotZ == 0)
        c.supRotY = 360;  // 180 degrees in half-degree storage units
}

bool IsDefault(const HolsterCalib& c, int type) {
    const HolsterCalib d = DefaultHolsterCalibration(type);
    return std::memcmp(&c, &d, sizeof(HolsterCalib)) == 0;
}

// SA eWeaponType names, for the menu heading. Unknown ids fall back to "WEAPON N".
const char* WeaponNameRaw(int t) {
    switch (t) {
        case 0:  return "UNARMED";      case 1:  return "BRASS KNUCKLE"; case 2:  return "GOLF CLUB";
        case 3:  return "NIGHTSTICK";   case 4:  return "KNIFE";         case 5:  return "BASEBALL BAT";
        case 6:  return "SHOVEL";       case 7:  return "POOL CUE";      case 8:  return "KATANA";
        case 9:  return "CHAINSAW";     case 14: return "FLOWERS";       case 15: return "CANE";
        case 16: return "GRENADE";      case 17: return "TEAR GAS";      case 18: return "MOLOTOV";
        case 22: return "PISTOL";       case 23: return "SILENCED";      case 24: return "DESERT EAGLE";
        case 25: return "SHOTGUN";      case 26: return "SAWNOFF";       case 27: return "SPAS12";
        case 28: return "MICRO UZI";    case 29: return "MP5";           case 30: return "AK47";
        case 31: return "M4";           case 32: return "TEC9";          case 33: return "RIFLE";
        case 34: return "SNIPER";       case 35: return "RPG";           case 36: return "HEATSEEKER";
        case 37: return "FLAMETHROWER"; case 38: return "MINIGUN";       case 39: return "SATCHEL";
        case 40: return "DETONATOR";    case 41: return "SPRAYCAN";      case 42: return "EXTINGUISHER";
        case 43: return "CAMERA";       case 44: return "NIGHTVISION";   case 45: return "THERMAL";
        case 46: return "PARACHUTE";    default: return nullptr;
    }
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> guard(g_calibMutex);
    for (int h = 0; h < kHands; ++h)
        for (int t = 0; t < kMaxWeapons; ++t)
            g_calib[h][t] = DefaultWeaponCalibration(t);
    for (int t = 0; t < kMaxWeapons; ++t)
        g_holsterCalib[t] = DefaultHolsterCalibration(t);
    for (int t = 0; t < kMaxWeapons; ++t) {
        g_laserLocked[t].store(DefaultLaserLocked(t),
                               std::memory_order_relaxed);
        g_supportConfigured[t] = DefaultSupportConfigured(t);
    }
    g_laserEnabled.store(false, std::memory_order_relaxed);

    FILE* f = std::fopen(kPath, "r");
    if (!f) { LOGI("[calib] no profile file, defaults"); return; }
    char line[512];
    int loaded = 0;
    bool seen[kHands][kMaxWeapons]{};
    bool supportSeen[kHands][kMaxWeapons]{};
    bool lockSeen[kMaxWeapons]{};
    bool anyTypedLockSeen = false;
    bool legacyLaserLocked = false;
    while (std::fgets(line, sizeof(line), f)) {
        int state = 0;
        if (std::sscanf(line, "laser_enabled %d", &state) == 1) {
            g_laserEnabled.store(state != 0, std::memory_order_relaxed);
            continue;
        }
        if (std::sscanf(line, "laser_locked %d", &state) == 1) {
            legacyLaserLocked = state != 0;
            continue;
        }
        int modeType = -1;
        if (std::sscanf(line, "laser_mode_type %d %d", &modeType, &state) == 2) {
            if (modeType >= 0 && modeType < kMaxWeapons)
                g_laserMode[modeType].store(std::clamp(state, 0, 2),
                                            std::memory_order_relaxed);
            continue;
        }
        int lockedType = -1;
        if (std::sscanf(line, "laser_locked_type %d %d", &lockedType, &state) == 2) {
            if (lockedType >= 0 && lockedType < kMaxWeapons) {
                g_laserLocked[lockedType].store(state != 0,
                                                std::memory_order_relaxed);
                lockSeen[lockedType] = true;
                anyTypedLockSeen = true;
            }
            continue;
        }
        int supportType = -1;
        if (std::sscanf(line, "support_configured %d %d", &supportType, &state) == 2) {
            if (supportType >= 0 && supportType < kMaxWeapons)
                g_supportConfigured[supportType] = state != 0;
            continue;
        }
        // Native format: one six-value row per weapon type (no hand).  Also
        // accept the PC SA form `holster 0 <type> ...` for easy migration.
        if (std::strncmp(line, "holster ", 8) == 0) {
            int ignoredHand = 0, t = -1, v[H_COUNT]{};
            int got = std::sscanf(line, "holster %d %d %d %d %d %d %d %d",
                                  &ignoredHand, &t, &v[0], &v[1], &v[2],
                                  &v[3], &v[4], &v[5]);
            bool validRow = got == 2 + H_COUNT;
            if (!validRow) {
                got = std::sscanf(line, "holster %d %d %d %d %d %d %d",
                                  &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
                validRow = got == 1 + H_COUNT;
            }
            if (validRow && t >= 0 && t < kMaxWeapons) {
                HolsterCalib& c = g_holsterCalib[t];
                c = HolsterCalib{};
                for (int field = 0; field < H_COUNT; ++field) {
                    int lo, hi; HolsterFieldRange(field, lo, hi);
                    const int value = v[field] < lo ? lo : (v[field] > hi ? hi : v[field]);
                    *HolsterFieldPtr(c, field) = static_cast<int16_t>(value);
                }
                ++loaded;
            }
            continue;
        }
        if (line[0] != 'w') continue;                       // not a profile row
        int t, h, v[F_COUNT];
        const int got = std::sscanf(line,
            "w %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
            &t, &h,
            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
            &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18]);
        if (got != 2 + F_COUNT) continue;                   // 21 = 2 + 19; skip malformed
        if (t < 0 || t >= kMaxWeapons || h < 0 || h >= kHands) continue;
        WeaponCalib& c = g_calib[h][t];
        c = WeaponCalib{};                                  // start from defaults
        for (int i = 0; i < F_COUNT; ++i) {
            int lo, hi; FieldRange(kFieldOrder[i], lo, hi);
            int val = v[i] < lo ? lo : (v[i] > hi ? hi : v[i]);
            if (int16_t* p = FieldPtr(c, kFieldOrder[i])) *p = static_cast<int16_t>(val);
        }
        seen[h][t] = true;
        supportSeen[h][t] = c.supOffX != 0 || c.supOffY != 0 || c.supOffZ != 0 ||
                            c.supRotX != 0 || c.supRotY != 0 || c.supRotZ != 0 ||
                            c.supStyle != SUPPORT_MAGAZINE;
        if (h == kMasterHand && supportSeen[h][t])
            g_supportConfigured[t] = true;
        ++loaded;
    }
    std::fclose(f);

    int migrated = 0;
    for (int t = 0; t < kMaxWeapons; ++t) {
        if (seen[0][t] && !seen[kMasterHand][t]) {
            g_calib[kMasterHand][t] = g_calib[0][t];
            if (supportSeen[0][t]) g_supportConfigured[t] = true;
            ++migrated;
        }
        // v5 stored one global cosmetic flag and could not identify its weapon.
        // Preserve the old SAVED indication for existing profiles; the next AIM
        // edit immediately makes only that weapon dirty under the v6 scheme.
        if (legacyLaserLocked && !anyTypedLockSeen && !lockSeen[t] &&
            (seen[0][t] || seen[kMasterHand][t]))
            g_laserLocked[t].store(true, std::memory_order_relaxed);
    }
    LOGI("[calib] loaded %d weapon profiles, migrated %d left-only profiles",
         loaded, migrated);
}

void Save() {
    std::lock_guard<std::mutex> guard(g_calibMutex);
    // Preserve the last complete profile even if Android pauses or kills the
    // process during a burst of menu edits. Direct fopen("w") used to leave a
    // truncated calibration file, which looked like a laser reset on restart.
    FILE* f = std::fopen(kTempPath, "w");
    if (!f) { LOGW("[calib] cannot write %s", kTempPath); return; }
    std::fputs("# vr_calib v6  w <type> <hand> <19 fields>; laser_locked_type <type> 1; support_configured <type> 1; holster <type> <6 fields>\n", f);
    std::fprintf(f, "laser_enabled %d\n", LaserEnabled() ? 1 : 0);
    // Keep the old line for downgrade compatibility; v6 readers use the
    // per-weapon rows below.
    std::fprintf(f, "laser_locked %d\n", LaserLocked() ? 1 : 0);
    int saved = 0;
    for (int t = 0; t < kMaxWeapons; ++t) {
        const int laserMode = g_laserMode[t].load(std::memory_order_relaxed);
        if (laserMode != 0)
            std::fprintf(f, "laser_mode_type %d %d\n", t, laserMode);
        const bool laserLocked =
            g_laserLocked[t].load(std::memory_order_relaxed);
        if (laserLocked || DefaultLaserLocked(t))
            std::fprintf(f, "laser_locked_type %d %d\n", t,
                         laserLocked ? 1 : 0);
        // AIM edits are live and AdjustField already persists every real
        // change.  Never replace them with the previous committed snapshot
        // merely because the optional SAVE LASER action was missed: on Quest
        // that made a perfectly calibrated ray jump back after every restart.
        // The per-type lock remains a useful UI confirmation marker, not a
        // second persistence gate.
        WeaponCalib c = g_calib[kMasterHand][t];
        if (g_supportConfigured[t])
            std::fprintf(f, "support_configured %d 1\n", t);
        if (IsDefault(c, t)) continue;                       // only overrides
        std::fprintf(f, "w %d %d", t, kMasterHand);
        for (int i = 0; i < F_COUNT; ++i)
            std::fprintf(f, " %d", static_cast<int>(*FieldPtr(c, kFieldOrder[i])));
        std::fputc('\n', f);
        ++saved;
    }
    int holsterSaved = 0;
    for (int t = 0; t < kMaxWeapons; ++t) {
        const HolsterCalib& c = g_holsterCalib[t];
        if (IsDefault(c, t)) continue;
        std::fprintf(f, "holster %d", t);
        for (int field = 0; field < H_COUNT; ++field)
            std::fprintf(f, " %d", static_cast<int>(*HolsterFieldPtr(c, field)));
        std::fputc('\n', f);
        ++holsterSaved;
    }
    bool writeOk = std::fflush(f) == 0;
    if (writeOk) writeOk = ::fsync(::fileno(f)) == 0;
    if (std::fclose(f) != 0) writeOk = false;
    if (!writeOk || std::rename(kTempPath, kPath) != 0) {
        const int error = errno;
        std::remove(kTempPath);
        LOGW("[calib] atomic save failed for %s (errno=%d)", kPath, error);
        return;
    }
    LOGI("[calib] saved %d master weapon profiles, %d holster profiles, laser=%d locked=%d",
         saved, holsterSaved, LaserEnabled() ? 1 : 0, LaserLocked() ? 1 : 0);
}

void SetActiveWeapon(int weaponType) { g_active.store(ClampType(weaponType), std::memory_order_relaxed); }
int  ActiveWeapon() { return g_active.load(std::memory_order_relaxed); }

WeaponCalib Snapshot(int /*hand*/, int type) {
    std::lock_guard<std::mutex> guard(g_calibMutex);
    WeaponCalib effective = g_calib[kMasterHand][ClampType(type)];
    ApplyEffectiveSupportDefaults(effective, g_supportConfigured[ClampType(type)]);
    return effective;
}

HolsterCalib SnapshotHolster(int type) {
    std::lock_guard<std::mutex> guard(g_calibMutex);
    return g_holsterCalib[ClampType(type)];
}

void HolsterFieldRange(int field, int& lo, int& hi) {
    if (field >= H_ROT_X && field <= H_ROT_Z) { lo = -360; hi = 360; return; }
    lo = -100;
    hi = 100;
}

int16_t GetHolsterField(int type, int field) {
    std::lock_guard<std::mutex> guard(g_calibMutex);
    const int16_t* value = HolsterFieldPtr(g_holsterCalib[ClampType(type)], field);
    return value ? *value : 0;
}

void SetHolsterField(int type, int field, int value) {
    std::lock_guard<std::mutex> guard(g_calibMutex);
    int16_t* target = HolsterFieldPtr(g_holsterCalib[ClampType(type)], field);
    if (!target) return;
    int lo, hi; HolsterFieldRange(field, lo, hi);
    *target = static_cast<int16_t>(value < lo ? lo : (value > hi ? hi : value));
}

void AdjustHolsterField(int type, int field, int steps) {
    {
        std::lock_guard<std::mutex> guard(g_calibMutex);
        int16_t* target = HolsterFieldPtr(g_holsterCalib[ClampType(type)], field);
        if (target) {
            int lo, hi; HolsterFieldRange(field, lo, hi);
            const int value = static_cast<int>(*target) + steps;
            *target = static_cast<int16_t>(value < lo ? lo : (value > hi ? hi : value));
        }
    }
    Save();
}

float HolsterDisplayValue(int16_t raw) { return raw * 0.5f; }

const char* HolsterFieldLabel(int field) {
    return (field < 0 || field >= H_COUNT) ? "" : kHolsterLabels[field];
}

const char* HolsterFieldUnit(int field) {
    if (field < 0 || field >= H_COUNT) return "";
    return field >= H_ROT_X ? "DEG" : "CM";
}

bool LaserEnabled() { return g_laserEnabled.load(std::memory_order_relaxed); }

void SetLaserEnabled(bool enabled) {
    if (g_laserEnabled.exchange(enabled, std::memory_order_relaxed) != enabled)
        Save();
}

void ToggleLaser() { SetLaserEnabled(!LaserEnabled()); }

bool LaserLocked(int weaponType) {
    return g_laserLocked[ClampType(weaponType)].load(
        std::memory_order_relaxed);
}

bool LaserLocked() { return LaserLocked(ActiveWeapon()); }

void MarkLaserDirty() {
    g_laserLocked[ClampType(ActiveWeapon())].store(false,
                                                   std::memory_order_relaxed);
}

void LockLaser(int weaponType) {
    const int type = ClampType(weaponType);
    g_laserLocked[type].store(true, std::memory_order_relaxed);
    Save();
}

int LaserModeForWeapon(int weaponType) {
    return std::clamp(
        g_laserMode[ClampType(weaponType)].load(std::memory_order_relaxed),
        0, 2);
}

const char* LaserModeName(int weaponType) {
    switch (LaserModeForWeapon(weaponType)) {
        case 1: return "ON";
        case 2: return "OFF";
        default: return "GLOBAL";
    }
}

void CycleLaserModeForWeapon(int weaponType, int direction) {
    if (!direction) direction = 1;
    const int type = ClampType(weaponType);
    const int next = (LaserModeForWeapon(type) + 3 +
                      (direction < 0 ? -1 : 1)) % 3;
    g_laserMode[type].store(next, std::memory_order_relaxed);
    Save();
    LOGI("[calib] laser mode type=%d -> %s", type, LaserModeName(type));
}

bool LaserVisibleForWeapon(int weaponType) {
    switch (LaserModeForWeapon(weaponType)) {
        case 1: return true;
        case 2: return false;
        default: return LaserEnabled();
    }
}

void LockLaser() { LockLaser(ActiveWeapon()); }

int FieldCount() { return F_COUNT; }

void FieldRange(int field, int& lo, int& hi) {
    if (field == F_SUP_STYLE)            { lo = 0;    hi = 1;   return; }
    if (field >= F_SUP_OX && field <= F_SUP_OZ) { lo = -200; hi = 200; return; }  // support offsets
    if (IsRot(field))                    { lo = -360; hi = 360; return; }          // any rotation
    lo = -100; hi = 100;                                                           // aim/weapon offsets
}

int16_t GetField(int hand, int type, int field) {
    (void)hand;
    std::lock_guard<std::mutex> guard(g_calibMutex);
    WeaponCalib effective = g_calib[kMasterHand][ClampType(type)];
    ApplyEffectiveSupportDefaults(effective, g_supportConfigured[ClampType(type)]);
    int16_t* p = FieldPtr(effective, field);
    return p ? *p : 0;
}

void SetField(int hand, int type, int field, int value) {
    (void)hand;
    std::lock_guard<std::mutex> guard(g_calibMutex);
    WeaponCalib& profile = g_calib[kMasterHand][ClampType(type)];
    if (field >= F_SUP_OX && field <= F_SUP_STYLE) {
        ApplyEffectiveSupportDefaults(profile, g_supportConfigured[ClampType(type)]);
        g_supportConfigured[ClampType(type)] = true;
    }
    int16_t* p = FieldPtr(profile, field);
    if (!p) return;
    int lo, hi; FieldRange(field, lo, hi);
    const int16_t clamped = static_cast<int16_t>(value < lo ? lo : (value > hi ? hi : value));
    if (*p == clamped) return;
    *p = clamped;
    if (field >= F_AIM_OX && field <= F_AIM_RZ)
        g_laserLocked[ClampType(type)].store(false, std::memory_order_relaxed);
}

void AdjustField(int hand, int type, int field, int steps) {
    (void)hand;
    bool changed = false;
    {
        std::lock_guard<std::mutex> guard(g_calibMutex);
        const int clampedType = ClampType(type);
        WeaponCalib& profile = g_calib[kMasterHand][clampedType];
        if (field >= F_SUP_OX && field <= F_SUP_STYLE) {
            ApplyEffectiveSupportDefaults(profile, g_supportConfigured[clampedType]);
            g_supportConfigured[clampedType] = true;
        }
        int16_t* p = FieldPtr(profile, field);
        if (p) {
            int lo, hi; FieldRange(field, lo, hi);
            const int value = static_cast<int>(*p) + steps;
            const int16_t clamped = static_cast<int16_t>(
                value < lo ? lo : (value > hi ? hi : value));
            if (*p != clamped) {
                *p = clamped;
                changed = true;
                if (field >= F_AIM_OX && field <= F_AIM_RZ)
                    g_laserLocked[clampedType].store(false,
                                                     std::memory_order_relaxed);
            }
        }
    }
    if (changed) Save();   // Vice City persists on every real adjustment
}

float DisplayValue(int /*field*/, int16_t raw) { return raw * 0.5f; }

const char* FieldLabel(int field) {
    return (field < 0 || field >= F_COUNT) ? "" : kLabels[field];
}
const char* FieldUnit(int field) {
    if (field < 0 || field >= F_COUNT || field == F_SUP_STYLE) return "";
    return IsRot(field) ? "DEG" : "CM";
}
const char* StyleName(int16_t style) { return style ? "FROM BELOW" : "MAGAZINE"; }

void ResetActive(int hand) {
    (void)hand;
    std::lock_guard<std::mutex> guard(g_calibMutex);
    const int type = ClampType(g_active.load(std::memory_order_relaxed));
    WeaponCalib& c = g_calib[kMasterHand][type];
    if (c.aimOffX || c.aimOffY || c.aimOffZ || c.aimRotX || c.aimRotY || c.aimRotZ)
        g_laserLocked[type].store(false, std::memory_order_relaxed);
    c = DefaultWeaponCalibration(type);
    g_laserLocked[type].store(DefaultLaserLocked(type),
                              std::memory_order_relaxed);
    g_supportConfigured[type] = DefaultSupportConfigured(type);
}

const char* WeaponName(int type) {
    const char* n = WeaponNameRaw(ClampType(type));
    if (n) return n;
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "WEAPON %d", ClampType(type));
    return buf;
}

}  // namespace savr::calib
