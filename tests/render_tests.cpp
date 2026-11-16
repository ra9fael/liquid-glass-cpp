#include <liquid_glass/window.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <d2d1_3.h>
#include <dwmapi.h>
void require(bool value,const char* message);
int channel(const liquid_glass::Image& image,int x,int y,int c);
LRESULT CALLBACK backgroundProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);
        int gray=static_cast<int>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        HBRUSH brush=CreateSolidBrush(RGB(gray,gray,gray));FillRect(dc,&r,brush);DeleteObject(brush);EndPaint(hwnd,&ps);return 0;}
    return DefWindowProcW(hwnd,message,w,l);
}

void settle(liquid_glass::Window& window) {
    const auto end=GetTickCount64()+350;
    while(GetTickCount64()<end) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        window.render();
        Sleep(20);
    }
}

void desktopTest() {
    WNDCLASSW cls{};cls.lpfnWndProc=backgroundProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"GlassTestBackdrop";RegisterClassW(&cls);
    HWND background=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,500,200,800,600,nullptr,nullptr,cls.hInstance,nullptr);
    require(background!=nullptr,"desktop background window");
    liquid_glass::Window window;
    float luminance=0;
    window.paintContent=[&](const liquid_glass::ContentFrame& f){
        luminance=f.backdropLuminance;
        ID2D1SolidColorBrush* brush=nullptr;
        f.context->CreateSolidColorBrush(D2D1::ColorF(1,0,0,1),&brush);
        if(brush){f.context->FillRectangle(D2D1::RectF(60,40,180,120),brush);brush->Release();}
    };
    auto fill=[&](int gray){
        SetWindowLongPtrW(background,GWLP_USERDATA,gray);
        SetWindowPos(background,HWND_TOPMOST,500,200,800,600,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        RedrawWindow(background,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ERASE);
        SetWindowPos(window.hwnd(),HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        DwmFlush();
    };
    for(bool visible:{false,true}) {
        window.setCaptureVisible(visible);
        window.show({700,400,240,160});
        for(int gray:{220,40,220}) {
            fill(gray);settle(window);
            HDC diagnostic=GetDC(nullptr);
            const auto desktop=GetPixel(diagnostic,650,350);
            ReleaseDC(nullptr,diagnostic);
            std::cout<<"background desktop RGB="<<static_cast<int>(GetRValue(desktop))<<","<<static_cast<int>(GetGValue(desktop))<<","<<static_cast<int>(GetBValue(desktop))<<'\n';
            require(desktop!=CLR_INVALID&&std::abs(static_cast<int>(GetRValue(desktop))-gray)<8
                &&std::abs(static_cast<int>(GetGValue(desktop))-gray)<8&&std::abs(static_cast<int>(GetBValue(desktop))-gray)<8,
                "test backdrop must be visible on the interactive desktop");
            const auto image=window.backdropSnapshot();
            require(!image.empty(),"desktop capture exists");
            const int x=image.width/2,y=image.height/2;
            std::cout<<"captureVisible="<<visible<<" background="<<gray<<" RGB="<<channel(image,x,y,2)<<","<<channel(image,x,y,1)<<","<<channel(image,x,y,0)<<" luma="<<luminance<<'\n';
            require(gray>100?luminance>.7f:luminance<.3f,"stationary desktop luminance follows background");
            require(std::abs(channel(image,x,y,2)-gray)<8,"capture excludes own content");
            require(std::abs(channel(image,x,y,1)-gray)<8,"capture retains underlying background");
            HDC screen=GetDC(nullptr);
            for(POINT p : {POINT{700,400},POINT{939,400},POINT{700,559},POINT{939,559}}){
                COLORREF pixel=GetPixel(screen,p.x,p.y);
                require(std::abs(static_cast<int>(GetRValue(pixel))-gray)<8,"desktop corners reveal background");
            }
            ReleaseDC(nullptr,screen);
        }
        for(int width : {360,300,240}) {
            const int height=width*2/3;
            window.setGeometry({700,400,width,height});
            fill(110);settle(window);
            std::cout<<"desktop resize="<<width<<" luma="<<luminance<<'\n';
            require(luminance>.35f&&luminance<.5f,"desktop resize captures new background");
            require(SUCCEEDED(window.lastError()),"desktop resize HRESULT");
            const auto resized=window.snapshot();
            require(resized.width==width&&resized.height==height,"desktop resize dimensions");
            SetWindowDisplayAffinity(window.hwnd(),WDA_NONE);
            DwmFlush();
            HDC screen=GetDC(nullptr);
            bool corners=true;
            for(POINT p : {POINT{700,400},POINT{700+width-1,400},POINT{700,400+height-1},POINT{700+width-1,400+height-1}}) {
                const COLORREF pixel=GetPixel(screen,p.x,p.y);
                corners &= pixel!=CLR_INVALID && std::abs(static_cast<int>(GetRValue(pixel))-110)<8
                    && std::abs(static_cast<int>(GetGValue(pixel))-110)<8 && std::abs(static_cast<int>(GetBValue(pixel))-110)<8;
            }
            const COLORREF content=GetPixel(screen,800,480);
            ReleaseDC(nullptr,screen);
            window.setCaptureVisible(visible);
            require(corners,"resized desktop corners reveal background");
            require(GetRValue(content)>230&&GetGValue(content)<20,"resized desktop content remains visible");
        }
    }
    window.hide();DestroyWindow(background);
}

void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int channel(const liquid_glass::Image& image,int x,int y,int c){return image.pixels[(y*image.width+x)*4+c];}
int main(){
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        liquid_glass::Window window;
        require(window.create(),"create D3D11 window");
        liquid_glass::Material material;
        material.blur=0; material.refraction=0; material.dispersion=0; material.rimLight=0; material.surfaceMotion=0; material.glass=100;
        window.setMaterial(material);
        liquid_glass::Image input; input.width=240; input.height=160; input.pixels.resize(240*160*4);
        for(int y=0;y<160;++y)for(int x=0;x<240;++x){
            const auto i=(y*240+x)*4;
            input.pixels[i]=static_cast<unsigned char>(y*255/159);
            input.pixels[i+1]=static_cast<unsigned char>(x*255/239);
            input.pixels[i+2]=80; input.pixels[i+3]=255;
        }
        window.setBackdrop(input);
        for(int size : {240,360,300,240}){
            const int height=size*2/3;
            material.radius=22*size/240;
            window.setMaterial(material);
            window.show({80,80,size,height});
            window.render(); window.render();
            require(SUCCEEDED(window.lastError()),"resize/render HRESULT");
            const auto result=window.snapshot();
            require(result.width==size&&result.height==height,"backbuffer dimensions after resize");
            require(channel(result,size/2,height/4,0)<channel(result,size/2,height*3/4,0),"background must not flip vertically");
            require(channel(result,size/4,height/2,1)<channel(result,size*3/4,height/2,1),"background must not flip horizontally");
            require(channel(result,size/2,height/2,3)==255,"interior replaces background without ghost blend");
            for(auto p : {liquid_glass::Point{0,0},{size-1,0},{0,height-1},{size-1,height-1}})
                require(channel(result,p.x,p.y,3)==0,"rounded corners must have zero alpha");
            require(std::abs(channel(result,size/2,height/4,0)-64)<8,"center sample position must stay aligned");
        }
        for(int y=0;y<160;++y)for(int x=0;x<240;++x){
            const auto i=(y*240+x)*4;
            const unsigned char v=((x/4+y/4)%2)?200:40;
            input.pixels[i]=input.pixels[i+1]=input.pixels[i+2]=v;
        }
        window.setBackdrop(input); material.blur=100; window.setMaterial(material);
        window.render(); window.render();
        const auto blurred=window.snapshot();
        for(int y=55;y<105;y+=7)for(int x=75;x<165;x+=7){
            const int b=channel(blurred,x,y,0),g=channel(blurred,x,y,1),r=channel(blurred,x,y,2);
            require(std::abs(b-g)<4&&std::abs(r-g)<4,"neutral blur must treat RGB consistently");
            require(g>80&&g<160,"blur must reduce checker contrast");
        }
        window.hide();
        material.animations=false; window.setMaterial(material);
        window.show({80,80,240,160});
        for(auto edge:{liquid_glass::Edge::Left,liquid_glass::Edge::Right,liquid_glass::Edge::Top,liquid_glass::Edge::Bottom}) {
            window.setEdgeCollapsed(true,edge);window.render();
            const auto panel=window.snapshot();
            require(channel(panel,120,80,3)==0,"collapsed panel center is transparent");
            const auto capsule=window.capsuleSnapshot();
            require(!capsule.empty(),"half circle snapshot exists");
            require(channel(capsule,capsule.width/2,capsule.height/2,3)>240,"half circle retains glass");
            const int outerX=edge==liquid_glass::Edge::Left?capsule.width-1:0;
            const int outerY=edge==liquid_glass::Edge::Top?capsule.height-1:0;
            require(channel(capsule,outerX,outerY,3)<10,"half circle outer corner is transparent");
            window.setEdgeCollapsed(false,edge);window.render();
            require(channel(window.snapshot(),120,80,3)==255,"expanded panel restores glass");
        }
        window.hide();
        desktopTest();
        std::cout<<"liquid_glass_render: all checks passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
