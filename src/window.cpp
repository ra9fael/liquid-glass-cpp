#include "liquid_glass/window.h"
#include "edge_reveal.h"
#include "capture_mapping.h"

#include <d2d1_3.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <optional>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wtsapi32.lib")

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

bool verticalEdge(Edge edge)
{
    return edge == Edge::Left || edge == Edge::Right;
}

float capsuleRadius(Edge edge, float scale, float width, float height)
{
    const float along = verticalEdge(edge) ? height : width;
    const float depth = verticalEdge(edge) ? width : height;
    return std::min({26.0f * scale, along * 0.5f, depth});
}

const char* kGlassShader = R"(
cbuffer Frame : register(b0) {
    float4 sizeRadiusOpacity;
    float4 blurRefractionTimeMotion;
    float4 desktopSizeOrigin;
    float4 velocityAndShape;
    float4 opticalControls;
    float4 cursorAndLighting;
    float4 shapeBounds;
    float4 materialStyle;
    float4 revealSpan;
    float4 revealAndCaptureScale;
    float4 cursorMotionControls;
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
    float2 core = max(halfSize - radius, 0.0);
    float2 q = max(abs(p) - core, 0.0) / max(radius, .001);
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
    float cursorGlow = cursorAndLighting.z * cursorMotionControls.x;
    float2 localPosition = input.uv * size;
    float2 p = localPosition - size * .5;
    float corner = min(radius, min(size.x,size.y) * .42);
    float d = sdRoundRect(p, size * .5 - 1.0, corner);
        float4 bounds = shapeBounds;
    float2 shapeCenter = (bounds.xy + bounds.zw) * .5;
    float2 shapeHalfSize = (bounds.zw - bounds.xy) * .5;
    float shapeRadius = min(revealSpan.x, min(shapeHalfSize.x, shapeHalfSize.y));
    d = revealSpan.y <= 2.001 ? sdRoundRect(localPosition-shapeCenter, shapeHalfSize, shapeRadius)
        : sdSquircle(localPosition-shapeCenter, shapeHalfSize, shapeRadius, revealSpan.y);
    float coverage = 1.0 - smoothstep(-max(fwidth(d), .75), max(fwidth(d), .75), d);
    float edge = 1.0 - smoothstep(0.0, max(3.0, corner), -d);
    float rim = pow(edge, 2.25);
    float2 normal = normalize(float2(ddx(d), ddy(d)) + float2(.0001,.0001));
    float speed = saturate(length(velocity) / 1500.0) * motion * surfaceMotion + edgePulse;
    float2 flow = length(velocity) > .1 ? normalize(velocity) : float2(0,0);
    float leading = dot(normal, flow);
    float thickness = 1.0 + leading * speed * .32;
    float2 captureScale = max(revealAndCaptureScale.zw, float2(.001,.001));
    float2 rawUv = diagnostic > .5 ? input.uv : (desktopOrigin + input.uv * size * captureScale) / desktopSize;
    float refractBand = 1.0 - sqrt(max(0.0, 1.0 - edge * edge));
    float interior = saturate(edge * 2.0);
    float2 displacement = normal * thickness * refractBand * refraction * .10 / desktopSize;
    float2 refractedUv = rawUv + displacement;
    refractedUv += flow * refractBand * speed * 1.5 / desktopSize;
    float cursorDistance = length(p - cursor);
    float cursorLens = exp(-cursorDistance * cursorDistance / max(800.0, min(size.x,size.y) * min(size.x,size.y) * .10));
    refractedUv += (p-cursor) * cursorLens * cursorGlow * (.12 + cursorAndLighting.w*.18) / desktopSize;
    refractedUv += flow * speed * sin(time*8-length(p)*.035) * 3 / desktopSize;
    float2 tile = frac(input.uv*size/max(materialStyle.y,8))-.5;
    if(materialStyle.x>1.5) refractedUv += sin(tile*6.283185)*refraction*.06*interior/desktopSize;
    float2 spectrum = displacement * dispersion * .2 * (p.x*p.y/max(size.x*size.y*.25,1.0));
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
    if(materialStyle.x>1.5) color += pow(max(abs(tile.x),abs(tile.y))*2,16)*.09;
    if(diagnostic<.5) color=lerp(color,materialStyle.z>.5?float3(.035,.045,.065):float3(.94,.96,.98),lerp(.30,.02,saturate(opacity)));
    float alpha = coverage;
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
    float backdropScaleX = 1.f;
    float backdropScaleY = 1.f;
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
    ULONGLONG lastBackdropCopy = 0;
    bool backdropIsDiagnostic = false;
    bool forceDiagnosticPattern = false;
    bool captureVisible = true;
    bool sessionLocked = false;
    float collapseProgress = 0;
    std::optional<float> edgeAnchor;
    float revealProgress = 0;
    float revealStart = 0;
    float revealElapsedMs = 0;
    bool revealAnimating = false;
    bool collapsed = false;
    Edge edgeSide = Edge::Right;
    bool freeCollapsed = false;
    std::optional<Point> freeAnchor;
    bool wakeOnEntry = false;
    int workAreaFirst = 0;
    int workAreaLast = 0;

    float localAnchor(const RECT& rect) const
    {
        const bool vertical = verticalEdge(edgeSide);
        const float screenAnchor = edgeAnchor.value_or(vertical ? (rect.top+rect.bottom)*.5f : (rect.left+rect.right)*.5f);
        return screenAnchor - (vertical ? rect.top : rect.left);
    }
    float pressResponse = 0;
    bool pressed = false;
    ID3D11Query* timingDisjoint=nullptr;
    ID3D11Query* timingStart=nullptr;
    ID3D11Query* timingEnd=nullptr;
    bool timingPending=false;
    ULONGLONG pointerOutsideSince = 0;
    ULONGLONG lastDrawTick = 0;

    bool initDevice();
    void updateInputStyle();
    void resize(UINT width, UINT height);
    void releaseTarget();
    void ensureDuplication();
    void createDiagnosticBackdrop();
    void captureBackdrop();
    void ensureBlurTargets(UINT width, UINT height, DXGI_FORMAT format);
    void runBlur();
    PanelShape revealShape(float width, float height, const RECT& rect, float progress) const;
    void drawGlass(ID3D11RenderTargetView* destination, UINT width, UINT height, const RECT& desktopRect, const PanelShape& shape);
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
    buffer.ByteWidth = 176;
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
    const float sigma=std::max(.5f,(model.style==1?100:model.blur)*.08f*model.scale);
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
    outputDesc = {};
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
    if (fixedBackdrop || sessionLocked) return;
    if (model.debugPattern || forceDiagnosticPattern) { createDiagnosticBackdrop(); return; }
    const ULONGLONG captureNow = GetTickCount64();
    const bool needsFrame = backdropDirty || captureNow - lastBackdropCapture >= 16;
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
    const HRESULT acquired = duplication->AcquireNextFrame(0, &info, &resource);
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
        const CaptureMapping mapping = captureMapping(outputDesc.DesktopCoordinates,
            static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height), window, kCapturePad);
        LONG sourceLeft = mapping.source.left;
        LONG sourceTop = mapping.source.top;
        LONG sourceRight = mapping.source.right;
        LONG sourceBottom = mapping.source.bottom;
        const float scaleX = mapping.scaleX;
        const float scaleY = mapping.scaleY;
        const UINT cropWidth = static_cast<UINT>(sourceRight - sourceLeft);
        const UINT cropHeight = static_cast<UINT>(sourceBottom - sourceTop);
        if (!backdropDirty && backdrop && !captureVisible && captureNow-lastBackdropCopy<250) {
            bool changed=info.TotalMetadataBufferSize==0 && info.LastPresentTime.QuadPart!=0;
            const RECT region{sourceLeft,sourceTop,sourceRight,sourceBottom};
            RECT overlap{};
            UINT bytes=0;
            duplication->GetFrameDirtyRects(0,nullptr,&bytes);
            std::vector<RECT> dirty(bytes/sizeof(RECT));
            if(bytes && SUCCEEDED(duplication->GetFrameDirtyRects(bytes,dirty.data(),&bytes)))
                for(const auto& rect:dirty) changed |= IntersectRect(&overlap,&region,&rect)!=FALSE;
            else if(bytes) changed=true;
            bytes=0;
            duplication->GetFrameMoveRects(0,nullptr,&bytes);
            std::vector<DXGI_OUTDUPL_MOVE_RECT> moves(bytes/sizeof(DXGI_OUTDUPL_MOVE_RECT));
            if(bytes && SUCCEEDED(duplication->GetFrameMoveRects(bytes,moves.data(),&bytes)))
                for(const auto& move:moves) changed |= IntersectRect(&overlap,&region,&move.DestinationRect)!=FALSE;
            else if(bytes) changed=true;
            if(!changed) {
                release(frame);release(resource);duplication->ReleaseFrame();lastBackdropCapture=captureNow;return;
            }
        }
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
            backdropScaleX = scaleX;
            backdropScaleY = scaleY;
            backdropIsDiagnostic = false;
            const UINT sx=cropWidth/2, sy=cropHeight/2;
            D3D11_TEXTURE2D_DESC staging=desc;
            backdrop->GetDesc(&staging);
            staging.Width=1; staging.Height=1;
            staging.Usage=D3D11_USAGE_STAGING; staging.BindFlags=0; staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ; staging.MiscFlags=0;
            ID3D11Texture2D* readback=nullptr;
            if(SUCCEEDED(device->CreateTexture2D(&staging,nullptr,&readback))){
                const D3D11_BOX center{sx,sy,0,sx+1,sy+1,1};
                context->CopySubresourceRegion(readback,0,0,0,0,backdrop,0,&center);D3D11_MAPPED_SUBRESOURCE mapped{};
                if(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped))){
                    const auto* px=static_cast<const unsigned char*>(mapped.pData);
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
    lastBackdropCopy = captureNow;
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

PanelShape Window::Impl::revealShape(float width, float height, const RECT& rect, float progress) const
{
    const float panelRadius = std::min({static_cast<float>(model.radius), width * .5f, height * .5f});
    const float radius = capsuleRadius(edgeSide, model.scale, width, height);
    if (edgeSide == Edge::Center) {
        const float inset = std::min({1.f, width * .5f, height * .5f});
        const float r = std::max(0.f, std::min({radius, width * .5f - inset, height * .5f - inset}));
        const float x = std::clamp(freeAnchor ? static_cast<float>(freeAnchor->x) : width * .5f,
                                   inset + r, width - inset - r);
        const float y = std::clamp(freeAnchor ? static_cast<float>(freeAnchor->y) : height * .5f,
                                   inset + r, height - inset - r);
        const EdgeSpan horizontal = revealSpan(width - 2.f * inset, r, x - inset, progress);
        const EdgeSpan verticalSpan = revealSpan(height - 2.f * inset, r, y - inset, progress);
        return {horizontal.first + inset, verticalSpan.first + inset,
                horizontal.last + inset, verticalSpan.last + inset,
                std::min({std::lerp(r, panelRadius, progress), (horizontal.last - horizontal.first) * .5f,
                          (verticalSpan.last - verticalSpan.first) * .5f}),
                2.f + .6f * std::sin(3.14159265f * progress)};
    }
    const bool vertical = verticalEdge(edgeSide);
    const float workOrigin = static_cast<float>(vertical ? rect.top : rect.left);
    const bool hasWorkArea = workAreaFirst != 0 || workAreaLast != 0;
    return panelShape(width, height, edgeSide, localAnchor(rect), radius, panelRadius, progress,
                      hasWorkArea ? workAreaFirst - workOrigin : 0.f,
                      hasWorkArea ? workAreaLast - workOrigin : (vertical ? height : width));
}

void Window::Impl::drawGlass(ID3D11RenderTargetView* destination, UINT width, UINT height, const RECT& desktop, const PanelShape& shape)
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
    const float desktopW = static_cast<float>(std::max(1u, backdropDesc.Width));
    const float desktopH = static_cast<float>(std::max(1u, backdropDesc.Height));
    const ULONGLONG now = GetTickCount64();
    if (!dragging) {
        const float decay = std::pow(.88f, std::max<ULONGLONG>(1, now - lastMotionTick) / 16.0f);
        velocityX *= decay; velocityY *= decay;
        edgeVelocityX *= decay; edgeVelocityY *= decay; edgePulse *= std::pow(.82f, std::max<ULONGLONG>(1, now - lastMotionTick) / 16.0f);
    }
    POINT screenCursor{}; GetCursorPos(&screenCursor);
    const float localCursorX = static_cast<float>(screenCursor.x - desktop.left - width * .5f);
    const float localCursorY = static_cast<float>(screenCursor.y - desktop.top - height * .5f);
    struct Frame { float v[44]; } frame{{
        static_cast<float>(width), static_cast<float>(height), static_cast<float>(model.radius), model.glass / 100.0f,
        static_cast<float>(model.style==1?100:model.blur), static_cast<float>(model.style==1?model.refraction*.15f:model.refraction), static_cast<float>(now % 100000) / 1000.0f, model.animations?1.0f:0.0f,
        desktopW, desktopH,
        static_cast<float>(desktop.left - outputDesc.DesktopCoordinates.left) * backdropScaleX - backdropLeft,
        static_cast<float>(desktop.top - outputDesc.DesktopCoordinates.top) * backdropScaleY - backdropTop,
        velocityX + edgeVelocityX, velocityY + edgeVelocityY, edgePulse, (fixedBackdrop || model.debugPattern || forceDiagnosticPattern) ? 1.0f : 0.0f,
        model.dispersion / 100.0f, model.rimLight / 100.0f, model.surfaceMotion / 100.0f, 0,
        localCursorX, localCursorY, model.animations ? model.surfaceMotion / 100.0f : 0, pressResponse,
        0, 0, 0, 0,
        static_cast<float>(model.style), model.tileSize*model.scale, model.dark?1.f:0.f, 0,
        0, 0, 0, 0,
        revealProgress, 0, backdropScaleX, backdropScaleY,
        dragging ? 0.f : 1.f, 0, 0, 0}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    frame.v[24]=shape.left; frame.v[25]=shape.top; frame.v[26]=shape.right; frame.v[27]=shape.bottom;
    frame.v[32]=shape.radius;
    frame.v[33]=shape.curve;
    frame.v[34]=revealProgress;
    frame.v[40]=shape.radius;
    frame.v[41]=shape.curve;
    frame.v[42]=0;
    frame.v[43]=0;
    frame.v[35]=1.f;
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
    const auto tick=GetTickCount64();
    LARGE_INTEGER started{},captured{},finished{},frequency{};
    const bool measure=static_cast<bool>(owner->frameMeasured);
    double gpuMs=-1;
    bool queryStarted=false;
    if(measure) {
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
        if(!timingDisjoint) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0}; device->CreateQuery(&desc,&timingDisjoint);
            desc.Query=D3D11_QUERY_TIMESTAMP; device->CreateQuery(&desc,&timingStart); device->CreateQuery(&desc,&timingEnd);
        }
        if(timingPending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{}; UINT64 begin=0,end=0;
            if(context->GetData(timingDisjoint,&disjoint,sizeof(disjoint),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
               context->GetData(timingStart,&begin,sizeof(begin),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
               context->GetData(timingEnd,&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK) {
                if(!disjoint.Disjoint) gpuMs=1000.0*(end-begin)/disjoint.Frequency;
                timingPending=false;
            }
        }
    }
    const float dt=lastDrawTick ? std::min(64.f,static_cast<float>(tick-lastDrawTick)) : 16.f;
    lastDrawTick=tick;
    pressResponse += ((pressed?1.f:0.f)-pressResponse)*(1-std::exp(-dt/90.f));
    const float goal = collapsed ? 1.f : 0.f;
    if (!model.animations) {
        revealProgress = goal;
        revealAnimating = false;
    } else if (revealAnimating) {
        revealElapsedMs = std::min(revealElapsedMs + dt, static_cast<float>(model.animationDurationMs));
        const float linear = revealElapsedMs / std::max(1, model.animationDurationMs);
        const float eased = animationEase(linear, model.animationCurve);
        revealProgress = std::lerp(revealStart, goal, eased);
        if (linear >= 1.f) revealAnimating = false;
    }
    collapseProgress = revealProgress;
    RECT desktop{}; GetWindowRect(hwnd, &desktop);
    captureBackdrop();
    if(measure) QueryPerformanceCounter(&captured);
    if(measure && !timingPending && timingDisjoint && timingStart && timingEnd) {
        context->Begin(timingDisjoint); context->End(timingStart); queryStarted=true;
    }
    runBlur();
    const float width = static_cast<float>(rc.right);
    const float height = static_cast<float>(rc.bottom);
    const PanelShape shape = revealShape(width, height, desktop, 1.f - revealProgress);
    drawGlass(target, static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom), desktop, shape);

    if (d2d) {
        d2d->BeginDraw();
        ID2D1PathGeometry* geometry=nullptr;
        ID2D1GeometrySink* sink=nullptr;
        d2dFactory->CreatePathGeometry(&geometry);
        if(geometry && SUCCEEDED(geometry->Open(&sink))){
            const D2D1_POINT_2F tl{shape.left,shape.top},tr{shape.right,shape.top},br{shape.right,shape.bottom},bl{shape.left,shape.bottom};
            const float r=shape.radius;
            // Match the shader's superellipse at the diagonal of each corner.
            const float k = (std::pow(.5f, 1.f / shape.curve) - .5f) * (8.f / 3.f);
            sink->BeginFigure({tl.x+r,tl.y},D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({tr.x-r,tr.y});
            sink->AddBezier(D2D1_BEZIER_SEGMENT{{tr.x-r+k*r,tr.y},{tr.x,tr.y+r-k*r},{tr.x,tr.y+r}});
            sink->AddLine({br.x,br.y-r});
            sink->AddBezier(D2D1_BEZIER_SEGMENT{{br.x,br.y-r+k*r},{br.x-r+k*r,br.y},{br.x-r,br.y}});
            sink->AddLine({bl.x+r,bl.y});
            sink->AddBezier(D2D1_BEZIER_SEGMENT{{bl.x+r-k*r,bl.y},{bl.x,bl.y-r+k*r},{bl.x,bl.y-r}});
            sink->AddLine({tl.x,tl.y+r});
            sink->AddBezier(D2D1_BEZIER_SEGMENT{{tl.x,tl.y+r-k*r},{tl.x+r-k*r,tl.y},{tl.x+r,tl.y}});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
            const float contentOpacity = 1.f - animationEase(revealProgress, model.animationCurve);
            d2d->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),geometry,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),contentOpacity),nullptr);
            if (owner->paintContent && contentOpacity > .001f)
                owner->paintContent({d2d,width,height,backdropLuma});
            d2d->PopLayer();
        }
        release(sink); release(geometry);
        error = d2d->EndDraw();
        if (FAILED(error)) return;
    }
    if(queryStarted) {context->End(timingEnd);context->End(timingDisjoint);timingPending=true;}
    error = swapChain->Present(1, 0);
    if(measure) {
        QueryPerformanceCounter(&finished);
        owner->frameMeasured(1000.0*(finished.QuadPart-started.QuadPart)/frequency.QuadPart,
            1000.0*(captured.QuadPart-started.QuadPart)/frequency.QuadPart,gpuMs);
    }
}

// The window is layered throughout; WS_EX_TRANSPARENT takes it out of hit
// testing, which is the only thing that works across processes (HTTRANSPARENT
// only hands the message to windows of the same thread). Collapsed panels are
// transparent too: the panel keeps its full rectangle while it is off the edge,
// and only the capsule's polling (WM_TIMER 44) brings it back.
void Window::Impl::updateInputStyle()
{
    if (!hwnd) return;
    const bool wanted = clickThrough || collapsed;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (((style & WS_EX_TRANSPARENT) != 0) == wanted) return;
    style = wanted ? (style | WS_EX_TRANSPARENT) : (style & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) {
            impl->sessionLocked = true;
            release(impl->duplication);
        } else if (wParam == WTS_SESSION_UNLOCK) {
            impl->sessionLocked = false;
            impl->backdropDirty = true;
            impl->blurDirty = true;
            impl->lastBackdropCapture = 0;
            impl->ensureDuplication();
            impl->draw();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        release(impl->duplication);
        impl->outputDesc = {};
        impl->backdropDirty = true;
        impl->blurDirty = true;
        impl->lastBackdropCapture = 0;
        impl->lastBackdropCopy = 0;
        impl->draw();
        return 0;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); impl->draw(); EndPaint(hwnd, &paint); return 0; }
    case WM_NCHITTEST:
        // Same-thread fallback; the cross-process answer is WS_EX_TRANSPARENT
        // (updateInputStyle), which keeps this message from arriving at all.
        if (impl->clickThrough || impl->collapsed) return HTTRANSPARENT;
        return HTCLIENT;
    case WM_LBUTTONDOWN:
        impl->pressed=true;
        if (!impl->locked) { impl->dragging = true; GetCursorPos(&impl->grab); impl->lastPointer=impl->grab; impl->lastMotionTick=GetTickCount64(); RECT r{}; GetWindowRect(hwnd, &r); impl->origin={r.left,r.top}; SetCapture(hwnd); }
        return 0;
    case WM_MOUSEMOVE:
        if (!impl->trackingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            impl->trackingMouse = true;
        }
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
        impl->pressed=false;
        {
            if (impl->dragging) {
                RECT r{}; GetWindowRect(hwnd,&r);
                POINT cursor{}; GetCursorPos(&cursor);
                const bool moved = cursor.x != impl->grab.x || cursor.y != impl->grab.y;
                if (moved)
                    impl->freeAnchor = Point{cursor.x - r.left, cursor.y - r.top};
                impl->dragging=false; ReleaseCapture();
                if(impl->owner->moved) impl->owner->moved({r.left,r.top}, {cursor.x,cursor.y});
                impl->pointerOutsideSince = 0;
            }
        }
        return 0;
    case WM_RBUTTONUP:
        return 0;
    case WM_CAPTURECHANGED:
        impl->pressed=false;impl->dragging=false;
        return 0;
    case WM_MOUSELEAVE:
        impl->trackingMouse = false;
        return 0;
    case WM_TIMER:
        if (wParam == 44 && IsWindowVisible(hwnd)) {
            POINT cursor{};
            RECT bounds{};
            GetCursorPos(&cursor);
            GetWindowRect(hwnd, &bounds);
            if (impl->collapsed) {
                if (!impl->owner->hitTestCollapsedCapsule({cursor.x, cursor.y})) {
                    impl->wakeOnEntry = true;
                } else if (impl->wakeOnEntry) {
                    impl->wakeOnEntry = false;
                    if (impl->freeCollapsed)
                        impl->owner->setFreeCollapsed(false);
                    else if (impl->owner->edgeCollapsedChanged)
                        impl->owner->edgeCollapsedChanged(false);
                    else
                        impl->owner->setCollapsed(false);
                }
            } else if (impl->pressed || impl->dragging || PtInRect(&bounds, cursor)) {
                impl->pointerOutsideSince = 0;
            } else if (impl->model.edgeHide || impl->model.freeHideEnabled) {
                const auto now = GetTickCount64();
                if (!impl->pointerOutsideSince) impl->pointerOutsideSince = now;
                const auto elapsed = now - impl->pointerOutsideSince;
                const bool freeDue = impl->model.freeHideEnabled && impl->model.freeHideDelayMs > 0 &&
                    elapsed >= static_cast<ULONGLONG>(impl->model.freeHideDelayMs);
                const bool edgeDue = impl->model.edgeHide && elapsed >= 700;
                const bool didEdgeCollapse = edgeDue && impl->owner->edgeCollapsedChanged &&
                    impl->owner->edgeCollapsedChanged(true);
                if (didEdgeCollapse) {
                    impl->pointerOutsideSince = 0;
                } else if (freeDue) {
                    impl->owner->setFreeCollapsed(true);
                    impl->pointerOutsideSince = 0;
                }
            }
            impl->draw();
        }
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
    const bool vertical = edgeSide == Edge::Left || edgeSide == Edge::Right;
    if (freeCollapsed) {
        const int diameter = static_cast<int>(std::ceil(std::min({
            capsuleRadius(edgeSide, model.scale, static_cast<float>(panel.width), static_cast<float>(panel.height)) * 2.f,
            static_cast<float>(panel.width - 2), static_cast<float>(panel.height - 2)})));
        Image result; result.width = std::min(panel.width, diameter); result.height = std::min(panel.height, diameter);
        const int x = (panel.width - result.width) / 2;
        const int y = (panel.height - result.height) / 2;
        result.pixels.resize(result.width * result.height * 4);
        for (int row = 0; row < result.height; ++row)
            std::memcpy(result.pixels.data() + row * result.width * 4,
                        panel.pixels.data() + ((y + row) * panel.width + x) * 4, result.width * 4);
        return result;
    }
    const int radius = static_cast<int>(std::ceil(capsuleRadius(edgeSide, model.scale, static_cast<float>(panel.width), static_cast<float>(panel.height))));
    const int longSide = radius * 2;
    Image result; result.width = std::min(panel.width, vertical ? radius : longSide); result.height = std::min(panel.height, vertical ? longSide : radius);
    const int x = vertical ? (edgeSide == Edge::Right ? panel.width - result.width : 0) : (panel.width - result.width) / 2;
    const int y = vertical ? (panel.height - result.height) / 2 : (edgeSide == Edge::Bottom ? panel.height - result.height : 0);
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
    m->hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, kClassName, L"Liquid Glass", WS_POPUP,
                             0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),m.get());
    if (!m->hwnd || !m->initDevice()) return false;
    // A layered window is only composed once its attributes are set; 255 keeps
    // DirectComposition's alpha, which is what draws the glass.
    SetLayeredWindowAttributes(m->hwnd, 0, 255, LWA_ALPHA);
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
    WTSRegisterSessionNotification(m->hwnd, NOTIFY_FOR_THIS_SESSION);
    SetTimer(m->hwnd,44,16,nullptr);
    return true;
}

void Window::destroy()
{
    if (!m) return;
    release(m->timingDisjoint);release(m->timingStart);release(m->timingEnd);m->timingPending=false;
    if (m->hwnd) {
        WTSUnRegisterSessionNotification(m->hwnd);
        DestroyWindow(m->hwnd);
    }
    m->hwnd=nullptr; m->releaseTarget(); release(m->frameBuffer); release(m->blurBuffer); release(m->pixelShader); release(m->downsampleShader); release(m->kawaseShader); release(m->vertexShader); release(m->swapChain);
    release(m->edgeClip); release(m->compositionVisual); release(m->compositionTarget); release(m->compositionDevice);
    release(m->backdropView); release(m->backdrop); release(m->blurPingView); release(m->blurPongView); release(m->blurPingTarget); release(m->blurPongTarget); release(m->blurPing); release(m->blurPong); release(m->duplication); release(m->sampler);
    release(m->d2dFactory); release(m->context); release(m->device);
}

void Window::show(const Rect& geometry) { if(create()){setGeometry(geometry); ShowWindow(m->hwnd,SW_SHOWNOACTIVATE); SetWindowPos(m->hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE); render();} }
void Window::hide(){ m->pointerOutsideSince = 0; if(m->hwnd) ShowWindow(m->hwnd,SW_HIDE); }
bool Window::visible() const{return m->hwnd&&IsWindowVisible(m->hwnd);}
HWND Window::hwnd() const{return m->hwnd;}
Rect Window::geometry() const{RECT r{};if(m->hwnd)GetWindowRect(m->hwnd,&r);return {r.left,r.top,r.right-r.left,r.bottom-r.top};}
void Window::setGeometry(const Rect& geometry){if(m->hwnd){m->backdropDirty=true;SetWindowPos(m->hwnd,HWND_TOPMOST,geometry.x,geometry.y,geometry.width,geometry.height,SWP_NOACTIVATE);}}
void Window::setClickThrough(bool enabled){m->clickThrough=enabled;m->updateInputStyle();}
void Window::setCaptureVisible(bool enabled){
    m->captureVisible=enabled;
    if(m->hwnd)SetWindowDisplayAffinity(m->hwnd, enabled ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
}
void Window::setLocked(bool enabled){m->locked=enabled;}
void Window::setMaterial(Material model){
    const bool materialChanged=m->model.style!=model.style||m->model.blur!=model.blur||m->model.glass!=model.glass||m->model.refraction!=model.refraction||m->model.radius!=model.radius||m->model.dark!=model.dark;
    if (m->model.edgeHide != model.edgeHide || m->model.freeHideEnabled != model.freeHideEnabled ||
        m->model.freeHideDelayMs != model.freeHideDelayMs)
        m->pointerOutsideSince = 0;
    m->model=std::move(model);
    if (!m->model.freeHideEnabled && m->freeCollapsed) setFreeCollapsed(false);
    if (!m->model.edgeHide && m->collapsed && !m->freeCollapsed) setCollapsed(false);
    if(materialChanged){m->backdropDirty=true;m->blurDirty=true;}render();
}
void Window::setEdgeCollapsed(bool collapsed,Edge side,std::optional<int> anchor,int workAreaFirst,int workAreaLast){
    RECT rect{}; if(m->hwnd) GetWindowRect(m->hwnd,&rect);
    const bool wasCollapsed = m->collapsed;
    const Edge previousEdge = m->edgeSide;
    const bool edgeChanged = wasCollapsed && collapsed && !m->freeCollapsed && previousEdge != side;
    if (wasCollapsed != collapsed) {
        m->revealStart = m->revealProgress;
        m->revealElapsedMs = 0;
        m->revealAnimating = m->model.animations;
        if (!m->model.animations) m->revealProgress = collapsed ? 1.f : 0.f;
    }
    m->collapsed=collapsed;m->freeCollapsed=false;m->edgeSide=side;
    m->workAreaFirst=workAreaFirst;m->workAreaLast=workAreaLast;
    if (anchor) m->edgeAnchor = static_cast<float>(*anchor);
    else if (!wasCollapsed && collapsed) m->edgeAnchor = verticalEdge(side)
        ? (rect.top + rect.bottom) * .5f : (rect.left + rect.right) * .5f;
    else if (edgeChanged) m->edgeAnchor = verticalEdge(side)
        ? (rect.top + rect.bottom) * .5f : (rect.left + rect.right) * .5f;
    m->pointerOutsideSince = 0;
    POINT cursor{};
    GetCursorPos(&cursor);
    m->wakeOnEntry = !PtInRect(&rect, cursor);
    m->updateInputStyle();render();
}
void Window::setFreeCollapsed(bool collapsed){
    if (!m->hwnd || (m->collapsed && !m->freeCollapsed) || m->freeCollapsed == collapsed) return;
    m->revealStart = m->revealProgress;
    m->revealElapsedMs = 0;
    m->revealAnimating = m->model.animations;
    if (!m->model.animations) m->revealProgress = collapsed ? 1.f : 0.f;
    m->freeCollapsed = collapsed;
    m->collapsed = collapsed;
    m->edgeSide = Edge::Center;
    if (!collapsed && !m->freeAnchor) {
        RECT rect{}; GetWindowRect(m->hwnd, &rect);
        m->freeAnchor = Point{(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
    }
    m->edgeAnchor.reset();
    m->pointerOutsideSince = 0;
    m->wakeOnEntry = true;
    m->updateInputStyle();
    render();
}
void Window::setCollapsed(bool collapsed){
    if (!m->hwnd) return;
    if (m->collapsed == collapsed && !m->freeCollapsed) return;
    m->revealStart = m->revealProgress;
    m->revealElapsedMs = 0;
    m->revealAnimating = m->model.animations;
    if (!m->model.animations) m->revealProgress = collapsed ? 1.f : 0.f;
    const bool wasFreeCollapsed = m->freeCollapsed;
    m->freeCollapsed = false;
    m->collapsed = collapsed;
    if (wasFreeCollapsed)
        m->edgeSide = Edge::Center;
    m->pointerOutsideSince = 0;
    m->wakeOnEntry = true;
    m->updateInputStyle();
    render();
}

bool Window::edgeCollapsed() const{return m->collapsed;}
bool Window::hitTestCollapsedCapsule(Point point) const
{
    if(!m || !m->hwnd || !m->collapsed) return false;
    RECT rect{}; GetWindowRect(m->hwnd,&rect);
    const float width=static_cast<float>(rect.right-rect.left),height=static_cast<float>(rect.bottom-rect.top);
    const PanelShape shape = m->revealShape(width, height, rect, m->freeCollapsed ? 0.f : 1.f - m->revealProgress);
    const float localX=static_cast<float>(point.x-rect.left),localY=static_cast<float>(point.y-rect.top);
    if(localX<shape.left||localX>shape.right||localY<shape.top||localY>shape.bottom)return false;
    const float dx=std::max(shape.left+shape.radius-localX,0.f)+std::max(localX-(shape.right-shape.radius),0.f);
    const float dy=std::max(shape.top+shape.radius-localY,0.f)+std::max(localY-(shape.bottom-shape.radius),0.f);
    return dx*dx+dy*dy<=shape.radius*shape.radius;
}
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
