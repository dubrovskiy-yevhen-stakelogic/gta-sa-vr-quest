#include "Cheats.h"

#include "Holster.h"
#include "Log.h"
#include "PhysicalWeapon.h"
#include "Symbols.h"

#include <algorithm>
#include <atomic>
#include <algorithm>
#include <dlfcn.h>
#include <cstdio>
#include <ctime>
#include <iterator>

namespace savr::cheats {
namespace {

// Every symbol below is present in the checked-in 2.11 arm64 export table
// (recon/gtasa211-dynsym.txt). Calling the game's handlers retains its own model
// streaming, wanted, weather and ped logic.
enum class Kind : unsigned char { Plain, Vehicle, GodMode, UnlockCities, Reset,
                                  MissionSkip, TutorialSkip };

struct Entry {
    const char* label;
    const char* sym;
    int         vehicleModel;
    Kind        kind;
    void*       fn;
};

constexpr const char* kVehicleCheat = "_ZN6CCheat12VehicleCheatEi";

// Match the Quest Vice City layout: persistent/toggle-style utility first,
// vehicle spawns next, then the native game cheat catalogue. Debug mission-skip
// handlers are deliberately excluded because their preconditions are not safe
// to infer from the retail mobile binary.
Entry g_cheats[] = {
    {"GOD MODE",               "_ZN6CCheat25TogglePlayerInvincibilityEv", -1, Kind::GodMode, nullptr},

    {"SPAWN RHINO TANK",       kVehicleCheat, 432, Kind::Vehicle, nullptr},
    {"SPAWN HYDRA JET",        kVehicleCheat, 520, Kind::Vehicle, nullptr},
    {"SPAWN HUNTER HELI",      kVehicleCheat, 425, Kind::Vehicle, nullptr},
    {"SPAWN MONSTER TRUCK",    kVehicleCheat, 444, Kind::Vehicle, nullptr},
    {"SPAWN NRG 500",          kVehicleCheat, 522, Kind::Vehicle, nullptr},
    {"SPAWN SANCHEZ",          kVehicleCheat, 468, Kind::Vehicle, nullptr},
    {"SPAWN INFERNUS",         kVehicleCheat, 411, Kind::Vehicle, nullptr},
    {"SPAWN TURISMO",          kVehicleCheat, 451, Kind::Vehicle, nullptr},
    {"SPAWN BULLET",           kVehicleCheat, 541, Kind::Vehicle, nullptr},
    {"SPAWN QUAD BIKE",        kVehicleCheat, 471, Kind::Vehicle, nullptr},
    {"SPAWN DOZER",            kVehicleCheat, 486, Kind::Vehicle, nullptr},
    {"SPAWN TANKER",           kVehicleCheat, 514, Kind::Vehicle, nullptr},
    {"SPAWN STUNT PLANE",      kVehicleCheat, 513, Kind::Vehicle, nullptr},
    {"SPAWN VORTEX",           kVehicleCheat, 539, Kind::Vehicle, nullptr},
    {"SPAWN MAVERICK",         kVehicleCheat, 487, Kind::Vehicle, nullptr},
    {"SPAWN SHAMAL",           kVehicleCheat, 519, Kind::Vehicle, nullptr},
    {"SPAWN JETMAX",           kVehicleCheat, 493, Kind::Vehicle, nullptr},

    {"MONEY ARMOUR HEALTH",    "_ZN6CCheat22MoneyArmourHealthCheatEv", -1, Kind::Plain, nullptr},
    {"RESTORE HEALTH",         "_ZN6CCheat11HealthCheatEv",            -1, Kind::Plain, nullptr},
    {"WEAPONS SET ONE",        "_ZN6CCheat12WeaponCheat1Ev",           -1, Kind::Plain, nullptr},
    {"WEAPONS SET TWO",        "_ZN6CCheat12WeaponCheat2Ev",           -1, Kind::Plain, nullptr},
    {"WEAPONS SET THREE",      "_ZN6CCheat12WeaponCheat3Ev",           -1, Kind::Plain, nullptr},
    {"WEAPONS SET FOUR",       "_ZN6CCheat12WeaponCheat4Ev",           -1, Kind::Plain, nullptr},
    {"HITMAN WEAPON SKILLS",   "_ZN6CCheat17WeaponSkillsCheatEv",      -1, Kind::Plain, nullptr},
    {"MAX VEHICLE SKILLS",     "_ZN6CCheat18VehicleSkillsCheatEv",     -1, Kind::Plain, nullptr},
    {"MAX STAMINA",            "_ZN6CCheat12StaminaCheatEv",           -1, Kind::Plain, nullptr},
    {"MAX MUSCLE",             "_ZN6CCheat11MuscleCheatEv",            -1, Kind::Plain, nullptr},
    {"MAX FAT",                "_ZN6CCheat8FatCheatEv",                -1, Kind::Plain, nullptr},
    {"SKINNY",                 "_ZN6CCheat11SkinnyCheatEv",            -1, Kind::Plain, nullptr},
    {"ADRENALINE",             "_ZN6CCheat15AdrenalineCheatEv",        -1, Kind::Plain, nullptr},
    {"JETPACK",                "_ZN6CCheat12JetpackCheatEv",           -1, Kind::Plain, nullptr},
    {"PARACHUTE",              "_ZN6CCheat14ParachuteCheatEv",         -1, Kind::Plain, nullptr},

    {"RAISE WANTED LEVEL",     "_ZN6CCheat18WantedLevelUpCheatEv",     -1, Kind::Plain, nullptr},
    {"LOWER WANTED LEVEL",     "_ZN6CCheat20WantedLevelDownCheatEv",   -1, Kind::Plain, nullptr},
    {"CLEAR WANTED LEVEL",     "_ZN6CCheat14NotWantedCheatEv",         -1, Kind::Plain, nullptr},
    {"SIX STAR WANTED",        "_ZN6CCheat11WantedCheatEv",            -1, Kind::Plain, nullptr},

    {"EXTRA SUNNY WEATHER",    "_ZN6CCheat22ExtraSunnyWeatherCheatEv", -1, Kind::Plain, nullptr},
    {"SUNNY WEATHER",          "_ZN6CCheat17SunnyWeatherCheatEv",      -1, Kind::Plain, nullptr},
    {"CLOUDY WEATHER",         "_ZN6CCheat18CloudyWeatherCheatEv",     -1, Kind::Plain, nullptr},
    {"RAINY WEATHER",          "_ZN6CCheat17RainyWeatherCheatEv",      -1, Kind::Plain, nullptr},
    {"FOGGY WEATHER",          "_ZN6CCheat17FoggyWeatherCheatEv",      -1, Kind::Plain, nullptr},
    {"STORMY WEATHER",         "_ZN6CCheat10StormCheatEv",             -1, Kind::Plain, nullptr},
    {"SANDSTORM",              "_ZN6CCheat14SandstormCheatEv",         -1, Kind::Plain, nullptr},
    {"MIDNIGHT",               "_ZN6CCheat13MidnightCheatEv",          -1, Kind::Plain, nullptr},
    {"DUSK",                   "_ZN6CCheat9DuskCheatEv",               -1, Kind::Plain, nullptr},
    {"FAST GAME",              "_ZN6CCheat13FastTimeCheatEv",          -1, Kind::Plain, nullptr},
    {"SLOW GAME",              "_ZN6CCheat13SlowTimeCheatEv",          -1, Kind::Plain, nullptr},

    {"PEDESTRIAN RIOT",        "_ZN6CCheat9RiotCheatEv",               -1, Kind::Plain, nullptr},
    {"PEDESTRIAN MAYHEM",      "_ZN6CCheat11MayhemCheatEv",            -1, Kind::Plain, nullptr},
    {"EVERYONE ATTACKS CJ",    "_ZN6CCheat27EverybodyAttacksPlayerCheatEv", -1, Kind::Plain, nullptr},
    {"GANGS CONTROL STREETS",  "_ZN6CCheat10GangsCheatEv",             -1, Kind::Plain, nullptr},
    {"GANG MEMBERS EVERYWHERE", "_ZN6CCheat13GangLandCheatEv",         -1, Kind::Plain, nullptr},
    {"NINJA THEME",            "_ZN6CCheat10NinjaCheatEv",             -1, Kind::Plain, nullptr},
    {"BEACH PARTY",            "_ZN6CCheat15BeachPartyCheatEv",        -1, Kind::Plain, nullptr},
    {"ELVIS EVERYWHERE",       "_ZN6CCheat15ElvisLivesCheatEv",        -1, Kind::Plain, nullptr},
    {"VILLAGE PEOPLE",         "_ZN6CCheat18VillagePeopleCheatEv",     -1, Kind::Plain, nullptr},
    {"COUNTRYSIDE INVASION",   "_ZN6CCheat24CountrysideInvasionCheatEv", -1, Kind::Plain, nullptr},
    {"FUNHOUSE THEME",         "_ZN6CCheat13FunhouseCheatEv",          -1, Kind::Plain, nullptr},
    {"LOVE CONQUERS ALL",      "_ZN6CCheat20LoveConquersAllCheatEv",   -1, Kind::Plain, nullptr},
    {"GAMBLER",                "_ZN6CCheat15TheGamblerCheatEv",        -1, Kind::Plain, nullptr},

    {"BLOW UP ALL CARS",       "_ZN6CCheat15BlowUpCarsCheatEv",        -1, Kind::Plain, nullptr},
    {"PERFECT HANDLING",       "_ZN6CCheat20AllCarsAreGreatCheatEv",   -1, Kind::Plain, nullptr},
    {"CHEAP TRAFFIC",          "_ZN6CCheat19AllCarsAreShitCheatEv",    -1, Kind::Plain, nullptr},
    {"FLYING CARS",            "_ZN6CCheat11FlyboyCheatEv",            -1, Kind::Plain, nullptr},
    {"DRIVE BY AIM",           "_ZN6CCheat12DrivebyCheatEv",           -1, Kind::Plain, nullptr},
    {"BLACK TRAFFIC",          "_ZN6CCheat14BlackCarsCheatEv",         -1, Kind::Plain, nullptr},
    {"PINK TRAFFIC",           "_ZN6CCheat13PinkCarsCheatEv",          -1, Kind::Plain, nullptr},
    {"UNLOCK ALL CITIES",      "_ZN6CStats12SetStatValueEtf",          -1, Kind::UnlockCities, nullptr},
    {"SUICIDE",                "_ZN6CCheat12SuicideCheatEv",           -1, Kind::Plain, nullptr},
    {"RESET ALL CHEATS",       "_ZN6CCheat11ResetCheatsEv",            -1, Kind::Reset, nullptr},


    // Manual 5-second window that auto-passes "press key to continue" script
    // waits (CCheat debug leftover). Deliberately NOT automatic: always-on it
    // skipped real story sequences.
    {"SKIP SCRIPT PROMPT (5S)", "_ZN6CCheat17ScriptBypassCheatEv",     -1, Kind::Plain, nullptr},
    {"SKIP MISSION (BETA)",    "DoMissionSkip",                        -1, Kind::MissionSkip, nullptr},
    {"SKIP TUTORIAL PROMPT",   nullptr,                                -1, Kind::TutorialSkip, nullptr},

};

std::atomic<bool> g_godMode{false};
bool* g_nativeInvincible = nullptr;
bool* g_playerIsOffTheMap = nullptr;
float (*g_getStatValue)(unsigned short) = nullptr;
void (*g_setStatValue)(unsigned short, float) = nullptr;
bool (*g_isPlayerOnAMission)() = nullptr;
void (*g_scriptBypassCheat)() = nullptr;
// The city-unlock stat can be rewritten by the mission script as the story
// state machine runs, so a single write silently reverted. While the cheat is
// latched, the per-frame tick re-asserts it.
std::atomic<bool> g_unlockCitiesActive{false};

struct VehicleChoice { const char* name; int model; };
struct VehicleSelector {
    const char* label;
    const VehicleChoice* choices;
    int count;
    std::atomic<int> selected{0};
};

constexpr VehicleChoice kCars[]={{"INFERNUS",411},{"TURISMO",451},
    {"BULLET",541},{"MONSTER",444},{"RHINO",432},{"DOZER",486},
    {"TANKER",514},{"QUAD",471}};
constexpr VehicleChoice kMotorcycles[]={{"NRG-500",522},{"SANCHEZ",468},
    {"FCR-900",521},{"PCJ-600",461},{"FREEWAY",463},{"FAGGIO",462},
    {"BF-400",581},{"WAYFARER",586}};
constexpr VehicleChoice kBicycles[]={{"BMX",481},{"BIKE",509},{"MTB",510}};
constexpr VehicleChoice kHelicopters[]={{"HUNTER",425},{"MAVERICK",487},
    {"CARGOBOB",548},{"LEVIATHAN",417},{"RAINDANCE",563},
    {"SEASPARROW",447},{"SPARROW",469}};
constexpr VehicleChoice kAirplanes[]={{"HYDRA",520},{"SHAMAL",519},
    {"STUNTPLANE",513},{"ANDROMADA",592},{"DODO",593},{"NEVADA",553},
    {"AT-400",577}};
constexpr VehicleChoice kBoats[]={{"JETMAX",493},{"VORTEX",539},
    {"SPEEDER",452},{"SQUALO",446},{"TROPIC",454},{"REEFER",453},
    {"COASTGUARD",472}};

// The complete drivable catalogue (every SA vehicle model 400..611) so any
// vehicle can be spawned for wheel/HUD calibration. Trains, the tram and
// unpowered trailers are omitted: CCheat::VehicleCheat spawns them detached
// from their rail/tractor logic. Names follow the stock model names.
constexpr VehicleChoice kAllVehicles[]={
    {"LANDSTALKER",400},{"BRAVURA",401},{"BUFFALO",402},{"LINERUNNER",403},
    {"PERENNIAL",404},{"SENTINEL",405},{"DUMPER",406},{"FIRETRUCK",407},
    {"TRASHMASTER",408},{"STRETCH",409},{"MANANA",410},{"INFERNUS",411},
    {"VOODOO",412},{"PONY",413},{"MULE",414},{"CHEETAH",415},
    {"AMBULANCE",416},{"LEVIATHAN",417},{"MOONBEAM",418},{"ESPERANTO",419},
    {"TAXI",420},{"WASHINGTON",421},{"BOBCAT",422},{"MR WHOOPEE",423},
    {"BF INJECTION",424},{"HUNTER",425},{"PREMIER",426},{"ENFORCER",427},
    {"SECURICAR",428},{"BANSHEE",429},{"PREDATOR",430},{"BUS",431},
    {"RHINO",432},{"BARRACKS",433},{"HOTKNIFE",434},{"PREVION",436},
    {"COACH",437},{"CABBIE",438},{"STALLION",439},{"RUMPO",440},
    {"RC BANDIT",441},{"ROMERO",442},{"PACKER",443},{"MONSTER",444},
    {"ADMIRAL",445},{"SQUALO",446},{"SEASPARROW",447},{"PIZZABOY",448},
    {"TURISMO",451},{"SPEEDER",452},{"REEFER",453},{"TROPIC",454},
    {"FLATBED",455},{"YANKEE",456},{"CADDY",457},{"SOLAIR",458},
    {"TOPFUN",459},{"SKIMMER",460},{"PCJ-600",461},{"FAGGIO",462},
    {"FREEWAY",463},{"RC BARON",464},{"RC RAIDER",465},{"GLENDALE",466},
    {"OCEANIC",467},{"SANCHEZ",468},{"SPARROW",469},{"PATRIOT",470},
    {"QUAD",471},{"COASTGUARD",472},{"DINGHY",473},{"HERMES",474},
    {"SABRE",475},{"RUSTLER",476},{"ZR-350",477},{"WALTON",478},
    {"REGINA",479},{"COMET",480},{"BMX",481},{"BURRITO",482},
    {"CAMPER",483},{"MARQUIS",484},{"BAGGAGE",485},{"DOZER",486},
    {"MAVERICK",487},{"NEWS MAVERICK",488},{"RANCHER",489},
    {"FBI RANCHER",490},{"VIRGO",491},{"GREENWOOD",492},{"JETMAX",493},
    {"HOTRING",494},{"SANDKING",495},{"BLISTA COMPACT",496},
    {"POLICE MAVERICK",497},{"BOXVILLE",498},{"BENSON",499},{"MESA",500},
    {"RC GOBLIN",501},{"HOTRING A",502},{"HOTRING B",503},{"BLOODRING",504},
    {"RANCHER LURE",505},{"SUPER GT",506},{"ELEGANT",507},{"JOURNEY",508},
    {"BIKE",509},{"MOUNTAIN BIKE",510},{"BEAGLE",511},{"CROPDUSTER",512},
    {"STUNTPLANE",513},{"TANKER",514},{"ROADTRAIN",515},{"NEBULA",516},
    {"MAJESTIC",517},{"BUCCANEER",518},{"SHAMAL",519},{"HYDRA",520},
    {"FCR-900",521},{"NRG-500",522},{"HPV1000",523},{"CEMENT TRUCK",524},
    {"TOWTRUCK",525},{"FORTUNE",526},{"CADRONA",527},{"FBI TRUCK",528},
    {"WILLARD",529},{"FORKLIFT",530},{"TRACTOR",531},{"COMBINE",532},
    {"FELTZER",533},{"REMINGTON",534},{"SLAMVAN",535},{"BLADE",536},
    {"VORTEX",539},{"VINCENT",540},{"BULLET",541},{"CLOVER",542},
    {"SADLER",543},{"FIRETRUCK LA",544},{"HUSTLER",545},{"INTRUDER",546},
    {"PRIMO",547},{"CARGOBOB",548},{"TAMPA",549},{"SUNRISE",550},
    {"MERIT",551},{"UTILITY VAN",552},{"NEVADA",553},{"YOSEMITE",554},
    {"WINDSOR",555},{"MONSTER A",556},{"MONSTER B",557},{"URANUS",558},
    {"JESTER",559},{"SULTAN",560},{"STRATUM",561},{"ELEGY",562},
    {"RAINDANCE",563},{"RC TIGER",564},{"FLASH",565},{"TAHOMA",566},
    {"SAVANNA",567},{"BANDITO",568},{"KART",571},{"MOWER",572},
    {"DUNERIDE",573},{"SWEEPER",574},{"BROADWAY",575},{"TORNADO",576},
    {"AT-400",577},{"DFT-30",578},{"HUNTLEY",579},{"STAFFORD",580},
    {"BF-400",581},{"NEWSVAN",582},{"TUG",583},{"EMPEROR",585},
    {"WAYFARER",586},{"EUROS",587},{"HOTDOG",588},{"CLUB",589},
    {"ANDROMADA",592},{"DODO",593},{"RC CAM",594},{"LAUNCH",595},
    {"POLICE LS",596},{"POLICE SF",597},{"POLICE LV",598},
    {"POLICE RANGER",599},{"PICADOR",600},{"SWAT TANK",601},{"ALPHA",602},
    {"PHOENIX",603},{"GLENDALE DMG",604},{"SADLER DMG",605},
    {"BOXBURG",609}};

VehicleSelector g_vehicleSelectors[]={
    {"ANY VEHICLE",kAllVehicles,static_cast<int>(std::size(kAllVehicles))},
    {"CARS",kCars,static_cast<int>(std::size(kCars))},
    {"MOTORCYCLES",kMotorcycles,static_cast<int>(std::size(kMotorcycles))},
    {"BICYCLES",kBicycles,static_cast<int>(std::size(kBicycles))},
    {"HELICOPTERS",kHelicopters,static_cast<int>(std::size(kHelicopters))},
    {"AIRPLANES",kAirplanes,static_cast<int>(std::size(kAirplanes))},
    {"BOATS",kBoats,static_cast<int>(std::size(kBoats))}
};
constexpr int kVehicleSelectorCount=static_cast<int>(std::size(g_vehicleSelectors));

// Item giver: ONE selector row cycling a flat list of every carryable
// weapon/item. Activating gives the item and puts it straight into the RIGHT
// tracked hand (physicalweapon::ForceHold) — with the holster-menu GRIP LOCK
// enabled it stays in the hand regardless of grip, which makes calibrating
// items that have no body point (gadgets, gifts) possible in one place.
struct WeaponChoice { const char* name; int type; };

constexpr WeaponChoice kGiveItems[]={
    {"BRASS KNUCKLE",1},{"GOLF CLUB",2},{"NIGHTSTICK",3},{"KNIFE",4},
    {"BASEBALL BAT",5},{"SHOVEL",6},{"POOL CUE",7},{"KATANA",8},
    {"CHAINSAW",9},
    {"PISTOL",22},{"SILENCED",23},{"DESERT EAGLE",24},
    {"SHOTGUN",25},{"SAWNOFF",26},{"SPAS12",27},
    {"MICRO UZI",28},{"MP5",29},{"TEC9",32},
    {"AK47",30},{"M4",31},{"RIFLE",33},{"SNIPER",34},
    {"RPG",35},{"HEATSEEKER",36},{"FLAMETHROWER",37},{"MINIGUN",38},
    {"GRENADE",16},{"TEAR GAS",17},{"MOLOTOV",18},{"SATCHEL",39},
    {"SPRAYCAN",41},{"EXTINGUISHER",42},{"CAMERA",43},
    {"NIGHTVISION",44},{"THERMAL",45},{"PARACHUTE",46},
    {"FLOWERS",14},{"CANE",15},{"DILDO",10},{"WHITE DILDO",11},
    {"VIBRATOR",12},{"SILVER VIBRATOR",13},
};
constexpr int kGiveItemCount=static_cast<int>(std::size(kGiveItems));
std::atomic<int> g_giveItemSelected{0};
constexpr int kWeaponSelectorCount=1; // the GIVE ITEM row

// SA weapon-slot layout (eWeaponType -> ped weapon slot), the same slots the
// physical holster sockets are configured with.
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

const WeaponChoice& SelectedGiveItem() {
    return kGiveItems[std::clamp(g_giveItemSelected.load(),0,
                                 kGiveItemCount-1)];
}

void* VehicleCheatFunction() {
    for (Entry& cheat:g_cheats)
        if (cheat.kind==Kind::Vehicle&&cheat.fn) return cheat.fn;
    return nullptr;
}

int CategoryForIndex(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(g_cheats)))
        return CATEGORY_GENERAL;
    if (g_cheats[index].kind == Kind::Vehicle || index == 25 ||
        (index >= 61 && index <= 67))
        return CATEGORY_VEHICLES;
    if (index >= 20 && index <= 24)
        return CATEGORY_WEAPONS;
    if (index >= 37 && index <= 47)
        return CATEGORY_WEATHER;
    if ((index >= 27 && index <= 29) || (index >= 53 && index <= 60))
        return CATEGORY_SKINS;
    return CATEGORY_GENERAL;
}

int CategorySourceIndex(int category, int item) {
    if (category < 0 || category >= CATEGORY_COUNT || item < 0) return -1;
    if (category==CATEGORY_VEHICLES) {
        item-=kVehicleSelectorCount;
        if (item<0) return -1;
    }
    if (category==CATEGORY_WEAPONS) {
        item-=kWeaponSelectorCount;
        if (item<0) return -1;
    }
    for (int source=0; source<static_cast<int>(std::size(g_cheats)); ++source) {
        if (category==CATEGORY_VEHICLES&&g_cheats[source].kind==Kind::Vehicle)
            continue;
        if (CategoryForIndex(source)==category && item--==0) return source;
    }
    return -1;
}

}  // namespace

void Init(void* handle) {
    if (handle == nullptr) return;
    int ok = 0;
    for (Entry& cheat : g_cheats) {
        cheat.fn = dlsym(handle, cheat.sym);
        if (cheat.fn != nullptr) ++ok;
    }
    g_nativeInvincible = static_cast<bool*>(
        dlsym(handle, "_ZN10CPlayerPed22bDebugPlayerInvincibleE"));
    g_playerIsOffTheMap = static_cast<bool*>(
        dlsym(handle, "_ZN11CTheScripts18bPlayerIsOffTheMapE"));
    g_getStatValue = reinterpret_cast<float (*)(unsigned short)>(
        dlsym(handle, "_ZN6CStats12GetStatValueEt"));
    g_setStatValue = reinterpret_cast<void (*)(unsigned short, float)>(
        dlsym(handle, "_ZN6CStats12SetStatValueEtf"));
    g_isPlayerOnAMission = reinterpret_cast<bool (*)()>(
        dlsym(handle, "_ZN11CTheScripts18IsPlayerOnAMissionEv"));
    g_scriptBypassCheat = reinterpret_cast<void (*)()>(
        dlsym(handle, "_ZN6CCheat17ScriptBypassCheatEv"));
    if (g_nativeInvincible != nullptr)
        g_godMode.store(*g_nativeInvincible, std::memory_order_release);
    LOGI("cheats: resolved %d/%d handlers, native god state=%s", ok,
         static_cast<int>(std::size(g_cheats)),
         g_nativeInvincible != nullptr ? "yes" : "no");
}

void Tick() {
    // NOTE: an always-on ScriptBypassCheat was tried here and REVERTED: it
    // auto-passed scripted waits that are real gameplay beats (it skipped an
    // entire story bicycle ride). The bypass is manual-only again below.
    // Field diagnostic for the blocking touch tutorial: watch its step
    // counter and the stats-view global live.
    if (g.CTheScripts_ScriptSpace != nullptr) {
        static double lastTutorialLog = 0.0;
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double now = static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
        if (now - lastTutorialLog > 2.0) {
            lastTutorialLog = now;
            const auto* space =
                static_cast<const unsigned char*>(g.CTheScripts_ScriptSpace);
            // Full gate probe: pad control flags (0x110 disable, 0x12d
            // enter-car button, 0x133 vital-stats button) and the live
            // CWidgetHelpText queue with each entry's condition id + timer.
            int p110=-1,p12d=-1,p133=-1;
            if (g.CPad_GetPad) {
                if (const auto* pad = static_cast<const unsigned char*>(
                        g.CPad_GetPad(0))) {
                    p110=*reinterpret_cast<const unsigned short*>(pad+0x110);
                    p12d=pad[0x12d];
                    p133=pad[0x133];
                }
            }
            char queue[128]{}; int ql=0;
            if (g.CWidgetHelpText_m_pInstance) {
                if (const auto* inst = static_cast<const unsigned char*>(
                        *g.CWidgetHelpText_m_pInstance)) {
                    for (int i = 0; i < 10 && ql < 100; ++i) {
                        const unsigned char* e = inst + 0xcc + i * 0x334;
                        const unsigned cond =
                            *reinterpret_cast<const unsigned*>(e + 0x330);
                        const float timer =
                            *reinterpret_cast<const float*>(e + 0x320);
                        const short ch =
                            *reinterpret_cast<const short*>(e);
                        if (ch == 0 && cond == 0) continue;
                        ql += std::snprintf(queue + ql, sizeof(queue) - ql,
                                            " [%d]c=%u t=%.1f", i, cond,
                                            timer);
                    }
                }
            }
            LOGI("[tutorial] $B904=%d $AD3C=%d pad110=%d pad12d=%d pad133=%d help:%s",
                 *reinterpret_cast<const int*>(space + 0xB904),
                 *reinterpret_cast<const int*>(space + 0xAD3C),
                 p110, p12d, p133, ql ? queue : " empty");
        }
    }
    if (!g_unlockCitiesActive.load(std::memory_order_acquire)) return;
    if (!g_getStatValue || !g_setStatValue) return;
    if (g_getStatValue(181u) < 3.0f) {
        g_setStatValue(181u, 3.0f);
        if (g_playerIsOffTheMap) *g_playerIsOffTheMap = false;
        static unsigned int reassertCount = 0;
        if ((++reassertCount % 100u) == 1u)
            LOGI("cheat cities: stat181 re-asserted (script rewrote it, n=%u)",
                 reassertCount);
    }
}

int Count() { return static_cast<int>(std::size(g_cheats)); }

const char* Name(int index) {
    if (index < 0 || index >= Count()) return "";
    if (g_cheats[index].kind != Kind::GodMode) return g_cheats[index].label;
    return g_godMode.load(std::memory_order_acquire) ? "GOD MODE   ON" : "GOD MODE   OFF";
}

bool Available(int index) {
    return index >= 0 && index < Count() && g_cheats[index].fn != nullptr;
}

bool ToggleState(int index, bool* enabled) {
    if (enabled == nullptr || index < 0 || index >= Count() ||
        g_cheats[index].kind != Kind::GodMode)
        return false;
    *enabled = g_godMode.load(std::memory_order_acquire);
    return true;
}

void Activate(int index) {
    if (index < 0 || index >= Count()) return;
    if (g.FindPlayerPed == nullptr || g.FindPlayerPed(-1) == nullptr) {
        LOGW("cheat '%s' ignored (not in gameplay)", g_cheats[index].label);
        return;
    }
    Entry& cheat = g_cheats[index];
    if (cheat.fn == nullptr && cheat.kind != Kind::TutorialSkip) {
        LOGW("cheat '%s' unavailable (symbol missing)", cheat.label);
        return;
    }

    if (cheat.kind == Kind::Vehicle) {
        reinterpret_cast<void (*)(int)>(cheat.fn)(cheat.vehicleModel);
    } else if (cheat.kind == Kind::TutorialSkip) {
        auto* space = static_cast<unsigned char*>(g.CTheScripts_ScriptSpace);
        if (space == nullptr) { LOGW("tutorial skip: no ScriptSpace"); return; }
        auto* step = reinterpret_cast<int*>(space + 0xB904);
        LOGI("tutorial skip: step global $B904 %d -> 100", *step);
        *step = 100;
        return;
    } else if (cheat.kind == Kind::MissionSkip) {
        // cheat.fn resolves to the DoMissionSkip DATA byte, not code.
        if (g_isPlayerOnAMission == nullptr || !g_isPlayerOnAMission()) {
            LOGW("mission skip ignored: no mission running");
            return;
        }
        *static_cast<unsigned char*>(cheat.fn) = 1;
        LOGI("mission skip: DoMissionSkip=1 (main.scm takes over)");
    } else if (cheat.kind == Kind::UnlockCities) {
        // STAT_CITY_UNLOCKED (181) > 1 removes the forbidden-territory wanted
        // gate for all three cities (CGameLogic checks <=0 for SF, <=1 for
        // LV). 3.0 is the finished-story value. The tick below keeps
        // re-asserting it: the mission script owns this stat and can write its
        // own story progress back over a single poke.
        const float before =
            g_getStatValue ? g_getStatValue(181u) : -1.0f;
        reinterpret_cast<void (*)(unsigned short,float)>(cheat.fn)(181u,3.0f);
        g_unlockCitiesActive.store(true, std::memory_order_release);
        if (g_playerIsOffTheMap) *g_playerIsOffTheMap=false;
        LOGI("cheat cities: stat181 %.1f -> %.1f (enforced per-frame)",
             before, g_getStatValue ? g_getStatValue(181u) : -1.0f);
    } else {
        reinterpret_cast<void (*)()>(cheat.fn)();
        if (cheat.kind == Kind::GodMode) {
            const bool enabled = g_nativeInvincible != nullptr
                ? *g_nativeInvincible
                : !g_godMode.load(std::memory_order_relaxed);
            g_godMode.store(enabled, std::memory_order_release);
        } else if (cheat.kind == Kind::Reset) {
            // Rockstar's ResetCheats clears m_aCheatsActive but its debug-player
            // invincibility flag lives separately. Make the menu's "ALL" literal.
            if (g_godMode.load(std::memory_order_acquire) &&
                g_cheats[0].fn != nullptr)
                reinterpret_cast<void (*)()>(g_cheats[0].fn)();
            const bool enabled = g_nativeInvincible != nullptr
                ? *g_nativeInvincible : false;
            g_godMode.store(enabled, std::memory_order_release);
        }
    }
    LOGI("cheat activated: %s", cheat.label);
}

const char* CategoryName(int category) {
    switch (category) {
        case CATEGORY_GENERAL: return "GENERAL";
        case CATEGORY_WEAPONS: return "WEAPONS";
        case CATEGORY_SKINS: return "SKINS";
        case CATEGORY_WEATHER: return "WEATHER";
        case CATEGORY_VEHICLES: return "VEHICLES";
        default: return "CHEATS";
    }
}

int CategoryCount(int category) {
    if (category < 0 || category >= CATEGORY_COUNT) return 0;
    int count=0;
    if (category==CATEGORY_VEHICLES) count=kVehicleSelectorCount;
    if (category==CATEGORY_WEAPONS) count=kWeaponSelectorCount;
    for (int source=0; source<Count(); ++source) {
        if (category==CATEGORY_VEHICLES&&g_cheats[source].kind==Kind::Vehicle)
            continue;
        if (CategoryForIndex(source)==category) ++count;
    }
    return count;
}

const char* CategoryItemName(int category, int item) {
    if (category==CATEGORY_VEHICLES&&item>=0&&item<kVehicleSelectorCount) {
        static char labels[kVehicleSelectorCount][64]{};
        VehicleSelector& selector=g_vehicleSelectors[item];
        const int selected=std::clamp(selector.selected.load(),0,selector.count-1);
        std::snprintf(labels[item],sizeof(labels[item]),"%s   < %s >",
                      selector.label,selector.choices[selected].name);
        return labels[item];
    }
    if (category==CATEGORY_WEAPONS&&item==0) {
        static char label[96]{};
        std::snprintf(label,sizeof(label),"GIVE TO HAND   < %s >",
                      SelectedGiveItem().name);
        return label;
    }
    return Name(CategorySourceIndex(category,item));
}

bool CategoryItemAvailable(int category, int item) {
    if (category==CATEGORY_VEHICLES&&item>=0&&item<kVehicleSelectorCount)
        return VehicleCheatFunction()!=nullptr;
    if (category==CATEGORY_WEAPONS&&item>=0&&item<kWeaponSelectorCount)
        return g.CPed_GiveWeapon!=nullptr;
    return Available(CategorySourceIndex(category,item));
}

void ActivateCategoryItem(int category, int item) {
    if (category==CATEGORY_VEHICLES&&item>=0&&item<kVehicleSelectorCount) {
        if (!g.FindPlayerPed||!g.FindPlayerPed(-1)) return;
        void* fn=VehicleCheatFunction();
        if (!fn) return;
        VehicleSelector& selector=g_vehicleSelectors[item];
        const int selected=std::clamp(selector.selected.load(),0,selector.count-1);
        reinterpret_cast<void(*)(int)>(fn)(selector.choices[selected].model);
        LOGI("vehicle selector spawned %s (%d)",selector.choices[selected].name,
             selector.choices[selected].model);
        return;
    }
    if (category==CATEGORY_WEAPONS&&item==0) {
        void* ped=g.FindPlayerPed?g.FindPlayerPed(-1):nullptr;
        if (!ped||g.CPed_GiveWeapon==nullptr) return;
        const WeaponChoice& choice=SelectedGiveItem();
        const int slot=SlotForWeaponType(choice.type);
        // GiveWeapon into an occupied slot replaces the previous weapon.
        g.CPed_GiveWeapon(ped,choice.type,500u,true);
        // Straight into the RIGHT tracked hand for calibration. Brass knuckle
        // lives in slot 0 (fists) which the hand system cannot hold — it is
        // still given and works through the unarmed melee path.
        const bool held=slot>0?physicalweapon::ForceHold(1,slot):false;
        LOGI("weapon giver: %s (type %d, slot %d) held=%d",
             choice.name,choice.type,slot,held?1:0);
        return;
    }
    Activate(CategorySourceIndex(category,item));
}

bool CycleCategoryItem(int category,int item,int direction) {
    if (direction==0) return false;
    if (category==CATEGORY_VEHICLES&&item>=0&&item<kVehicleSelectorCount) {
        VehicleSelector& selector=g_vehicleSelectors[item];
        const int next=(selector.selected.load()+selector.count+
                        (direction<0?-1:1))%selector.count;
        selector.selected.store(next);
        return true;
    }
    if (category==CATEGORY_WEAPONS&&item==0) {
        const int step=direction<0?-1:1;
        g_giveItemSelected.store(
            (g_giveItemSelected.load()+kGiveItemCount+step)%kGiveItemCount);
        return true;
    }
    return false;
}

}  // namespace savr::cheats
