#pragma once

// VR world-pickup interaction. Hooks CPickup::Update (called by the game once
// per near-the-player pickup each frame) so map pickups physically rise while
// the player is inside their zone. A later layer adds hand-grab collection.
namespace savr::pickups {

// Resolve CPickup::Update from libGame.so and install the rise hook. Call once,
// after ResolveGameSymbols has succeeded (pass the same dlopen handle).
void Install(void* handle);

// Per-gameplay-frame follow-up: retries putting a just-collected weapon into
// the hand that physically grabbed its pickup (the game grants weapons a few
// frames later via GiveDelayedWeapon).
void Tick();

}  // namespace savr::pickups
