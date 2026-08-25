#pragma once

#include <cstdint>

namespace savr::eye_depth {

using RwRasterCreateFn = void* (*)(int width, int height, int depth, int flags);

// Installs the retail-2.11 RQRenderTarget::Create GOT hook. The hook is
// process-start configurable through debug.savr.eye_depth24 (1 by default,
// 0 restores the stock D16 eye targets on the next launch).
bool Install(std::uintptr_t gameLoadBase);

// The marker is armed only around an eye camera-texture allocation. HUD and
// every other RenderWare raster keep their original depth format.
void* CreateStereoEyeRaster(RwRasterCreateFn create,
                            int width, int height, int depth, int flags);

}  // namespace savr::eye_depth
