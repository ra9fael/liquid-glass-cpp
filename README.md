# liquid-glass-cpp

A Windows C++20 library for a continuous desktop glass surface, using Win32,
D3D11, DXGI Desktop Duplication, DirectComposition and a D2D content callback.
It has no Qt, WinShun, ImGui or system-monitor dependency.

`Window` owns the HWND, device, swap chain, backdrop textures and edge-reveal
animation. Applications own their settings, content model, layout, persistence,
and monitor-placement policy. Geometry is in physical pixels. `paintContent`
runs within BeginDraw/EndDraw and receives a borrowed D2D context at 96 DPI.
The callback must restore its transform and must not retain target resources.

The material uses a half-resolution separable Gaussian background, a circular
edge displacement profile, seven spectral sample positions and antialiased
rounded coverage. These material techniques follow imgui-liquid-glass's
AndroidLiquidGlassView-derived shader; Windows desktop capture is separate.
See THIRD-PARTY-NOTICES.md for source attribution.

## Build and test

Run in a Visual Studio developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Consumers use `add_subdirectory()` and link `liquid_glass::liquid_glass`.
Set `LIQUID_GLASS_BUILD_TESTS=OFF` to omit the standalone test executable.

## Test boundaries

The render executable injects a deterministic top-down BGRA source and reads
GPU pixels after rendering. It checks orientation, interior sampling, opaque
interior coverage, transparent corners, repeated resize and grayscale blur.
It does not require WinShun or sysinfo. Tests use an actual D3D11 device and
fail if window/device creation fails; they do not silently skip GPU checks.

## Limitations

- Desktop capture currently selects a single output on the device's adapter.
- Capture-visible mode temporarily changes capture affinity while acquiring
  a background frame. Its exclusion timing requires desktop integration testing.
- Automated RGBA tests verify render-target transparency, not every desktop
  compositor configuration or every external screen recorder.
- The material is a native implementation of the reference techniques;
  visual parity with the Android demo has not been established.
