#include <liquid_glass/window.h>
#include "../src/edge_reveal.h"
#include "../src/capture_mapping.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <d2d1_3.h>
#include <dwmapi.h>
#include <windows.h>
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
            const auto glass=window.snapshot();
            if(gray==220) require(channel(glass,30,80,1)>170,"desktop glass must retain bright backdrop");
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
void cornerAnimationProbe()
{
    using namespace liquid_glass;
    constexpr int width = 300, height = 160;
    constexpr int firstLimit = 20, lastLimit = 280;
    Window window;
    require(window.create(), "create corner animation window");
    Material material;
    material.edgeHide = true;
    material.glass = 100;
    material.blur = material.refraction = material.dispersion = material.rimLight = material.surfaceMotion = 0;
    window.setMaterial(material);
    Image backdrop{width, height, std::vector<std::uint8_t>(width * height * 4, 0)};
    for (size_t i = 0; i < backdrop.pixels.size(); i += 4)
        backdrop.pixels[i] = backdrop.pixels[i + 3] = 255;
    window.setBackdrop(backdrop);
    window.show({100, 100, width, height});
    const auto rect = window.geometry();
    window.paintContent = [](const ContentFrame& frame) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(frame.context->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.f, 0.f), &brush))) {
            frame.context->FillRectangle(D2D1::RectF(0, 0, frame.width, frame.height), brush);
            brush->Release();
        }
    };

    for (const auto edge : {Edge::Top, Edge::Bottom}) {
        for (const bool left : {true, false}) {
            material.animations = false;
            window.setMaterial(material);
            const int anchor = left ? firstLimit + 34 : lastLimit - 34;
            const int first = rect.x + firstLimit, last = rect.x + lastLimit;
            window.setEdgeCollapsed(true, edge, rect.x + anchor, first, last);
            material.animations = true;
            window.setMaterial(material);
            window.setEdgeCollapsed(false, edge, {}, first, last);

            int firstBlockedFrame = -1, farAtContact = 0, farthest = 0;
            int intermediateFrames = 0, maxMismatch = 0, maxOutside = 0;
            for (int frame = 0; frame < 75; ++frame) {
                window.render();
                const auto image = window.snapshot();
                require(image.width == width && image.height == height, "corner frame readback size");
                int firstVisible = width, lastVisible = -1;
                int contentLeft = width, contentRight = -1, mismatch = 0;
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const int alpha = channel(image, x, y, 3);
                        const int red = channel(image, x, y, 2);
                        if (x < firstLimit || x >= lastLimit) maxOutside = std::max(maxOutside, alpha);
                        if (alpha >= 128) {
                            firstVisible = std::min(firstVisible, x);
                            lastVisible = std::max(lastVisible, x);
                        }
                        if (red > 128) {
                            contentLeft = std::min(contentLeft, x);
                            contentRight = std::max(contentRight, x);
                        }
                    }
                }
                if (contentRight >= contentLeft && lastVisible >= firstVisible)
                    mismatch = std::max(std::abs(contentLeft - firstVisible), std::abs(contentRight - lastVisible));
                maxMismatch = std::max(maxMismatch, mismatch);
                const int blocked = left ? firstVisible : lastVisible;
                const int growingSide = left ? lastVisible : width - 1 - firstVisible;
                const int boundary = left ? firstLimit + 1 : lastLimit - 2;
                if (std::abs(blocked - boundary) <= 1) {
                    if (firstBlockedFrame < 0) {
                        firstBlockedFrame = frame;
                        farAtContact = growingSide;
                    }
                    farthest = std::max(farthest, growingSide);
                    if (growingSide > farAtContact && growingSide < (left ? lastLimit - 2 : width - firstLimit - 2))
                        ++intermediateFrames;
                } else if (firstBlockedFrame >= 0) {
                    require(false, "blocked side moves after contact");
                }
                Sleep(16);
            }
            std::cout << "corner " << (edge == Edge::Top ? "top-" : "bottom-") << (left ? "left" : "right")
                      << " contactFrame=" << firstBlockedFrame << " farAdvance=" << farthest - farAtContact
                      << " intermediateFrames=" << intermediateFrames << " outsideAlpha=" << maxOutside
                      << " maskMismatch=" << maxMismatch << std::endl;
            require(firstBlockedFrame >= 0 && farthest - farAtContact >= 40 && intermediateFrames >= 2,
                    "unblocked side continues to grow across multiple frames");
            require(maxOutside == 0, "corner glass and content stay inside work area throughout reveal");
            require(maxMismatch <= 8, "glass and content contours agree beyond antialiasing band");
        }
    }
    window.hide();
}

int main(int argc,char** argv){
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (argc > 1 && std::string(argv[1]) == "--probe-corners") {
            cornerAnimationProbe();
            return 0;
        }
        if(argc>1 && std::string(argv[1])=="--probe-free-hide") {
            liquid_glass::Window probe;
            require(probe.create(),"create free-hide window");
            liquid_glass::Material material; material.animations=true; material.edgeHide=false;
            material.freeHideEnabled=true; material.freeHideDelayMs=500;
            material.blur=0;material.refraction=0;material.dispersion=0;material.rimLight=0;material.surfaceMotion=0;
            probe.setMaterial(material);
            liquid_glass::Image backdrop;backdrop.width=240;backdrop.height=160;backdrop.pixels.resize(240*160*4,255);
            probe.setBackdrop(backdrop);probe.show({200,180,240,160});
            POINT cursor{};GetCursorPos(&cursor);
            auto tick=[&]{MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}probe.render();Sleep(16);};
            SetCursorPos(1000,900);
            const auto started=GetTickCount64();
            while(GetTickCount64()-started<350)tick();
            auto image=probe.snapshot();
            require(channel(image,120,80,3)==255,"free panel remains expanded before delay");
            Sleep(220);
            for(int i=0;i<12;++i)tick();
            image=probe.snapshot();
            const auto visiblePixels=[](const liquid_glass::Image& frame){int count=0;for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x)count+=channel(frame,x,y,3)>0;return count;};
            const int midpoint=visiblePixels(image);
            require(midpoint>0&&midpoint<240*160,"free hide uses an animated intermediate shape");
            for(int i=0;i<60;++i)tick();
            image=probe.snapshot();
            require(visiblePixels(image)<240*160/2,"free hide reaches its centered trigger shape");
            SetCursorPos(320,260);
            for(int i=0;i<15;++i)tick();
            image=probe.snapshot();
            const int wakeMidpoint=visiblePixels(image);
            require(wakeMidpoint>0&&wakeMidpoint<240*160,"free wake uses an animated intermediate shape");
            for(int i=0;i<60;++i)tick();
            image=probe.snapshot();
            require(channel(image,120,80,3)==255,"hovering trigger restores the full panel");
            SetCursorPos(cursor.x,cursor.y);probe.hide();
            std::cout<<"free_hide_window: delay, animated collapse, wake and animated reveal passed\\n";
            return 0;
        }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if(argc>1 && std::string(argv[1])=="--geometry-only") {
            const auto centered=liquid_glass::revealSpan(160,26,80,1);
            require(centered.first==0&&centered.last==160,"center anchor expands to both ends");
            const auto offCenter=liquid_glass::revealSpan(160,26,45,1);
            require(offCenter.first==0&&offCenter.last==160,"off-center anchor expands to both ends");
            const auto hitWall=liquid_glass::revealSpan(160,26,35,1,20,160);
            require(hitWall.first==20&&hitWall.last==160,"work-area boundary clips only the blocked side");
            const auto asymmetric=liquid_glass::revealSpan(160,26,35,.5f,20,160);
            require(asymmetric.first==20&&asymmetric.last>35&&asymmetric.last<160,"unblocked side continues after the other hits the work-area edge");
            const auto collapsed=liquid_glass::revealSpan(160,26,45,0);
            require(collapsed.first==19&&collapsed.last==71,"collapse returns to the saved pointer anchor");
            require(liquid_glass::capsuleHit(100,100,100,100,26),"pointer anchor hits capsule");
            require(!liquid_glass::capsuleHit(100,127,100,100,26),"pointer outside capsule does not hit");
            const auto circle = liquid_glass::panelShape(240, 160, liquid_glass::Edge::Center, 14, 26, 20, 0, 0, 0);
            const auto restored = liquid_glass::panelShape(240, 160, liquid_glass::Edge::Center, 14, 26, 20, 1, 0, 0);
            require(std::abs((circle.right - circle.left) - 52.f) < .01f &&
                    std::abs((circle.bottom - circle.top) - 52.f) < .01f,
                    "free-hide trigger is a complete circle");
            require(restored.left == 1 && restored.top == 1 && restored.right == 239 && restored.bottom == 159,
                    "free-hide reveal restores the full panel rectangle");
            require(std::abs(liquid_glass::animationEase(.5f, 0) - .5f) < .001f &&
                    liquid_glass::animationEase(.5f, 1) > .5f &&
                    liquid_glass::animationEase(.5f, 3) < .5f &&
                    liquid_glass::animationEase(.8f, 4) > 1.f &&
                    std::abs(liquid_glass::animationEase(1.f, 4) - 1.f) < .001f,
                    "animation curves include bounded-endpoint overshoot");
            const auto topCorner=liquid_glass::panelShape(300,180,liquid_glass::Edge::Top,26,26,20,.5f,0,300);
            const auto topRest=liquid_glass::panelShape(300,180,liquid_glass::Edge::Top,26,26,20,0,0,300);
            const auto topFull=liquid_glass::panelShape(300,180,liquid_glass::Edge::Top,26,26,20,1,0,300);
            require(topCorner.left==1&&topCorner.right>topRest.right,"top-edge corner stops at left boundary while right side continues");
            require(topCorner.curve>2&&topRest.curve==2&&topFull.curve==2,"liquid contour curves through the reveal and settles at both ends");
            const auto bottomCorner=liquid_glass::panelShape(300,180,liquid_glass::Edge::Bottom,274,26,20,.5f,0,300);
            require(bottomCorner.right==299&&bottomCorner.left>topCorner.left,"bottom-edge corner clips independently at the right boundary");
            const RECT desktop{0, 0, 2560, 1440};
            const RECT window{600, 500, 900, 700};
            const auto native = liquid_glass::captureMapping(desktop, 2560, 1440, window, 120);
            const auto scaled = liquid_glass::captureMapping(desktop, 1280, 720, window, 120);
            require(native.source.top < native.source.bottom && native.source.bottom <= 820,
                    "capture mapping preserves the lower window region");
            require(std::abs(scaled.scaleX - .5f) < .001f && std::abs(scaled.scaleY - .5f) < .001f,
                    "remote-session texture scales against desktop coordinates");
            require(std::abs(scaled.source.top * 2 - native.source.top) <= 2 &&
                    std::abs(scaled.source.bottom * 2 - native.source.bottom) <= 2,
                    "scaled capture crop tracks both upper and lower regions");
            std::cout<<"edge_reveal_geometry: all checks passed\n";
            std::cout<<"capture_mapping: native and half-resolution upper/lower crops passed\n";
            return 0;
        }
        const auto centered=liquid_glass::revealSpan(160,26,80,1);
        require(centered.first==0&&centered.last==160,"center anchor expands to both ends");
        const auto offCenter=liquid_glass::revealSpan(160,26,45,1);
        require(offCenter.first==0&&offCenter.last==160,"off-center anchor expands to both ends");
        const auto hitWall=liquid_glass::revealSpan(160,26,35,1,20,160);
        require(hitWall.first==20&&hitWall.last==160,"work-area boundary clips only the blocked side");
        const auto asymmetric=liquid_glass::revealSpan(160,26,35,.5f,20,160);
        require(asymmetric.first==20&&asymmetric.last>35&&asymmetric.last<160,"unblocked side continues after the other hits the work-area edge");
        const auto collapsed=liquid_glass::revealSpan(160,26,45,0);
        require(collapsed.first==19&&collapsed.last==71,"collapse returns to the saved pointer anchor");
        if(argc>1 && std::string(argv[1])=="--probe-edge") {
            if(argc<4) throw std::runtime_error("--probe-edge requires side and anchor");
            const std::string side=argv[2];
            const auto edge=side=="left"?liquid_glass::Edge::Left:side=="right"?liquid_glass::Edge::Right:side=="top"?liquid_glass::Edge::Top:liquid_glass::Edge::Bottom;
            const int anchor=std::stoi(argv[3]);
            const int workBoundary=argc>4?std::stoi(argv[4]):-1;
            const bool lowerBoundary=argc>5&&std::string(argv[5])=="lower";
            liquid_glass::Window probe;
            if(!probe.create()) throw std::runtime_error("create edge probe window HRESULT="+std::to_string(static_cast<unsigned long>(probe.lastError())));
            liquid_glass::Material probeMaterial; probeMaterial.animations=true; probeMaterial.blur=0; probeMaterial.refraction=0; probeMaterial.dispersion=0; probeMaterial.rimLight=0; probeMaterial.surfaceMotion=0;
            probe.setMaterial(probeMaterial);
            liquid_glass::Image backdrop; backdrop.width=300; backdrop.height=180; backdrop.pixels.resize(300*180*4,255);
            for(int y=0;y<180;++y)for(int x=0;x<300;++x){const auto i=(y*300+x)*4; backdrop.pixels[i]=static_cast<unsigned char>(x%255);backdrop.pixels[i+1]=static_cast<unsigned char>(y%255);backdrop.pixels[i+2]=90;}
            probe.setBackdrop(backdrop);probe.show({100,100,300,180});
            const bool vertical=edge==liquid_glass::Edge::Left||edge==liquid_glass::Edge::Right;
            const auto geometry=probe.geometry();
            const int workFirst=workBoundary>=0&&!lowerBoundary?geometry.y+workBoundary:geometry.y;
            const int workLast=workBoundary>=0&&lowerBoundary?geometry.y+workBoundary:geometry.y+geometry.height;
            probe.setEdgeCollapsed(true,edge,anchor+(vertical?geometry.y:geometry.x),workFirst,workLast);
            for(int i=0;i<100;++i){probe.render();Sleep(16);}
            probe.render();
            auto image=probe.snapshot();
            const int localAnchor=anchor;
            const int cx=vertical?(edge==liquid_glass::Edge::Left?0:299):localAnchor;
            const int cy=vertical?localAnchor:(edge==liquid_glass::Edge::Top?0:179);
            require(channel(image,cx,cy,3)>90,"capsule alpha at pointer anchor");
            const int first=vertical?cy-28:cx-28, last=vertical?cy+28:cx+28;
            const int atFirst=vertical?channel(image,edge==liquid_glass::Edge::Left?0:299,first,3):channel(image,first,edge==liquid_glass::Edge::Top?0:179,3);
            const int atLast=vertical?channel(image,edge==liquid_glass::Edge::Left?0:299,last,3):channel(image,last,edge==liquid_glass::Edge::Top?0:179,3);
            require(atFirst<40&&atLast<40,"capsule ends stay within pointer diameter");
            const float anchorX=vertical?(edge==liquid_glass::Edge::Left?geometry.x:geometry.x+geometry.width):geometry.x+anchor;
            const float anchorY=vertical?geometry.y+anchor:(edge==liquid_glass::Edge::Top?geometry.y:geometry.y+geometry.height);
            require(probe.hitTestCollapsedCapsule({static_cast<int>(anchorX),static_cast<int>(anchorY)}),"runtime hit test accepts pointer at edge anchor");
            const liquid_glass::Point outside=vertical?liquid_glass::Point{static_cast<int>(anchorX),static_cast<int>(anchorY)+27}:liquid_glass::Point{static_cast<int>(anchorX)+27,static_cast<int>(anchorY)};
            require(!probe.hitTestCollapsedCapsule(outside),"runtime hit test rejects pointer outside capsule radius");
            if(workBoundary>=0 && vertical){
                probe.setEdgeCollapsed(false,edge,-1,workFirst,workLast);
                for(int i=0;i<100;++i){probe.render();Sleep(16);}
                image=probe.snapshot();probe.render();image=probe.snapshot();
                int firstVisible=-1,lastVisible=-1,outsideAlpha=0;
                for(int y=0;y<image.height;++y){const int a=channel(image,cx,y,3);if(a>20){if(firstVisible<0)firstVisible=y;lastVisible=y;}if((lowerBoundary?y>workBoundary:y<workBoundary)&&a>40)outsideAlpha=std::max(outsideAlpha,a);}
                std::cout<<"probe boundary metrics side="<<side<<" anchor="<<anchor<<" interval="<<firstVisible<<".."<<lastVisible<<" outside="<<outsideAlpha<<"\\n";
                if(lowerBoundary) require(firstVisible<anchor,"unblocked upper edge keeps expanding");
                else require(lastVisible>anchor,"unblocked lower edge keeps expanding");
                require(outsideAlpha==0,"expanded capsule stays inside blocked work-area edge");
                std::cout<<"edge expansion "<<side<<" interval="<<firstVisible<<".."<<lastVisible<<" outsideAlpha="<<outsideAlpha<<"\\n";
            }
            std::cout<<"edge probe "<<side<<" anchor="<<anchor<<" centerAlpha="<<channel(image,cx,cy,3)<<" endAlpha="<<atFirst<<","<<atLast<<"\\n";
            return 0;
        }
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
        window.show({80,80,240,38});
        for(auto edge:{liquid_glass::Edge::Left,liquid_glass::Edge::Right}) {
            window.setEdgeCollapsed(true,edge);window.render();
            const auto capsule=window.capsuleSnapshot();
            require(capsule.width==19&&capsule.height==38,"short side constrains side capsule to a semicircle");
            const int screenEdgeX=edge==liquid_glass::Edge::Left?0:capsule.width-1;
            const int arcX=edge==liquid_glass::Edge::Left?capsule.width-1:0;
            require(channel(capsule,arcX,0,3)<10&&channel(capsule,arcX,capsule.height-1,3)<10,"side capsule arc fits short window");
            if(channel(capsule,screenEdgeX,capsule.height/2,3)<=240) std::cerr<<"side edge alpha="<<channel(capsule,screenEdgeX,capsule.height/2,3)<<" edge="<<static_cast<int>(edge)<<" dims="<<capsule.width<<"x"<<capsule.height<<"\\n";
            require(channel(capsule,screenEdgeX,capsule.height/2,3)>240,"side capsule reaches screen edge");
            window.setEdgeCollapsed(false,edge);
        }
        window.hide();
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
            const int innerX=edge==liquid_glass::Edge::Right?capsule.width-1:0;
            const int innerY=edge==liquid_glass::Edge::Bottom?capsule.height-1:0;
            if(edge==liquid_glass::Edge::Left||edge==liquid_glass::Edge::Right)
                require(channel(capsule,innerX,capsule.height/2,3)>240,"half circle meets the screen edge");
            else
                require(channel(capsule,capsule.width/2,innerY,3)>240,"half circle meets the screen edge");
            window.setEdgeCollapsed(false,edge);window.render();
            require(channel(window.snapshot(),120,80,3)==255,"expanded panel restores glass");
        }
        window.hide();
        desktopTest();
        material.edgeHide=true; window.setMaterial(material);window.setClickThrough(true);
        POINT original{};GetCursorPos(&original);
        for(auto edge:{liquid_glass::Edge::Left,liquid_glass::Edge::Right,liquid_glass::Edge::Top,liquid_glass::Edge::Bottom}) {
        window.edgeCollapsedChanged=[&,edge](bool collapsed){window.setEdgeCollapsed(collapsed,edge);return true;};
        std::cout<<"click-through edge="<<static_cast<int>(edge)<<std::endl;
        window.show({80,80,240,160});window.setEdgeCollapsed(true,edge);
        SetCursorPos(edge==liquid_glass::Edge::Left?80:edge==liquid_glass::Edge::Right?319:200,
            edge==liquid_glass::Edge::Top?80:edge==liquid_glass::Edge::Bottom?239:160);settle(window);
        require(!window.edgeCollapsed(),"click-through hover expands half circle");
        SetCursorPos(500,500);settle(window);settle(window);settle(window);settle(window);
        require(window.edgeCollapsed(),"click-through pointer exit collapses panel");
        }
        // Hit testing: a foreign window underneath must get the click back,
        // both while click-through is on and while the panel is collapsed off
        // the edge (its rectangle stays where it was, as the capsule's home).
        window.edgeCollapsedChanged=nullptr;
        window.setMaterial(material);
        window.setClickThrough(false);window.setEdgeCollapsed(false,liquid_glass::Edge::Right);
        window.show({700,400,240,160});
        WNDCLASSW hit{};hit.lpfnWndProc=backgroundProc;hit.hInstance=GetModuleHandleW(nullptr);hit.lpszClassName=L"GlassTestHitTarget";
        RegisterClassW(&hit);
        HWND target=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,hit.lpszClassName,L"",WS_POPUP,
            500,200,800,600,nullptr,nullptr,hit.hInstance,nullptr);
        require(target!=nullptr,"hit test target window");
        SetWindowLongPtrW(target,GWLP_USERDATA,110);
        SetWindowPos(target,HWND_TOPMOST,500,200,800,600,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        SetWindowPos(window.hwnd(),HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        DwmFlush();
        auto exStyle=[&]{return GetWindowLongPtrW(window.hwnd(),GWL_EXSTYLE);};
        auto through=[&]{return (exStyle()&WS_EX_TRANSPARENT)!=0;};
        require((exStyle()&WS_EX_LAYERED)!=0,"window is layered");
        require(!through(),"shown panel takes the mouse");
        require(WindowFromPoint(POINT{820,480})==window.hwnd(),"shown panel owns the point");
        window.setClickThrough(true);
        require(through(),"click-through leaves hit testing");
        require(WindowFromPoint(POINT{820,480})==target,"click-through reaches the window underneath");
        window.setClickThrough(false);
        window.setEdgeCollapsed(true,liquid_glass::Edge::Right);
        require(through(),"collapsed panel leaves hit testing");
        require(WindowFromPoint(POINT{820,480})==target,"collapsed panel's old area reaches the window underneath");
        window.render();
        require(channel(window.snapshot(),120,80,3)==0,"collapsed panel is still drawn transparent");
        window.setEdgeCollapsed(false,liquid_glass::Edge::Right);
        require(!through(),"expanded panel takes the mouse again");
        window.hide();DestroyWindow(target);
        SetCursorPos(original.x,original.y);
        std::cout<<"liquid_glass_render: all checks passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
