#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ID2D1DeviceContext;

namespace liquid_glass {

struct Point { int x = 0, y = 0; };
struct Rect { int x = 0, y = 0, width = 0, height = 0; };
enum class Edge { Left, Right, Top, Bottom };

// BGRA8, top-down, tightly packed, premultiplied alpha.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
    bool empty() const { return pixels.empty(); }
};

struct Material {
    int style = 0; // 0: liquid, 1: frosted, 2: square embossed
    float tileSize = 24;
    bool dark = true;
    bool edgeHide = false;
    bool debugPattern = false;
    bool animations = true;
    int radius = 22;
    int blur = 34;
    int glass = 72;
    int refraction = 58;
    int dispersion = 12;
    int rimLight = 54;
    int surfaceMotion = 35;
    float scale = 1.0f;
};

struct ContentFrame {
    ID2D1DeviceContext* context;
    float width, height, backdropLuminance;
};

class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    bool create();
    void destroy();
    void show(const Rect& geometry);
    void hide();
    bool visible() const;
    HWND hwnd() const;
    Rect geometry() const;
    void setGeometry(const Rect& geometry);
    void setClickThrough(bool enabled);
    void setCaptureVisible(bool enabled);
    void setLocked(bool enabled);
    void setMaterial(Material material);
    void setEdgeCollapsed(bool collapsed, Edge side);
    bool edgeCollapsed() const;
    void render();
    Image snapshot() const;
    Image backdropSnapshot() const;
    Image capsuleSnapshot() const;
    // Fixed source for deterministic GPU tests and standalone previews.
    void setBackdrop(const Image& image);
    HRESULT lastError() const;
    std::function<void(Point)> moved;
    std::function<void(bool)> edgeCollapsedChanged;
    // Called inside BeginDraw/EndDraw; the caller must not retain the context.
    std::function<void(const ContentFrame&)> paintContent;
    // Optional diagnostics. GPU time is negative until a query completes.
    std::function<void(double frameMs, double captureMs, double gpuMs)> frameMeasured;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
