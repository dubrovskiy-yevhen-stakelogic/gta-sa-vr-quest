#pragma once

namespace savr::vrcam::detail {

// The Android game surface may be smaller than an XR eye texture. Identify
// its main pass by the registered surface size so reflection cameras stay mono.
constexpr bool IsStereoMainPassSize(int width, int height,
                                   int registeredWidth,
                                   int registeredHeight) noexcept {
    if (width < 64 || height < 64 || width >= 8192 || height >= 8192)
        return false;
    if (registeredWidth > 0 && registeredHeight > 0)
        return width == registeredWidth && height == registeredHeight;
    // Retain the original rule until both surface dimensions are available.
    return width >= 1024 && height >= 1024;
}

} // namespace savr::vrcam::detail
