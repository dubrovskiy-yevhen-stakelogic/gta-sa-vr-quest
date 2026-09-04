#pragma once

// VR cheat menu backing: a table of GTA:SA cheat handlers (the game's own
// CCheat::* functions), resolved by symbol and invoked by menu index. Activation
// runs on the GameThread (right after the game tick) so the handlers' internal
// streaming/world work happens where the engine expects it.
namespace savr::cheats {

enum Category {
    CATEGORY_GENERAL = 0,
    CATEGORY_WEAPONS,
    CATEGORY_SKINS,
    CATEGORY_WEATHER,
    CATEGORY_VEHICLES,
    CATEGORY_COUNT
};

// Resolve the cheat symbols from the loaded libGame.so. Call once after the game
// symbols are resolved.
void Init(void* libGameHandle);

// Per-frame enforcement for latched cheats (GameThread). UNLOCK ALL CITIES
// keeps stat 181 pinned: the mission script owns that stat and writes its own
// story progress back over a one-shot poke.
void Tick();

// Number of cheats and the display label of one (for the menu list).
int         Count();
const char* Name(int index);

// Whether the row is backed by a symbol in this exact libGame.so.  The current
// SA 2.11 table is audited against recon/gtasa211-dynsym.txt, but keeping this
// query lets the UI fail visibly if Rockstar ships a different binary.
bool Available(int index);

// Toggle rows (currently native player invincibility) expose their live state so
// the VR menu can render ON/OFF without duplicating game state.
bool ToggleState(int index, bool* enabled);

// Activate the cheat at `index`. No-op unless in gameplay (a player ped exists).
void Activate(int index);

// Dev-only vehicle-spawn-by-button (calibration aid). The hidden cheat row
// "SPAWN VEHICLE BY BUTTON" flips SpawnByButtonEnabled(); while it is on, the
// grips+A chord calls RequestNextVehicle() to cycle model 400..611 on the
// GameThread. Both are inert in player builds (the row is not compiled and the
// chord never calls them).
void RequestNextVehicle();
bool SpawnByButtonEnabled();

// Sorted VR presentation. The original flat indices stay internal so every
// entry still invokes the same audited Rockstar handler.
const char* CategoryName(int category);
int         CategoryCount(int category);
const char* CategoryItemName(int category, int item);
bool        CategoryItemAvailable(int category, int item);
void        ActivateCategoryItem(int category, int item);
// Returns true only for a selector row (currently the compact vehicle spawn
// rows). L2/R2 can therefore edit the row without activating a cheat.
bool        CycleCategoryItem(int category, int item, int direction);

}  // namespace savr::cheats
