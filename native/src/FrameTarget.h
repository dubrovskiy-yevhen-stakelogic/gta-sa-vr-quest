#pragma once

namespace savr::frame_target {

// Single source of truth for the experiment. GTA's cap word, the outer pacer,
// and the OpenXR display-refresh request all derive from this value.
inline constexpr int kFps = 72;

static_assert(kFps > 0 && kFps <= 0xffff,
              "target FPS must fit the retail ARM64 MOVZ immediate");

} // namespace savr::frame_target
