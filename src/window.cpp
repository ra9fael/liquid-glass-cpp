#include "liquid_glass/window.h"

#include <d2d1_3.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace liquid_glass {
namespace {

template <typename T> void release(T*& value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

constexpr wchar_t kClassName[] = L"LiquidGlassCppWindow";

const char* kGlassShader = R"(
cbuffer Frame : register(b0) {
    float4 sizeRadiusOpacity;
    float4 blurRefractionTimeMotion;
    float4 desktopSizeOrigin;
    float4 velocityAndShape;
    float4 opticalControls;
    float4 cursorAndLighting;
};
struct VOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Texture2D backdrop : register(t0);
Texture2D softenedBackdrop : register(t1);
SamplerState linearClamp : register(s0);
float3 sampleContent(float2 uv) {
    float2 halfTexel = .5 / desktopSizeOrigin.xy;
    uv = clamp(uv, halfTexel, 1.0-halfTexel);
    return lerp(backdrop.Sample(linearClamp,uv).rgb,
                softenedBackdrop.Sample(linearClamp,uv).rgb,
                saturate(blurRefractionTimeMotion.x/100.0));
}
float sdRoundRect(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r;
}
float sdSquircle(float2 p, float2 halfSize, float radius, float power) {
    float2 core = max(halfSize - radius, 1.0);
    float2 q = max(abs(p) - core, 0.0) / max(radius, 1.0);
    float implicit = pow(pow(q.x, power) + pow(q.y, power), 1.0 / power) - 1.0;
    return implicit * radius + min(max(abs(p).x - core.x, abs(p).y - core.y), 0.0);
}
float4 PS(VOut input) : SV_TARGET {
    float2 size = sizeRadiusOpacity.xy;
    float radius = sizeRadiusOpacity.z;
    float opacity = sizeRadiusOpacity.w;
    float blur = blurRefractionTimeMotion.x;
    float refraction = blurRefractionTimeMotion.y;
    float time = blurRefractionTimeMotion.z;
    float motion = blurRefractionTimeMotion.w;
    float2 desktopSize = desktopSizeOrigin.xy;
    float2 desktopOrigin = desktopSizeOrigin.zw;
    float2 velocity = velocityAndShape.xy;
    float edgePulse = velocityAndShape.z;
    float diagnostic = velocityAndShape.w;
    float dispersion = opticalControls.x;
    float rimLight = opticalControls.y;
    float surfaceMotion = opticalControls.z;
    float2 cursor = cursorAndLighting.xy;
    float cursorGlow = cursorAndLighting.z;
    float2 p = input.uv * size - size * .5;
    float corner = min(radius, min(size.x,size.y) * .42);
    float d = sdRoundRect(p, size * .5 - 1.0, corner);
    float coverage = 1.0 - smoothstep(-max(fwidth(d), .75), max(fwidth(d), .75), d);
    float edge = 1.0 - smoothstep(0.0, max(3.0, corner), -d);
    float rim = pow(edge, 2.25);
    float2 normal = normalize(float2(ddx(d), ddy(d)) + float2(.0001,.0001));
    float speed = saturate(length(velocity) / 1500.0) * motion * surfaceMotion + edgePulse;
    float2 flow = length(velocity) > .1 ? normalize(velocity) : float2(0,0);
    float leading = dot(normal, flow);
    float thickness = 1.0 + leading * speed * .32;
    float2 rawUv = diagnostic > .5 ? input.uv : (desktopOrigin + input.uv * size) / desktopSize;
    float refractBand = 1.0 - sqrt(max(0.0, 1.0 - edge * edge));
    float2 displacement = normal * thickness * refractBand * refraction * .28 / desktopSize;
    float2 refractedUv = rawUv + displacement;
    refractedUv += flow * refractBand * speed * 3.5 / desktopSize;
    float cursorDistance = length(p - cursor);
    float cursorLens = exp(-cursorDistance * cursorDistance / max(800.0, min(size.x,size.y) * min(size.x,size.y) * .10));
    refractedUv += normal * cursorLens * edge * cursorGlow * 1.4 / desktopSize;
    float2 spectrum = displacement * dispersion * (p.x*p.y/max(size.x*size.y*.25,1.0));
    float3 red = sampleContent(refractedUv+spectrum);
    float3 orange = sampleContent(refractedUv+spectrum*(2.0/3.0));
    float3 yellow = sampleContent(refractedUv+spectrum/3.0);
    float3 green = sampleContent(refractedUv);
    float3 cyan = sampleContent(refractedUv-spectrum/3.0);
    float3 blue = sampleContent(refractedUv-spectrum*(2.0/3.0));
    float3 purple = sampleContent(refractedUv-spectrum);
    float3 base = float3((red.r+orange.r+yellow.r)/3.5+purple.r/7.0,
        orange.g/7.0+(yellow.g+green.g+cyan.g)/3.5,
        (cyan.b+blue.b+purple.b)/3.0);
    float luma = dot(base, float3(.2126,.7152,.0722));
    float3 coolTint = float3(.20,.58,.82);
    float3 warmTint = float3(.82,.93,1.0);
    float3 tint = lerp(coolTint, warmTint, smoothstep(.32,.78,luma)) * (.002 + .012 * pow(edge,1.4));
    float highlightBias = saturate(.5 + .5 * leading * speed);
    float3 rimColor = lerp(float3(.55,.82,1.0), float3(1.0,.98,.90), smoothstep(.35,.80,luma));
    float2 lightDir = normalize(float2(-.62,-.78));
    float facing = saturate(dot(-normal, lightDir));
    float fresnel = pow(facing, 5.0) * rim;
    float innerRim = smoothstep(-max(7.0, blur*.19), -1.0, d) * (1.0-smoothstep(-1.0,0.0,d));
    float3 color = base + tint + rimColor * fresnel * (.018 + rimLight * (.00065 + refraction*.00001) + highlightBias*.045);
    color *= 1.0 - innerRim * (.018 + (1.0-luma)*.022);
    color += rimColor * pow(facing, 14.0) * pow(rim,1.55) * .035;
    color += rimColor * cursorLens * edge * cursorGlow * .11;
    color += sin(dot(p, float2(.035,.052)) + time * 3.0) * edge * speed * .025;
    float alpha = coverage;
    color = lerp(color, warmTint, (1.0-opacity)*.035);
    return float4(saturate(color) * alpha, alpha);
}
)";

const char* kBlurShader = R"(
cbuffer Blur : register(b0) { float2 texel; float2 direction; };
struct VOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Texture2D sourceTexture : register(t0);
SamplerState linearClamp : register(s0);
float4 Downsample(VOut input) : SV_TARGET {
    float2 d = texel * .5;
    return (sourceTexture.Sample(linearClamp, input.uv + float2(-d.x,-d.y)) +
            sourceTexture.Sample(linearClamp, input.uv + float2( d.x,-d.y)) +
            sourceTexture.Sample(linearClamp, input.uv + float2(-d.x, d.y)) +
            sourceTexture.Sample(linearClamp, input.uv + float2( d.x, d.y))) * .25;
}
float4 Kawase(VOut input) : SV_TARGET {
    float sigma=max(length(direction),.5);
    float2 axis=direction/sigma;
    float4 c=sourceTexture.Sample(linearClamp,input.uv);
    float total=1;
    int support=min(24,(int)ceil(sigma*3));
    for(int i=1;i<=support;++i) {
        float weight=exp(-.5*i*i/(sigma*sigma));
        float2 offset=axis*texel*i;
        c+=(sourceTexture.Sample(linearClamp,input.uv+offset)+sourceTexture.Sample(linearClamp,input.uv-offset))*weight;
        total+=2*weight;
    }
    return c/total;
}
)";

} // namespace

struct Window::Impl {
    Window* owner = nullptr;
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    IDCompositionDevice* compositionDevice = nullptr;
    IDCompositionTarget* compositionTarget = nullptr;
    IDCompositionVisual* compositionVisual = nullptr;
    IDCompositionRectangleClip* edgeClip = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11PixelShader* downsampleShader = nullptr;
    ID3D11PixelShader* kawaseShader = nullptr;
    ID3D11Buffer* frameBuffer = nullptr;
    ID3D11Buffer* blurBuffer = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* backdrop = nullptr;
    ID3D11ShaderResourceView* backdropView = nullptr;
    ID3D11Texture2D* blurPing = nullptr;
    ID3D11Texture2D* blurPong = nullptr;
    ID3D11RenderTargetView* blurPingTarget = nullptr;
    ID3D11RenderTargetView* blurPongTarget = nullptr;
    ID3D11ShaderResourceView* blurPingView = nullptr;
    ID3D11ShaderResourceView* blurPongView = nullptr;
    UINT blurWidth = 0;
    UINT blurHeight = 0;
    LONG backdropLeft = 0;
    LONG backdropTop = 0;
    ID3D11SamplerState* sampler = nullptr;
    DXGI_OUTPUT_DESC outputDesc{};
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* d2d = nullptr;
    ID2D1Bitmap1* d2dTarget = nullptr;
    Material model;
    HRESULT error = S_OK;
    bool fixedBackdrop = false;
    bool blurDirty = true;
    bool clickThrough = false;
    bool locked = false;
    bool dragging = false;
    POINT grab{};
    POINT origin{};
    POINT lastPointer{};
    ULONGLONG lastMotionTick = 0;
    float velocityX = 0;
    float velocityY = 0;
    bool collapsed = false;
    Edge edgeSide = Edge::Right;
    bool trackingMouse = false;
    float edgePulse = 0.0f;
    float edgeVelocityX = 0.0f;
    float edgeVelocityY = 0.0f;
    float visualOffsetX = 0.0f;
    float visualOffsetY = 0.0f;
    float targetOffsetX = 0.0f;
    float targetOffsetY = 0.0f;
    float backdropLuma = 0.22f;
    bool backdropDirty = true;
    ULONGLONG lastBackdropCapture = 0;
    bool backdropIsDiagnostic = false;
    bool forceDiagnosticPattern = false;
    bool captureVisible = true;

    bool initDevice();
    void resize(UINT width, UINT height);
    void releaseTarget();
    void ensureDuplication();
    void createDiagnosticBackdrop();
    void captureBackdrop();
    void ensureBlurTargets(UINT width, UINT height, DXGI_FORMAT format);
    void runBlur();
    void drawGlass(ID3D11RenderTargetView* destination, UINT width, UINT height, const RECT& desktopRect);
    void draw();
    Image snapshot() const;
    Image backdropSnapshot() const;
    Image capsuleSnapshot() const;
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

bool Window::Impl::initDevice()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL feature{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, requested,
                                 static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION, &device, &feature,
                                 &context)))
        return false;

    ID3DBlob* vertex = nullptr;
    ID3DBlob* pixel = nullptr;
    ID3DBlob* blur = nullptr;
    ID3DBlob* error = nullptr;
    const char* vs = "struct VOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; }; VOut VS(uint id : SV_VertexID) { float2 uv=float2((id<<1)&2,id&2); VOut o; o.position=float4(uv.x*2-1,1-uv.y*2,0,1); o.uv=uv; return o; }";
    if (FAILED(D3DCompile(vs, std::strlen(vs), "native-vs", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vertex, &error))) {
        release(error);
        return false;
    }
    if (FAILED(D3DCompile(kGlassShader, std::strlen(kGlassShader), "native-ps", nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                           &pixel, &error))) {
        release(vertex); release(error); return false;
    }
    if (FAILED(D3DCompile(kBlurShader, std::strlen(kBlurShader), "native-blur", nullptr, nullptr, "Downsample", "ps_5_0", 0, 0,
                          &blur, &error))) {
        release(vertex); release(pixel); release(error); return false;
    }
    const bool shaders = SUCCEEDED(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertexShader))
                          && SUCCEEDED(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &pixelShader))
                          && SUCCEEDED(device->CreatePixelShader(blur->GetBufferPointer(), blur->GetBufferSize(), nullptr, &downsampleShader));
    release(blur);
    if (shaders && FAILED(D3DCompile(kBlurShader, std::strlen(kBlurShader), "native-kawase", nullptr, nullptr, "Kawase", "ps_5_0", 0, 0,
                                     &blur, &error))) {
        release(vertex); release(pixel); release(error); return false;
    }
    if (shaders)
        device->CreatePixelShader(blur->GetBufferPointer(), blur->GetBufferSize(), nullptr, &kawaseShader);
    release(vertex); release(pixel); release(blur); release(error);
    if (!shaders)
        return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 96;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &frameBuffer)))
        return false;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &blurBuffer)))
        return false;
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDesc, &sampler)))
        return false;
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory));
}

void Window::Impl::releaseTarget()
{
    if (context)
        context->OMSetRenderTargets(0, nullptr, nullptr);
    release(d2dTarget); release(d2d); release(d2dDevice); release(target);
}

void Window::Impl::ensureBlurTargets(UINT width, UINT height, DXGI_FORMAT format)
{
    width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    if (blurPing && blurWidth == width && blurHeight == height)
        return;
    release(blurPingView); release(blurPongView); release(blurPingTarget); release(blurPongTarget); release(blurPing); release(blurPong);
    blurWidth = width; blurHeight = height;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &blurPing)) || FAILED(device->CreateTexture2D(&desc, nullptr, &blurPong)))
        return;
    device->CreateRenderTargetView(blurPing, nullptr, &blurPingTarget);
    device->CreateRenderTargetView(blurPong, nullptr, &blurPongTarget);
    device->CreateShaderResourceView(blurPing, nullptr, &blurPingView);
    device->CreateShaderResourceView(blurPong, nullptr, &blurPongView);
}

void Window::Impl::runBlur()
{
    if (!blurDirty || !backdrop || !backdropView)
        return;
    context->ClearState();
    D3D11_TEXTURE2D_DESC source{}; backdrop->GetDesc(&source);
    ensureBlurTargets(source.Width, source.Height, source.Format);
    if (!blurPingTarget || !blurPongTarget || !blurPingView || !blurPongView)
        return;
    auto pass = [&](ID3D11RenderTargetView* destination, ID3D11ShaderResourceView* sourceView, ID3D11PixelShader* shader, float dx, float dy) {
        const float clear[] = {0,0,0,0};
        context->OMSetRenderTargets(1, &destination, nullptr);
        context->ClearRenderTargetView(destination, clear);
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(blurWidth),static_cast<float>(blurHeight),0,1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader, nullptr, 0);
        const bool downsample = shader == downsampleShader;
        struct Blur { float tx,ty,dx,dy; } data{1.0f/(downsample?source.Width:blurWidth),1.0f/(downsample?source.Height:blurHeight),dx,dy};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(blurBuffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) { std::memcpy(mapped.pData,&data,sizeof(data));context->Unmap(blurBuffer,0); }
        context->PSSetConstantBuffers(0,1,&blurBuffer);
        context->PSSetShader(shader,nullptr,0);
        context->PSSetShaderResources(0,1,&sourceView);
        context->PSSetSamplers(0,1,&sampler);
        context->Draw(3,0);
        ID3D11ShaderResourceView* nullView=nullptr;context->PSSetShaderResources(0,1,&nullView);
    };
    pass(blurPingTarget, backdropView, downsampleShader, 0, 0);
    const float sigma=std::max(.5f,model.blur*.08f*model.scale);
    pass(blurPongTarget, blurPingView, kawaseShader, sigma, 0.0f);
    pass(blurPingTarget, blurPongView, kawaseShader, 0.0f, sigma);
    blurDirty=false;
}

void Window::Impl::ensureDuplication()
{
    if (!device || !hwnd)
        return;
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (duplication && outputDesc.Monitor == monitor)
        return;
    release(duplication);
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter))) {
        release(adapter); release(dxgiDevice); return;
    }
    for (UINT index = 0;; ++index) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        if (desc.Monitor == monitor) {
            outputDesc = desc;
            IDXGIOutput1* output1 = nullptr;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output1)))) {
                output1->DuplicateOutput(device, &duplication);
                release(output1);
            }
            release(output);
            break;
        }
        release(output);
    }
    release(adapter); release(dxgiDevice);
}

void Window::Impl::createDiagnosticBackdrop()
{
    constexpr UINT width = 640, height = 400;
    if (backdrop && backdropIsDiagnostic) return;
    release(backdropView); release(backdrop);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_DYNAMIC; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&backdrop))) return;
    if (FAILED(device->CreateShaderResourceView(backdrop,nullptr,&backdropView))) { release(backdrop); return; }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(backdrop,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) {
        const std::array<std::array<unsigned char,3>,4> palette{{{{236,70,81}},{{38,214,178}},{{46,139,255}},{{255,193,67}}}};
        for (UINT y=0;y<height;++y) {
            auto* row=static_cast<unsigned char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch;
            for (UINT x=0;x<width;++x) {
                const auto& c=palette[((x/80)+(y/80))%palette.size()];
                const bool grid=(x%80<4)||(y%80<4)||((x+y)%113<3);
                const unsigned char r=grid?255:c[0], g=grid?255:c[1], b=grid?255:c[2];
                row[x*4+0]=b; row[x*4+1]=g; row[x*4+2]=r; row[x*4+3]=255;
            }
        }
        context->Unmap(backdrop,0);
    }
    backdropLeft=0;backdropTop=0;backdropIsDiagnostic=true;backdropDirty=false;
}

void Window::Impl::captureBackdrop()
{
    if (fixedBackdrop) return;
    if (model.debugPattern || forceDiagnosticPattern) { createDiagnosticBackdrop(); return; }
    const ULONGLONG captureNow = GetTickCount64();
    const bool needsFrame = backdropDirty || captureNow - lastBackdropCapture >= 80;
    if (!needsFrame)
        return;
    ensureDuplication();
    if (!duplication || !device || !context)
        return;
    const bool restoreCaptureAffinity = captureVisible && hwnd;
    if (restoreCaptureAffinity) {
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
        DwmFlush();
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* resource = nullptr;
    const HRESULT acquired = duplication->AcquireNextFrame(16, &info, &resource);
    if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
        if (restoreCaptureAffinity) SetWindowDisplayAffinity(hwnd, WDA_NONE);
        return;
    }
    if (FAILED(acquired) || !resource) {
        if (acquired == DXGI_ERROR_ACCESS_LOST) release(duplication);
        if (restoreCaptureAffinity) SetWindowDisplayAffinity(hwnd, WDA_NONE);
        return;
    }
    ID3D11Texture2D* frame = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&frame)))) {
        D3D11_TEXTURE2D_DESC desc{}; frame->GetDesc(&desc);
        RECT window{}; GetWindowRect(hwnd, &window);
        constexpr LONG kCapturePad = 120;
        const LONG outputWidth = desc.Width;
        const LONG outputHeight = desc.Height;
        LONG sourceLeft = std::clamp<LONG>(window.left - outputDesc.DesktopCoordinates.left - kCapturePad, 0, outputWidth - 1);
        LONG sourceTop = std::clamp<LONG>(window.top - outputDesc.DesktopCoordinates.top - kCapturePad, 0, outputHeight - 1);
        const LONG sourceRight = std::clamp<LONG>(window.right - outputDesc.DesktopCoordinates.left + kCapturePad, sourceLeft + 1, outputWidth);
        const LONG sourceBottom = std::clamp<LONG>(window.bottom - outputDesc.DesktopCoordinates.top + kCapturePad, sourceTop + 1, outputHeight);
        const UINT cropWidth = static_cast<UINT>(sourceRight - sourceLeft);
        const UINT cropHeight = static_cast<UINT>(sourceBottom - sourceTop);
        D3D11_TEXTURE2D_DESC current{};
        if (backdrop) backdrop->GetDesc(&current);
        if (!backdrop || backdropIsDiagnostic || current.Width != cropWidth || current.Height != cropHeight || current.Format != desc.Format) {
            release(backdropView); release(backdrop);
            desc.Width = cropWidth; desc.Height = cropHeight;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0; desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_DEFAULT;
            if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &backdrop)))
                device->CreateShaderResourceView(backdrop, nullptr, &backdropView);
        }
        if (backdrop) {
            const D3D11_BOX crop{static_cast<UINT>(sourceLeft), static_cast<UINT>(sourceTop), 0,
                                 static_cast<UINT>(sourceRight), static_cast<UINT>(sourceBottom), 1};
            context->CopySubresourceRegion(backdrop, 0, 0, 0, 0, frame, 0, &crop);
            backdropLeft = sourceLeft;
            backdropTop = sourceTop;
            backdropIsDiagnostic = false;
            const UINT sx=cropWidth/2, sy=cropHeight/2;
            D3D11_TEXTURE2D_DESC staging=desc;
            backdrop->GetDesc(&staging);
            staging.Usage=D3D11_USAGE_STAGING; staging.BindFlags=0; staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ; staging.MiscFlags=0;
            ID3D11Texture2D* readback=nullptr;
            if(SUCCEEDED(device->CreateTexture2D(&staging,nullptr,&readback))){
                context->CopyResource(readback,backdrop);D3D11_MAPPED_SUBRESOURCE mapped{};
                if(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped))){
                    const auto* px=static_cast<const unsigned char*>(mapped.pData)+static_cast<size_t>(sy)*mapped.RowPitch+sx*4;
                    backdropLuma=(.0722f*px[0]+.7152f*px[1]+.2126f*px[2])/255.0f;
                    context->Unmap(readback,0);
                }release(readback);
            }
        }
    }
    release(frame); release(resource);
    duplication->ReleaseFrame();
    if (restoreCaptureAffinity) SetWindowDisplayAffinity(hwnd, WDA_NONE);
    backdropDirty = false;
    blurDirty = true;
    lastBackdropCapture = captureNow;
}

void Window::Impl::resize(UINT width, UINT height)
{
    if (!swapChain || !width || !height)
        return;
    releaseTarget();
    context->ClearState();
    error = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(error)) return;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&texture))))
        return;
    error = device->CreateRenderTargetView(texture, nullptr, &target);
    if (FAILED(error)) { release(texture); return; }
    IDXGISurface* surface = nullptr;
    if (SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&surface)))) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            d2dFactory->CreateDevice(dxgiDevice, &d2dDevice);
            release(dxgiDevice);
        }
        if (d2dDevice && SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d))) {
            const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            d2d->CreateBitmapFromDxgiSurface(surface, &properties, &d2dTarget);
            d2d->SetTarget(d2dTarget);
            d2d->SetDpi(96,96);
        }
        release(surface);
    }
    release(texture);
}

void Window::Impl::drawGlass(ID3D11RenderTargetView* destination, UINT width, UINT height, const RECT& desktop)
{
    if (!context || !destination)
        return;
    if (!width || !height)
        return;
    context->ClearState();
    const float clear[] = {0, 0, 0, 0};
    context->OMSetRenderTargets(1, &destination, nullptr);
    context->ClearRenderTargetView(destination, clear);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    D3D11_TEXTURE2D_DESC backdropDesc{};
    if (backdrop) backdrop->GetDesc(&backdropDesc);
    const float desktopW = std::max(1u, backdropDesc.Width);
    const float desktopH = std::max(1u, backdropDesc.Height);
    const ULONGLONG now = GetTickCount64();
    if (!dragging) {
        const float decay = std::pow(.88f, std::max<ULONGLONG>(1, now - lastMotionTick) / 16.0f);
        velocityX *= decay; velocityY *= decay;
        edgeVelocityX *= decay; edgeVelocityY *= decay; edgePulse *= std::pow(.82f, std::max<ULONGLONG>(1, now - lastMotionTick) / 16.0f);
    }
    POINT screenCursor{}; GetCursorPos(&screenCursor);
    const float localCursorX = static_cast<float>(screenCursor.x - desktop.left - width * .5f);
    const float localCursorY = static_cast<float>(screenCursor.y - desktop.top - height * .5f);
    struct Frame { float v[24]; } frame{{
        static_cast<float>(width), static_cast<float>(height), static_cast<float>(model.radius), model.glass / 100.0f,
        static_cast<float>(model.blur), static_cast<float>(model.refraction), static_cast<float>(now % 100000) / 1000.0f, 1.0f,
        desktopW, desktopH, static_cast<float>(desktop.left - outputDesc.DesktopCoordinates.left - backdropLeft),
        static_cast<float>(desktop.top - outputDesc.DesktopCoordinates.top - backdropTop),
        velocityX + edgeVelocityX, velocityY + edgeVelocityY, edgePulse, (fixedBackdrop || model.debugPattern || forceDiagnosticPattern) ? 1.0f : 0.0f,
        model.dispersion / 100.0f, model.rimLight / 100.0f, model.surfaceMotion / 100.0f, 0,
        localCursorX, localCursorY, model.surfaceMotion / 100.0f, 0}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(frameBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &frame, sizeof(frame));
        context->Unmap(frameBuffer, 0);
    }
    context->PSSetConstantBuffers(0, 1, &frameBuffer);
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* materialViews[] = {backdropView, blurPingView ? blurPingView : backdropView};
    context->PSSetShaderResources(0, 2, materialViews);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* nullViews[] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, nullViews);
}

void Window::Impl::draw()
{
    if (!context || !target || !swapChain)
        return;
    RECT rc{}; GetClientRect(hwnd, &rc);
    if (rc.right < 1 || rc.bottom < 1)
        return;
    captureBackdrop();
    runBlur();
    RECT desktop{}; GetWindowRect(hwnd, &desktop);
    drawGlass(target, static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom), desktop);
    const float width = static_cast<float>(rc.right);
    const float height = static_cast<float>(rc.bottom);

    if (d2d) {
        d2d->BeginDraw();
        if (owner->paintContent) owner->paintContent({d2d,width,height,backdropLuma});
        if (collapsed) {
            ID2D1SolidColorBrush* triggerBrush=nullptr;
            d2d->CreateSolidColorBrush(D2D1::ColorF(.68f,.90f,1.0f,.92f),&triggerBrush);
            const float triggerWidth=10.0f;
            // The visual is translated toward the edge. Keep the opposite source
            // sliver so it lands inside the original HWND after translation.
            if (edgeSide==Edge::Left)
                d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(width-triggerWidth,height*.5f-26,width,height*.5f+26),triggerWidth*.5f,triggerWidth*.5f),triggerBrush);
            else if (edgeSide==Edge::Right)
                d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0,height*.5f-26,triggerWidth,height*.5f+26),triggerWidth*.5f,triggerWidth*.5f),triggerBrush);
            else if (edgeSide==Edge::Top)
                d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(width*.5f-26,height-triggerWidth,width*.5f+26,height),triggerWidth*.5f,triggerWidth*.5f),triggerBrush);
            else
                d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(width*.5f-26,0,width*.5f+26,triggerWidth),triggerWidth*.5f,triggerWidth*.5f),triggerBrush);
            release(triggerBrush);
        }
        error = d2d->EndDraw();
        if (FAILED(error)) return;
    }
    error = swapChain->Present(1, 0);
}

LRESULT CALLBACK Window::Impl::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        impl = reinterpret_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    if (!impl)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_SIZE: impl->resize(LOWORD(lParam), HIWORD(lParam)); impl->draw(); return 0;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); impl->draw(); EndPaint(hwnd, &paint); return 0; }
    case WM_NCHITTEST:
        if (impl->clickThrough) return HTTRANSPARENT;
        if (impl->collapsed) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; RECT rect{}; GetWindowRect(hwnd, &rect);
            const bool right = impl->edgeSide == Edge::Right;
            const bool left = impl->edgeSide == Edge::Left;
            const bool top = impl->edgeSide == Edge::Top;
            const int x = point.x - rect.left, y = point.y - rect.top;
            const bool strip = right ? x >= rect.right - rect.left - 8 : left ? x < 8 : top ? y < 8 : y >= rect.bottom - rect.top - 8;
            return strip ? HTCLIENT : HTTRANSPARENT;
        }
        return HTCLIENT;
    case WM_LBUTTONDOWN:
        if (!impl->locked) { impl->dragging = true; GetCursorPos(&impl->grab); impl->lastPointer=impl->grab; impl->lastMotionTick=GetTickCount64(); RECT r{}; GetWindowRect(hwnd, &r); impl->origin={r.left,r.top}; SetCapture(hwnd); }
        return 0;
    case WM_MOUSEMOVE:
        KillTimer(hwnd, 41);
        if (!impl->trackingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            impl->trackingMouse = true;
        }
        if (impl->collapsed && impl->owner->edgeCollapsedChanged) impl->owner->edgeCollapsedChanged(false);
        if (impl->dragging) {
            POINT now{}; GetCursorPos(&now);
            const ULONGLONG tick=GetTickCount64(); const float dt=std::max(1.0f, static_cast<float>(tick-impl->lastMotionTick));
            impl->velocityX=(now.x-impl->lastPointer.x)*1000.0f/dt;
            impl->velocityY=(now.y-impl->lastPointer.y)*1000.0f/dt;
            impl->lastPointer=now; impl->lastMotionTick=tick;
            impl->backdropDirty=true;
            SetWindowPos(hwnd, HWND_TOPMOST, impl->origin.x + now.x - impl->grab.x, impl->origin.y + now.y - impl->grab.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            impl->draw();
        }
        return 0;
    case WM_SETCURSOR:
        if (!impl->trackingMouse && LOWORD(lParam) == HTCLIENT) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            impl->trackingMouse = true;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_LBUTTONUP:
        if (impl->dragging) {
            impl->dragging=false; ReleaseCapture();
            RECT r{}; GetWindowRect(hwnd,&r);
            if(impl->owner->moved) impl->owner->moved({r.left,r.top});
            GetWindowRect(hwnd,&r);
            POINT cursor{}; GetCursorPos(&cursor);
            if (impl->model.edgeHide && impl->owner->edgeCollapsedChanged && !PtInRect(&r, cursor))
                impl->owner->edgeCollapsedChanged(true);
        }
        return 0;
    case WM_RBUTTONUP:
        return 0;
    case WM_MOUSELEAVE:
        impl->trackingMouse = false;
        if (impl->model.edgeHide && !impl->dragging)
            SetTimer(hwnd, 41, 700, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == 41 && impl->model.edgeHide && !impl->dragging && impl->owner->edgeCollapsedChanged)
            impl->owner->edgeCollapsedChanged(true);
        if (wParam == 41) KillTimer(hwnd, 41);
        if (wParam == 43 && impl->compositionVisual) {
            const float ease = .26f;
            impl->visualOffsetX += (impl->targetOffsetX - impl->visualOffsetX) * ease;
            impl->visualOffsetY += (impl->targetOffsetY - impl->visualOffsetY) * ease;
            impl->compositionVisual->SetOffsetX(impl->visualOffsetX);
            impl->compositionVisual->SetOffsetY(impl->visualOffsetY);
            impl->compositionDevice->Commit();
            impl->draw();
            if (std::abs(impl->targetOffsetX - impl->visualOffsetX) < .4f && std::abs(impl->targetOffsetY - impl->visualOffsetY) < .4f) {
                impl->visualOffsetX = impl->targetOffsetX;
                impl->visualOffsetY = impl->targetOffsetY;
                impl->compositionVisual->SetOffsetX(impl->visualOffsetX);
                impl->compositionVisual->SetOffsetY(impl->visualOffsetY);
                impl->compositionDevice->Commit();
                KillTimer(hwnd, 43);
            }
        }
        return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

Image Window::Impl::snapshot() const
{
    if (!device || !context || !swapChain || !hwnd)
        return {};
    ID3D11Texture2D* source=nullptr;
    if (FAILED(swapChain->GetBuffer(0,IID_PPV_ARGS(&source)))) return {};
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging=desc;
    staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;staging.MiscFlags=0;
    ID3D11Texture2D* readback=nullptr;
    if(FAILED(device->CreateTexture2D(&staging,nullptr,&readback))){release(source);return {};}
    context->CopyResource(readback,source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Image out;
    if(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped))){
        out.width=desc.Width; out.height=desc.Height; out.pixels.resize(out.width*out.height*4);
        for(int y=0;y<out.height;++y) std::memcpy(out.pixels.data()+y*out.width*4,static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,out.width*4);
        context->Unmap(readback,0);
    }
    release(readback);release(source);return out;
}

Image Window::Impl::capsuleSnapshot() const
{
    if (!collapsed) return {};
    const Image panel = snapshot();
    if (panel.empty()) return {};
    constexpr int width = 10;
    constexpr int longSide = 52;
    const bool vertical=edgeSide==Edge::Left||edgeSide==Edge::Right;
    Image result; result.width=std::min(panel.width,vertical?width:longSide); result.height=std::min(panel.height,vertical?longSide:width);
    const int x=vertical?(edgeSide==Edge::Left?panel.width-result.width:0):(panel.width-result.width)/2;
    const int y=vertical?(panel.height-result.height)/2:(edgeSide==Edge::Top?panel.height-result.height:0);
    result.pixels.resize(result.width*result.height*4);
    for(int row=0;row<result.height;++row) std::memcpy(result.pixels.data()+row*result.width*4,panel.pixels.data()+((y+row)*panel.width+x)*4,result.width*4);
    return result;
}

Image Window::Impl::backdropSnapshot() const
{
    if (!device || !context || !backdrop)
        return {};
    D3D11_TEXTURE2D_DESC desc{}; backdrop->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging=desc;
    staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;staging.MiscFlags=0;
    ID3D11Texture2D* readback=nullptr;
    if(FAILED(device->CreateTexture2D(&staging,nullptr,&readback)))return {};
    context->CopyResource(readback,backdrop);
    D3D11_MAPPED_SUBRESOURCE mapped{};Image out;
    if(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped))){
        out.width=desc.Width; out.height=desc.Height; out.pixels.resize(out.width*out.height*4);
        for(int y=0;y<out.height;++y) std::memcpy(out.pixels.data()+y*out.width*4,static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,out.width*4);
        context->Unmap(readback,0);
    }
    release(readback);return out;
}

Window::Window() : m(std::make_unique<Impl>()) { m->owner = this; }
Window::~Window() { destroy(); }

bool Window::create()
{
    if (m->hwnd) return true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=kClassName; wc.lpfnWndProc=Impl::wndProc;
        wc.hCursor=LoadCursorW(nullptr, IDC_ARROW); registered = RegisterClassW(&wc) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
    }
    m->hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, kClassName, L"Liquid Glass", WS_POPUP,
                             0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),m.get());
    if (!m->hwnd || !m->initDevice()) return false;
    SetWindowDisplayAffinity(m->hwnd, WDA_EXCLUDEFROMCAPTURE);
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(m->device->QueryInterface(IID_PPV_ARGS(&dxgi)))) return false;
    IDXGIAdapter* adapter=nullptr; dxgi->GetAdapter(&adapter); IDXGIFactory2* factory=nullptr; adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=1; desc.Height=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.SampleDesc.Count=1; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT created = DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&m->compositionDevice));
    if (SUCCEEDED(created)) created = factory->CreateSwapChainForComposition(m->device, &desc, nullptr, &m->swapChain);
    if (SUCCEEDED(created)) created = m->compositionDevice->CreateTargetForHwnd(m->hwnd, TRUE, &m->compositionTarget);
    if (SUCCEEDED(created)) created = m->compositionDevice->CreateVisual(&m->compositionVisual);
    if (SUCCEEDED(created)) created = m->compositionVisual->SetContent(m->swapChain);
    if (SUCCEEDED(created)) created = m->compositionTarget->SetRoot(m->compositionVisual);
    if (SUCCEEDED(created)) created = m->compositionDevice->Commit();
    release(factory); release(adapter); release(dxgi);
    if (FAILED(created)) return false;
    return true;
}

void Window::destroy()
{
    if (!m) return;
    if (m->hwnd) DestroyWindow(m->hwnd);
    m->hwnd=nullptr; m->releaseTarget(); release(m->frameBuffer); release(m->blurBuffer); release(m->pixelShader); release(m->downsampleShader); release(m->kawaseShader); release(m->vertexShader); release(m->swapChain);
    release(m->edgeClip); release(m->compositionVisual); release(m->compositionTarget); release(m->compositionDevice);
    release(m->backdropView); release(m->backdrop); release(m->blurPingView); release(m->blurPongView); release(m->blurPingTarget); release(m->blurPongTarget); release(m->blurPing); release(m->blurPong); release(m->duplication); release(m->sampler);
    release(m->d2dFactory); release(m->context); release(m->device);
}

void Window::show(const Rect& geometry) { if(create()){setGeometry(geometry); ShowWindow(m->hwnd,SW_SHOWNOACTIVATE); SetWindowPos(m->hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE); render();} }
void Window::hide(){ if(m->hwnd) ShowWindow(m->hwnd,SW_HIDE); }
bool Window::visible() const{return m->hwnd&&IsWindowVisible(m->hwnd);}
HWND Window::hwnd() const{return m->hwnd;}
Rect Window::geometry() const{RECT r{};if(m->hwnd)GetWindowRect(m->hwnd,&r);return {r.left,r.top,r.right-r.left,r.bottom-r.top};}
void Window::setGeometry(const Rect& geometry){if(m->hwnd){m->backdropDirty=true;SetWindowPos(m->hwnd,HWND_TOPMOST,geometry.x,geometry.y,geometry.width,geometry.height,SWP_NOACTIVATE);}}
void Window::setClickThrough(bool enabled){m->clickThrough=enabled;}
void Window::setCaptureVisible(bool enabled){
    m->captureVisible=enabled;
    if(m->hwnd)SetWindowDisplayAffinity(m->hwnd, enabled ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
}
void Window::setLocked(bool enabled){m->locked=enabled;}
void Window::setMaterial(Material model){
    const bool materialChanged=m->model.blur!=model.blur||m->model.glass!=model.glass||m->model.refraction!=model.refraction||m->model.radius!=model.radius||m->model.dark!=model.dark;
    m->model=std::move(model);if(materialChanged){m->backdropDirty=true;m->blurDirty=true;}render();
}
void Window::setEdgeCollapsed(bool collapsed,Edge side){
    m->collapsed=collapsed;m->edgeSide=side;
    if (m->compositionVisual && m->hwnd) {
        RECT r{}; GetClientRect(m->hwnd, &r);
        const float slideReveal = 8.0f;
        float x=0,y=0;
        if (collapsed) {
            if (side == Edge::Left) { x = -static_cast<float>(r.right-r.left) + slideReveal; m->edgeVelocityX=-850; }
            else if (side == Edge::Right) { x = static_cast<float>(r.right-r.left) - slideReveal; m->edgeVelocityX=850; }
            else if (side == Edge::Top) { y = -static_cast<float>(r.bottom-r.top) + slideReveal; m->edgeVelocityY=-850; }
            else { y = static_cast<float>(r.bottom-r.top) - slideReveal; m->edgeVelocityY=850; }
            m->edgePulse=.72f;
        } else {
            if (side == Edge::Left) m->edgeVelocityX=850;
            else if (side == Edge::Right) m->edgeVelocityX=-850;
            else if (side == Edge::Top) m->edgeVelocityY=850;
            else m->edgeVelocityY=-850;
            m->edgePulse=.50f;
        }
        m->targetOffsetX=x; m->targetOffsetY=y;
        if (!m->edgeClip && m->compositionDevice)
            m->compositionDevice->CreateRectangleClip(&m->edgeClip);
        if (m->edgeClip) {
            const float reveal=10.0f;
            const float longSide=52.0f;
            if (collapsed) {
                if (side==Edge::Left) {
                    m->edgeClip->SetLeft(static_cast<float>(r.right-r.left)-reveal);m->edgeClip->SetRight(static_cast<float>(r.right-r.left));m->edgeClip->SetTop((r.bottom-r.top-longSide)*.5f);m->edgeClip->SetBottom((r.bottom-r.top+longSide)*.5f);
                } else if (side==Edge::Right) {
                    m->edgeClip->SetLeft(0.0f);m->edgeClip->SetRight(reveal);m->edgeClip->SetTop((r.bottom-r.top-longSide)*.5f);m->edgeClip->SetBottom((r.bottom-r.top+longSide)*.5f);
                } else if (side==Edge::Top) {
                    m->edgeClip->SetLeft((r.right-r.left-longSide)*.5f);m->edgeClip->SetRight((r.right-r.left+longSide)*.5f);m->edgeClip->SetTop(static_cast<float>(r.bottom-r.top)-reveal);m->edgeClip->SetBottom(static_cast<float>(r.bottom-r.top));
                } else {
                    m->edgeClip->SetLeft((r.right-r.left-longSide)*.5f);m->edgeClip->SetRight((r.right-r.left+longSide)*.5f);m->edgeClip->SetTop(0.0f);m->edgeClip->SetBottom(reveal);
                }
                m->compositionVisual->SetClip(m->edgeClip);
            } else {
                m->compositionVisual->SetClip(nullptr);
            }
        }
        SetTimer(m->hwnd, 43, 16, nullptr);
    }
    render();
}
bool Window::edgeCollapsed() const{return m->collapsed;}
void Window::render(){if(m&&m->hwnd&&visible())m->draw();}
Image Window::snapshot() const{return m?m->snapshot():Image();}
Image Window::backdropSnapshot() const{return m?m->backdropSnapshot():Image();}
Image Window::capsuleSnapshot() const{return m?m->capsuleSnapshot():Image();}
HRESULT Window::lastError() const{return m->error;}

void Window::setBackdrop(const Image& image) {
    if (!create() || image.width<=0 || image.height<=0 || image.pixels.size()!=static_cast<size_t>(image.width)*image.height*4) return;
    release(m->backdropView); release(m->backdrop);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=image.width; desc.Height=image.height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{image.pixels.data(),static_cast<UINT>(image.width*4),0};
    m->error=m->device->CreateTexture2D(&desc,&data,&m->backdrop);
    if (SUCCEEDED(m->error)) m->error=m->device->CreateShaderResourceView(m->backdrop,nullptr,&m->backdropView);
    m->fixedBackdrop=SUCCEEDED(m->error); m->blurDirty=true;
}

} // namespace liquid_glass
