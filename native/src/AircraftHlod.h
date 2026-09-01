#pragma once

namespace savr::airhlod {

// Records the independent, pitch-invariant far-world layer into the current
// RenderWare eye pass.  The caller restores DefinedState before stock world
// rendering, so this function owns no persistent RenderWare state.
void Render(float cameraX, float cameraY, bool aircraftActive,
            float requestedGroundRadiusM);

} // namespace savr::airhlod
