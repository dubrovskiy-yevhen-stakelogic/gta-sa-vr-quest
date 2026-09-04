#pragma once

// Restores GTA:SA's animated (UV-scrolling) textures on the mobile 2.11.311
// build. Rockstar's port left the master UV-animation switch off, so every
// scrolling texture the PC/PS2 originals had (waterfalls, conveyor belts,
// ammu/casino screens, some neon signs, etc.) is frozen. This re-enables them
// natively, with no external mod or asset download. See AnimatedTextures.cpp
// for the disassembly evidence.
namespace savr::animtex {

// Install the UV-animation fix: hook RpMaterialUVAnimAddAnimTime and patch
// the 11 Param-apply words. See AnimatedTextures.cpp for the disassembly
// evidence and for why the RunUVAnim byte must NOT be poked.
void Install(void* libGameHandle);

// Game-thread re-assert of the flag (one byte/frame) to the current on/off
// state. Nothing in the retail binary ever clears it, so this is mostly

// Live on/off for the ANIMATED TEXTURES graphics-menu row (default ON). OFF
// re-freezes UV scrolling, which is how the fix is verified visually.
void SetEnabled(bool enabled);
bool IsEnabled();

}  // namespace savr::animtex
