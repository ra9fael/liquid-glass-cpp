#include <liquid_glass/window.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

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
        window.hide();
        std::cout<<"liquid_glass_render: all checks passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
