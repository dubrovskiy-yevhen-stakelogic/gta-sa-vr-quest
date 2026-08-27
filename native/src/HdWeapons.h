#pragma once

// HD weapon model set: registers the optional payload under files/hdweapons
// (extra streaming image + loose-PNG texture database) when enabled.
namespace savr::hdweapons {

// The payload files exist on this device.
bool Available();
// The image and texture database are registered in this session. Turning the
// option off after this point takes effect on the next app start.
bool Applied();
// Called from the per-frame input pump on the GameThread; applies once as
// soon as the engine is ready.
void Tick(bool enabled);

}  // namespace savr::hdweapons
