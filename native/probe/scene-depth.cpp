#include "present/scene-depth.h"
#include <d3d11sdklayers.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace dspaa;
using Microsoft::WRL::ComPtr;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main(){
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        graphicsCheck(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,
            levels,2,D3D11_SDK_VERSION,&device,&level,&context),"Create scene-depth probe device");
        auto graphics=acquireDx11Dx12(device.Get());SceneDepth copy(graphics);
        std::array<float,16> samples{};for(unsigned i=0;i<16;++i)samples[i]=i/16.f;
        D3D11_TEXTURE2D_DESC sourceDesc{};sourceDesc.Width=sourceDesc.Height=4;sourceDesc.MipLevels=sourceDesc.ArraySize=1;
        sourceDesc.Format=DXGI_FORMAT_R32_FLOAT;sourceDesc.SampleDesc.Count=1;sourceDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{samples.data(),4*sizeof(float),0};ComPtr<ID3D11Texture2D> source;
        graphicsCheck(device->CreateTexture2D(&sourceDesc,&data,&source),"Create raw probe depth");
        auto outputDesc=sourceDesc;outputDesc.Width=8;outputDesc.Height=6;outputDesc.Format=DXGI_FORMAT_R32_TYPELESS;
        outputDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> output;
        graphicsCheck(device->CreateTexture2D(&outputDesc,nullptr,&output),"Create probe D32 attachment");
        auto stagingDesc=outputDesc;stagingDesc.BindFlags=0;stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;graphicsCheck(device->CreateTexture2D(&stagingDesc,nullptr,&staging),"Create probe depth readback");
        const unsigned columns[2][8]{{0,0,1,1,2,2,3,3},{0,1,1,2,2,3,3,3}};
        const unsigned rows[2][6]{{0,1,1,2,3,3},{0,0,0,1,2,2}};
        // Real Unity r8 used a D32S8 attachment, not a standalone RFloat
        // texture. Initialize both depth families independently of the helper;
        // two compensating copy/orientation errors must not make this pass.
        struct DepthStencil { float depth; unsigned stencil; };
        static_assert(sizeof(DepthStencil)==8);
        std::array<DepthStencil,16> packed{};
        for(unsigned i=0;i<16;++i) packed[i]={samples[i],0x5au};
        std::array<ComPtr<ID3D11Texture2D>,3> sources{source,{}, {}};
        std::array<ComPtr<ID3D11DepthStencilView>,3> views{};
        for(unsigned kind=1;kind<3;++kind){
            auto desc=sourceDesc;desc.BindFlags|=D3D11_BIND_DEPTH_STENCIL;
            desc.Format=kind==1?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32G8X24_TYPELESS;
            D3D11_SUBRESOURCE_DATA initial{kind==1?static_cast<const void*>(samples.data()):static_cast<const void*>(packed.data()),
                static_cast<UINT>(4*(kind==1?sizeof(float):sizeof(DepthStencil))),0};
            graphicsCheck(device->CreateTexture2D(&desc,&initial,&sources[kind]),"Create initialized raw attachment");
            D3D11_DEPTH_STENCIL_VIEW_DESC view{};view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            view.Format=kind==1?DXGI_FORMAT_D32_FLOAT:DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            graphicsCheck(device->CreateDepthStencilView(sources[kind].Get(),&view,&views[kind]),"Create original depth view");
        }
        for(unsigned kind=0;kind<sources.size();++kind){
            context->OMSetRenderTargets(0,nullptr,views[kind].Get());
            for(unsigned pass=0;pass<2;++pass){
                copy.copy(sources[kind].Get(),output.Get(),pass?.125f:0.f,pass?-.25f:0.f);
                ComPtr<ID3D11DepthStencilView> restored;context->OMGetRenderTargets(0,nullptr,&restored);
                require(restored.Get()==views[kind].Get(),"Depth helper did not restore the original attachment");
                context->CopyResource(staging.Get(),output.Get());graphics->drain();
                D3D11_MAPPED_SUBRESOURCE mapped{};graphicsCheck(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read resampled probe depth");
                bool good=true;
                for(unsigned y=0;y<6;++y){const auto* row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch);
                    for(unsigned x=0;x<8;++x)good=good&&row[x]==samples[rows[pass][y]*4+columns[pass][x]];
                }
                context->Unmap(staging.Get(),0);require(good,"Depth resampling lost point values, clamped jitter, or vertical orientation");
            }
        }
        auto rawReadDesc=sourceDesc;rawReadDesc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
        rawReadDesc.BindFlags=0;rawReadDesc.Usage=D3D11_USAGE_STAGING;rawReadDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> rawRead;
        graphicsCheck(device->CreateTexture2D(&rawReadDesc,nullptr,&rawRead),"Create original attachment readback");
        context->CopyResource(rawRead.Get(),sources[2].Get());graphics->drain();
        D3D11_MAPPED_SUBRESOURCE original{};
        graphicsCheck(context->Map(rawRead.Get(),0,D3D11_MAP_READ,0,&original),"Read original D32S8 attachment");
        bool unchanged=true;
        for(unsigned y=0;y<4;++y){const auto* row=reinterpret_cast<const DepthStencil*>(static_cast<const unsigned char*>(original.pData)+y*original.RowPitch);
            for(unsigned x=0;x<4;++x)unchanged=unchanged&&row[x].depth==samples[y*4+x]&&(row[x].stencil&0xffu)==0x5au;
        }
        context->Unmap(rawRead.Get(),0);require(unchanged,"Resampling changed source depth or stencil");
        ComPtr<ID3D11Predicate> predicate;D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE,0};
        graphicsCheck(device->CreatePredicate(&query,&predicate),"Create application predicate");
        context->Begin(predicate.Get());context->End(predicate.Get());context->SetPredication(predicate.Get(),TRUE);
        bool refused=false;try{copy.copy(sources[2].Get(),output.Get(),0,0);}catch(const std::runtime_error&){refused=true;}
        ComPtr<ID3D11Predicate> restoredPredicate;BOOL predicateValue=FALSE;context->GetPredication(&restoredPredicate,&predicateValue);
        require(refused&&restoredPredicate.Get()==predicate.Get()&&predicateValue==TRUE,"Raw attachment support bypassed or changed application predication");
        context->SetPredication(nullptr,FALSE);context->OMSetRenderTargets(0,nullptr,nullptr);
        ComPtr<ID3D11InfoQueue> debug;graphicsCheck(device.As(&debug),"Read depth debug messages");unsigned errors=0;
        for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
            SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());debug->GetMessage(i,message,&size);
            if(message->Severity==D3D11_MESSAGE_SEVERITY_ERROR||message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION){++errors;std::cerr<<message->pDescription<<'\n';}
        }
        require(!errors,"Scene-depth copy caused D3D11 errors");
        std::cout<<"{\"scene_depth_probe\":\"passed\",\"samples_checked\":288,\"raw_d32s8_unchanged\":true,\"predicate_preserved\":true,\"d3d11_errors\":0,\"window_created\":false}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
