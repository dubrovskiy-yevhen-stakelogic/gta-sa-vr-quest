#pragma once

namespace savr::throwable {

// Install the narrow CWeapon::FireProjectile hook used only for the local
// player's physical grenade/tear-gas/Molotov/satchel release. Native SA remains
// responsible for ammo, fuse, projectile allocation, effects and crime events.
bool Install();

// qbuild-compatible interaction: press/hold R2 to publish a collision-clipped
// trajectory, release R2 to call the native projectile path with that launch.
// Call once per on-foot GameThread frame after PhysicalWeapon::Update.
void Update(bool interactionsBlocked);
void Reset();

} // namespace savr::throwable
