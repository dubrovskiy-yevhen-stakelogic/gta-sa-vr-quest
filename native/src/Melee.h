#pragma once

namespace savr::melee {

// Sample tracked hands once per GameThread input tick. A real fast swing is
// swept through GTA world collision and handed to SA's native damage endpoints.
void Update(bool interactionsBlocked);

// Clear velocity/cooldown history across menus, vehicles and lifecycle gaps.
void Reset();

} // namespace savr::melee
