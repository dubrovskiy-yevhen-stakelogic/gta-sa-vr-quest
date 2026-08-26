#pragma once

// VR weapon rendering (Approach B): extract the loaded weapon model's geometry
// from the game's RpAtomic and draw it with our own GL at the hand / holster
// anchor — the same GL path that already renders the VR hand mesh, so weapon and
// hand share one eye FBO / mvp / depth pass and stay perfectly aligned.
//
// Staged bring-up:
//   Stage 0 (current) — probe the held weapon's geometry and log vert/tri counts
//     to prove the type->modelId->clump->atomic->geometry chain is live on-device.
namespace savr::weapon {

// Call once per frame on the GameThread (engine state valid), in gameplay only.
void Update();

}  // namespace savr::weapon
