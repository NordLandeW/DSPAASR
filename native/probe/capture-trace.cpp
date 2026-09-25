#include "present/capture-trace.h"
#include <d3d11sdklayers.h>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Image {ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> view;};
Image make(ID3D11Device* device,DXGI_FORMAT format,DXGI_FORMAT viewFormat,const void* bytes,unsigned row){
    D3D11_TEXTURE2D_DESC description{};description.Width=4;description.Height=2;description.MipLevels=1;description.ArraySize=1;
    description.Format=format;description.SampleDesc.Count=1;description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data{bytes,row,0};Image image;
    dspaa::graphicsCheck(device->CreateTexture2D(&description,&data,&image.texture),"Create diagnostic fixture image");
    D3D11_RENDER_TARGET_VIEW_DESC view{};view.Format=viewFormat;view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    dspaa::graphicsCheck(device->CreateRenderTargetView(image.texture.Get(),&view,&image.view),"Create diagnostic fixture view");return image;
}
}
int main(){
    try{
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_1,actual{};
        dspaa::graphicsCheck(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,&requested,1,
            D3D11_SDK_VERSION,&device,&actual,&context),"Create windowless diagnostic fixture");
        auto graphics=dspaa::acquireDx11Dx12(device.Get());
        ComPtr<ID3D11InfoQueue> debug;dspaa::graphicsCheck(device.As(&debug),"Query diagnostic debug layer");
        std::vector<std::string> reports;
        dspaa::CaptureTrace trace(graphics,[&](const char* text){reports.emplace_back(text);std::cout<<text<<'\n';});
        std::array<unsigned char,32> rgba;rgba.fill(255);
        auto source=make(device.Get(),DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,rgba.data(),16);
        auto destination=make(device.Get(),DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,rgba.data(),16);
        trace.observe(1,1,103,source.view.Get());trace.observe(1,1,200,destination.view.Get());graphics->drain();trace.collect();
        require(reports.size()==1&&reports.back().find("rgbaQuantizedEqual=8 ")!=std::string::npos,"Diagnostic lost exact identical images");
        rgba[0]=rgba[1]=rgba[2]=0;context->UpdateSubresource(destination.texture.Get(),0,nullptr,rgba.data(),16,0);
        trace.observe(2,1,103,source.view.Get());trace.observe(2,1,200,destination.view.Get());graphics->drain();trace.collect();
        require(reports.size()==2&&reports.back().find("rgbQuantizedEqual=7 ")!=std::string::npos&&
            reports.back().find("maxLinearRgb=1 ")!=std::string::npos,"Diagnostic failed to detect a changed GPU pixel");
        for(unsigned i=0;i<8;++i){rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=128;rgba[i*4+3]=255;}
        auto encoded=make(device.Get(),DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,rgba.data(),16);
        // IEC sRGB code 128 is approximately .2158605 linear, whose nearest
        // binary16 value is .2158203125 (0x32e8), not the raw .50196 code value.
        std::array<uint16_t,32> linear;
        for(unsigned i=0;i<8;++i){linear[i*4]=linear[i*4+1]=linear[i*4+2]=0x32e8;linear[i*4+3]=0x3c00;}
        auto hdr=make(device.Get(),DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R16G16B16A16_FLOAT,linear.data(),32);
        trace.observe(3,1,103,encoded.view.Get());trace.observe(3,1,201,hdr.view.Get());graphics->drain();trace.collect();
        require(reports.size()==3&&reports.back().find("rgbaQuantizedEqual=8 ")!=std::string::npos,"Diagnostic confused sRGB storage with linear HDR");
        // A real asymmetric GPU image must distinguish row inversion from a
        // same-coordinate color mismatch, not just report a low average error.
        std::array<unsigned char,32> inverted{};
        for(unsigned i=0;i<8;++i){rgba[i*4]=i<4?255:0;rgba[i*4+1]=i<4?0:255;rgba[i*4+2]=0;rgba[i*4+3]=255;}
        std::copy_n(rgba.begin()+16,16,inverted.begin());std::copy_n(rgba.begin(),16,inverted.begin()+16);
        context->UpdateSubresource(source.texture.Get(),0,nullptr,rgba.data(),16,0);
        context->UpdateSubresource(destination.texture.Get(),0,nullptr,inverted.data(),16,0);
        trace.observe(4,1,103,source.view.Get());trace.observe(4,1,201,destination.view.Get());graphics->drain();trace.collect();
        require(reports.size()==4 && reports.back().find("rgbQuantizedEqual=0 ")!=std::string::npos &&
            reports.back().find("flipLinearEqual=8 ")!=std::string::npos,"Diagnostic missed a vertical color-buffer flip");
        // A half-float holding the raw encoded gray is NOT the decoded .2158
        // value from the preceding case. Report that candidate separately.
        for(unsigned i=0;i<8;++i)linear[i*4]=linear[i*4+1]=linear[i*4+2]=0x3804;
        context->UpdateSubresource(hdr.texture.Get(),0,nullptr,linear.data(),32,0);
        trace.observe(5,1,103,encoded.view.Get());trace.observe(5,1,201,hdr.view.Get());graphics->drain();trace.collect();
        require(reports.size()==5 && reports.back().find("rgbQuantizedEqual=0 ")!=std::string::npos &&
            reports.back().find("storageEqual=8 ")!=std::string::npos,"Diagnostic conflated raw storage and linear interpretation");
        trace.observe(6,1,103,source.view.Get());trace.observe(7,1,200,destination.view.Get());graphics->drain();trace.collect();
        require(reports.size()==5,"Diagnostic compared different application frames");
        unsigned errors=0;
        for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
            SIZE_T count=0;debug->GetMessage(i,nullptr,&count);std::vector<unsigned char> bytes(count);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());debug->GetMessage(i,message,&count);
            if(message->Severity==D3D11_MESSAGE_SEVERITY_ERROR||message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION){++errors;std::cerr<<message->pDescription<<'\n';}
        }
        require(errors==0,"Diagnostic produced D3D11 debug errors");
        std::cout<<"{\"capture_trace_probe\":\"passed\",\"comparisons\":5,\"d3d11_errors\":0,\"window_created\":false}\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
