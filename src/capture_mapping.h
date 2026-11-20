#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace liquid_glass {

struct CaptureMapping {
    RECT source{};
    float scaleX = 1.f;
    float scaleY = 1.f;
};

inline CaptureMapping captureMapping(const RECT& desktop, LONG textureWidth, LONG textureHeight,
                                     const RECT& window, LONG padding)
{
    const LONG desktopWidth = std::max(1L, desktop.right - desktop.left);
    const LONG desktopHeight = std::max(1L, desktop.bottom - desktop.top);
    const float scaleX = static_cast<float>(textureWidth) / desktopWidth;
    const float scaleY = static_cast<float>(textureHeight) / desktopHeight;
    const auto mapX = [&](LONG x) { return static_cast<LONG>(std::floor((x - desktop.left) * scaleX)); };
    const auto mapY = [&](LONG y) { return static_cast<LONG>(std::floor((y - desktop.top) * scaleY)); };
    const LONG left = std::clamp(mapX(window.left - padding), 0L, textureWidth - 1);
    const LONG top = std::clamp(mapY(window.top - padding), 0L, textureHeight - 1);
    const LONG right = std::clamp(mapX(window.right + padding), left + 1, textureWidth);
    const LONG bottom = std::clamp(mapY(window.bottom + padding), top + 1, textureHeight);
    return {{left, top, right, bottom}, scaleX, scaleY};
}

} // namespace liquid_glass
