#pragma once

#include "liquid_glass/window.h"

#include <algorithm>
#include <cmath>

namespace liquid_glass {

struct EdgeSpan {
    float first;
    float last;
};

inline bool capsuleHit(float x,float y,float anchorX,float anchorY,float radius)
{
    const float dx=x-anchorX,dy=y-anchorY;
    return dx*dx+dy*dy<=radius*radius;
}

inline float revealAnchor(float length, float radius, float requested)
{
    return std::clamp(requested, radius, length - radius);
}

inline EdgeSpan revealSpan(float length, float radius, float anchor, float progress)
{
    if(progress<=0) return {anchor-radius,anchor+radius};
    const float reach = radius + progress * (std::max(anchor,length-anchor)-radius);
    return {std::max(0.0f,anchor-reach),std::min(length,anchor+reach)};
}

inline EdgeSpan revealSpan(float length,float radius,float anchor,float progress,float workFirst,float workLast)
{
    const EdgeSpan span=revealSpan(length,radius,anchor,progress);
    const float first=std::max(span.first,workFirst);
    return {first,std::max(first,std::min(span.last,workLast))};
}

struct PanelShape {
    float left, top, right, bottom, radius, curve;
};

inline float animationEase(float value, int curve)
{
    const float t = std::clamp(value, 0.f, 1.f);
    if (curve == 0) return t;
    if (curve == 2) return t * t * (3.f - 2.f * t);
    if (curve == 3) return t * t * t;
    if (curve == 5) return .5f - .5f * std::cos(3.14159265358979323846f * t);
    if (curve == 6) return 1.f - (1.f + 3.f * t) * std::pow(1.f - t, 3.f);
    if (curve == 7) {
        constexpr float damping = 7.f;
        return std::clamp(1.f - std::exp(-damping * t) *
            (std::cos(10.f * t) + damping / 10.f * std::sin(10.f * t)), 0.f, 1.f);
    }
    if (curve == 4) {
        constexpr float overshoot = 1.2f;
        const float shifted = t - 1.f;
        return 1.f + (overshoot + 1.f) * shifted * shifted * shifted + overshoot * shifted * shifted;
    }
    return 1.f - (1.f - t) * (1.f - t);
}

inline PanelShape panelShape(float width, float height, Edge edge, float anchor,
                             float radius, float panelRadius, float progress,
                             float workFirst, float workLast, float centerY = -1.f)
{
    const bool vertical = edge == Edge::Left || edge == Edge::Right;
    const float depth = vertical ? width : height;
    if (edge == Edge::Center) {
        const float inset = std::min({1.f, width * .5f, height * .5f});
        const float r = std::min({radius, width * .5f - inset, height * .5f - inset});
        const float x = std::clamp(anchor, inset + r, width - inset - r);
        const float y = std::clamp(centerY < 0.f ? height * .5f : centerY, inset + r, height - inset - r);
        progress = std::clamp(progress, 0.f, 1.f);
        // Grow by the same reach on both sides until each side meets the panel boundary.
        const EdgeSpan horizontal = revealSpan(width - 2.f * inset, r, x - inset, progress);
        const EdgeSpan verticalSpan = revealSpan(height - 2.f * inset, r, y - inset, progress);
        return {horizontal.first + inset, verticalSpan.first + inset,
                horizontal.last + inset, verticalSpan.last + inset,
                std::min({std::lerp(r, panelRadius, progress), (horizontal.last - horizontal.first) * .5f,
                          (verticalSpan.last - verticalSpan.first) * .5f}),
                2.f+0.6f*std::sin(3.14159265f*progress)};
    }
    const float length = vertical ? height : width;
    const float firstLimit = std::max(1.f, workFirst + 1.f);
    const float lastLimit = std::min(length - 1, workLast - 1.f);
    const float reach = std::lerp(radius, std::max({radius, anchor - firstLimit, lastLimit - anchor}), progress);
    const float first = std::max(firstLimit, anchor - reach);
    const float last = std::min(lastLimit, anchor + reach);
    const bool startEdge = edge == Edge::Left || edge == Edge::Top;
    const float normalNear = std::lerp(-radius, 1.f, progress);
    const float normalFar = std::lerp(radius, depth - 1, progress);
    const float normalFirst = startEdge ? normalNear : depth - normalFar;
    const float normalLast = startEdge ? normalFar : depth - normalNear;
    const float corner = std::min({std::lerp(radius, panelRadius, progress),
                                   (last - first) * .5f, (normalFar - normalNear) * .5f});
    return vertical ? PanelShape{normalFirst, first, normalLast, last, corner, 2.f+0.6f*std::sin(3.14159265f*progress)}
                    : PanelShape{first, normalFirst, last, normalLast, corner, 2.f+0.6f*std::sin(3.14159265f*progress)};
}

} // namespace liquid_glass
