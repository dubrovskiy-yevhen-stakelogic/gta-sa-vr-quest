#pragma once

namespace savr::graphicsfx {

// Record the VR-safe subset of GTA SA's late RenderEffects pass into the
// currently active RenderWare camera. The caller owns Begin/EndUpdate and has
// already installed the matching per-eye camera matrix.
void RenderEye(void* rwCamera, int eye);

// Draw the stock moon and R* constellation as world-space billboards after the
// eye sky gradient and before opaque world geometry. This replaces the legacy
// CPU-projected sprites suppressed by the stereo CClouds hook.
void RenderSkyEye(void* rwCamera, int eye);

// Replay SA's own C3dMarkers::Render into the active stereo eye. This is kept
// independent from the aggregate world-effects switch so mission arrows and
// checkpoint geometry remain visible when optional effects are disabled.
bool RenderStockMarkersEye(int eye);

// Internal process-start tier read from debug.savr.effects:
//   1 = default VR-safe effects: fire/blood, skidmarks/glass, ropes,
//       situational water cannons and searchlights
//   2 = balanced VR-safe effects
//   3 = extended geometry effects
// Tier 0 is retired: players control the entire stable tier through the
// persisted WORLD EFFECTS row in the graphics menu instead of an adb property.
int Profile();
bool Enabled();

// The eye pass is fail-safe: profiles 2/3 stay inert until the guarded retail
// GOT hook confirms that the now-invisible flat RenderEffects pass is
// suppressed. Profile 1 may instead use the narrower Fx_c::Render GOT hook so
// vehicle fire is moved into both eyes without paying for a third particle
// pass or depending on ownership of the whole RenderEffects slot.
void SetFlatPassSuppressed(bool suppressed);
void SetFireFlatPassSuppressed(bool suppressed);

} // namespace savr::graphicsfx
