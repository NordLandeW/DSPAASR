#include "scene-depth.h"
#include "capture/gpu.h"
#include "capture/hooks.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char program[] = R"(
Texture2D<float> sceneDepth : register(t0);
cbuffer Coordinates : register(b0) { float2 inverseDestination; float2 shift; };
float4 vertex(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
float pixel(float4 position : SV_Position) : SV_Depth {
    uint w,h; sceneDepth.GetDimensions(w,h);
    float2 uv = position.xy * inverseDestination + shift;
    int2 at = clamp(int2(floor(uv * float2(w,h))), int2(0,0), int2(w,h)-1);
    return sceneDepth.Load(int3(at,0));
}
)";
ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> bytes, errors;
    const auto result=D3DCompile(program,sizeof(program)-1,"DSPAASR scene depth",nullptr,nullptr,entry,profile,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytes,&errors);
    if(FAILED(result))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Compile scene depth helper");
    return bytes;
}
DXGI_FORMAT sampledDepthFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: throw std::invalid_argument("Scene-depth input needs a verified 32-bit float depth view");
    }
}
DXGI_FORMAT depthFormat(DXGI_FORMAT format) {
    switch(format){
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_D16_UNORM;
    default:throw std::invalid_argument("World UI depth attachment has an unsupported device format");
    }
}
}
struct SceneDepth::Impl {
    std::shared_ptr<Dx11Dx12> graphics;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> writeDepth;
    ComPtr<ID3D11Texture2D> input,output;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    ComPtr<ID3D11DepthStencilView> destinationView;
    explicit Impl(std::shared_ptr<Dx11Dx12> owner):graphics(std::move(owner)) {
        if(!graphics)throw std::invalid_argument("Scene depth needs the shared graphics owner");
        auto lock=graphics->lock();capture::Bypass bypass;
        auto vs=compile("vertex","vs_5_0"),ps=compile("pixel","ps_5_0");auto* device=graphics->device11();
        graphicsCheck(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"Create scene-depth vertex shader");
        graphicsCheck(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel),"Create scene-depth pixel shader");
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=16;buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        graphicsCheck(device->CreateBuffer(&buffer,nullptr,&constants),"Create scene-depth coordinates");
        D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
        graphicsCheck(device->CreateRasterizerState(&rasterDesc,&raster),"Create scene-depth raster state");
        D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
        graphicsCheck(device->CreateDepthStencilState(&depth,&writeDepth),"Create scene-depth write state");
    }
};
SceneDepth::SceneDepth(std::shared_ptr<Dx11Dx12> graphics):impl_(std::make_unique<Impl>(std::move(graphics))){}
SceneDepth::~SceneDepth()=default;
void SceneDepth::copy(ID3D11Texture2D* source,ID3D11Texture2D* destination,float shiftX,float shiftY) {
    if(!source||!destination||source==destination||!std::isfinite(shiftX)||!std::isfinite(shiftY))
        throw std::invalid_argument("Invalid scene-depth copy envelope");
    auto& s=*impl_;auto lock=s.graphics->lock();capture::Bypass bypass;auto* device=s.graphics->device11();auto* context=s.graphics->context11();
    D3D11_TEXTURE2D_DESC a{},b{};source->GetDesc(&a);destination->GetDesc(&b);
    if(!capture::Gpu::supported(source)||!capture::Gpu::supported(destination)||
        !(a.BindFlags&D3D11_BIND_SHADER_RESOURCE)||!(b.BindFlags&D3D11_BIND_DEPTH_STENCIL))
        throw std::invalid_argument("Scene-depth copy requires a sampleable raw depth input and a single-sample depth attachment");
    const auto inputFormat = sampledDepthFormat(a.Format);
    ComPtr<ID3D11Device> sourceDevice,destinationDevice;source->GetDevice(&sourceDevice);destination->GetDevice(&destinationDevice);
    if(sourceDevice.Get()!=device||destinationDevice.Get()!=device)throw std::invalid_argument("Scene-depth resources use another device");
    if(s.input.Get()!=source){
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=inputFormat;desc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;desc.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> view;graphicsCheck(device->CreateShaderResourceView(source,&desc,&view),"Create raw scene-depth input view");
        s.sourceView=std::move(view);s.input=source;
    }
    if(s.output.Get()!=destination){
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};desc.Format=depthFormat(b.Format);desc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> view;graphicsCheck(device->CreateDepthStencilView(destination,&desc,&view),"Create world UI depth attachment view");
        s.destinationView=std::move(view);s.output=destination;
    }
    capture::StateGuard saved(context);
    if(!saved.pureRaster)throw std::runtime_error("Scene-depth helper refuses UAV, SO or predication side effects");
    const float coordinates[]{1.f/b.Width,1.f/b.Height,shiftX,shiftY};context->UpdateSubresource(s.constants.Get(),0,nullptr,coordinates,0,0);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s.vertex.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
    context->PSSetShader(s.pixel.Get(),nullptr,0);auto* constants=s.constants.Get();context->PSSetConstantBuffers(0,1,&constants);
    context->OMSetRenderTargets(0,nullptr,s.destinationView.Get());context->OMSetDepthStencilState(s.writeDepth.Get(),0);
    const float factors[4]{};context->OMSetBlendState(nullptr,factors,UINT_MAX);context->RSSetState(s.raster.Get());
    const D3D11_VIEWPORT viewport{0,0,static_cast<float>(b.Width),static_cast<float>(b.Height),0,1};context->RSSetViewports(1,&viewport);
    auto* view=s.sourceView.Get();context->PSSetShaderResources(0,1,&view);
    if(b.Format==DXGI_FORMAT_R24G8_TYPELESS||b.Format==DXGI_FORMAT_D24_UNORM_S8_UINT||
        b.Format==DXGI_FORMAT_R32G8X24_TYPELESS||b.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        context->ClearDepthStencilView(s.destinationView.Get(),D3D11_CLEAR_STENCIL,0,0);
    context->Draw(3,0);
}
} // namespace dspaa
