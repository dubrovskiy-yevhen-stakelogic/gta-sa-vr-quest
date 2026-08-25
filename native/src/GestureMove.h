#pragma once

namespace savr::gesturemove {

// Result of the per-frame gesture evaluation, merged into CPad::NewState by
// the pad hook. `forward` is a stick-equivalent [0,1] push in the movement
// frame (same path as the physical left stick, so MOVE DIRECTION settings
// apply); `sprint` holds ButtonCross (sprint on land, fast stroke in water).
struct Result {
    float forward{0.0f};
    bool  sprint{false};
};

// Evaluate arm-swing running / physical swim strokes for this pad frame.
// Runs on the GameThread from the CPad::UpdatePads hook. `ped` is the player
// (may be null), `swimming` mirrors CPhysical bSubmergedInWater, `blocked`
// covers menus/vehicles/parachute and resets all gesture state.
void Update(void* ped, bool swimming, bool blocked, Result& out);

} // namespace savr::gesturemove
