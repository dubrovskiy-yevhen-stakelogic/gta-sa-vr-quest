#pragma once

// Pause-menu map zoom on the controller grips: right grip zooms in, left
// grip zooms out, centred on the middle of the screen. Implemented as a
// hook on Menu_MapUpdate(float) - the function only runs while the map
// page is actually displayed, so it is its own perfect gate.
namespace savr::mapzoom {

// Resolve Menu_MapUpdate + gMobileMenu and install the hook. Call once with
// the libGame.so NOLOAD handle.
void Install(void* handle);

// Fed every frame from the input pump with the raw squeeze axes [0,1].
void SetGrips(float left, float right);

}  // namespace savr::mapzoom
