#pragma once

namespace savr::vrfire {

// The physical-weapon layer owns holster/hand state. It must return true only
// when `weaponType` is currently held in its firing hand, and write that OpenXR
// hand (0 left, 1 right). A missing weapon or a type mismatch must return false.
using PhysicalFireQuery = bool (*)(int weaponType, int* firingHand);

void SetPhysicalFireQuery(PhysicalFireQuery query);

// Installs narrow hooks on player hitscan entry points. Non-player shots,
// vehicle shots, unheld weapons and invalid tracking all call the original game
// functions unchanged.
bool Install();

// GameThread owner for physical guns. This bypasses CJ's animation-driven
// attack task while retaining native CWeapon ammo/state/sound handling.
void Update(bool blocked);

} // namespace savr::vrfire
