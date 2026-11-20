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

inline PanelShape panelShape(float width, float height, Edge edge, float anchor,
                             float radius, float panelRadius, float progress,
                             float workFirst, float workLast)
{
    const bool vertical = edge == Edge::Left || edge == Edge::Right;
    const float depth = vertical ? width : height;
    if (edge == Edge::Center) {
        const float halfWidth = std::lerp(radius, width * .5f - 1, progress);
        const float halfHeight = std::lerp(radius, height * .5f - 1, progress);
        return {width * .5f - halfWidth, height * .5f - halfHeight,
                width * .5f + halfWidth, height * .5f + halfHeight,
                std::min({std::lerp(radius, panelRadius, progress), halfWidth, halfHeight}),
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
