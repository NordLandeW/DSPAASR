#include "capture/gpu.h"
#include "capture/owner.h"
#include "ui-proof/blit.h"
#include "ui-proof/shadow.h"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned width = 32, height = 16;
const std::array<float, 4> world{0.2f, 0.3f, 0.4f, 1.f};
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void close(float actual, float expected, const char* reason, float tolerance = 0.00001f) {
    if (!std::isfinite(actual) || std::abs(actual-expected) > tolerance)
        throw std::runtime_error(std::string(reason)+" actual="+std::to_string(actual)+" expected="+std::to_string(expected));
}
void sameValue(float actual, float expected, const char* reason) {
    if (std::isnan(expected)) require(std::isnan(actual), reason);
    else if (std::isinf(expected)) require(actual == expected, reason);
    else close(actual, expected, reason);
}
const char shader[] = R"(
cbuffer Fixture : register(b0) { float4 color; float4 bounds; float4 extent; };
Texture2D<float4> image : register(t0);
// One RTV occupies slot zero; the fixture's OM UAV starts immediately after it.
RWTexture2D<float4> writtenImage : register(u1);
SamplerState nearestSampler : register(s0);
float4 vertex(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
struct ClipInput { float3 position : POSITION; float2 uv : TEXCOORD; };
struct ClipOutput { float4 position : SV_Position; float2 uv : TEXCOORD; };
ClipOutput clipVertex(ClipInput value) {
    ClipOutput result; result.position=float4(value.position,1); result.uv=value.uv; return result;
}
float4 clipPixel(ClipOutput value) : SV_Target { return image.Sample(nearestSampler,value.uv); }
float4 solid(float4 p : SV_Position) : SV_Target {
    if (any(p.xy < bounds.xy) || any(p.xy >= bounds.zw)) discard;
    return color;
}
float4 writeUav(float4 p : SV_Position) : SV_Target {
    float4 value = solid(p);
    writtenImage[uint2(p.xy)] = value;
    return value;
}
float4 overlapVertex(uint id : SV_VertexID) : SV_Position {
    uint corner = id % 3;
    return float4(corner == 2 ? 3 : -1, corner == 1 ? 3 : -1, id < 3 ? 0.25 : 0.5, 1);
}
float4 overlapSolid(float4 p : SV_Position, uint primitive : SV_PrimitiveID) : SV_Target {
    float4 value = solid(p);
    return primitive == (uint)extent.z ? value : 0;
}
[earlydepthstencil]
float4 earlyOverlapSolid(float4 p : SV_Position, uint primitive : SV_PrimitiveID) : SV_Target {
    // The first primitive can update D/S and then discard. Disabling writes in
    // its replay wrongly lets the second primitive survive into the footprint.
    if (primitive == (uint)extent.z) discard;
    return solid(p);
}
float4 filter(float4 p : SV_Position) : SV_Target {
    float2 uv = p.xy/extent.xy;
    return (image.SampleLevel(nearestSampler, uv-float2(1/extent.x,0),0) +
            image.SampleLevel(nearestSampler, uv,0) +
            image.SampleLevel(nearestSampler, uv+float2(1/extent.x,0),0))/3;
}
)";
ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(shader, sizeof(shader)-1, "UI capture behavioral fixture", nullptr, nullptr, entry, profile,
                              D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    dspaa::graphicsCheck(hr, "Compile UI capture fixture"); return code;
}
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    dspaa::CaptureTexture captured(uint64_t epoch = 1) const { return {texture, epoch}; }
};
struct Pixels {
    std::vector<float> values;
    unsigned channels = 0;
    float at(unsigned x, unsigned y, unsigned channel = 0) const { return values[(y*width+x)*channels+channel]; }
};
class Fixture {
  public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11InfoQueue> debug;
    std::shared_ptr<dspaa::Dx11Dx12> graphics;
    std::unique_ptr<dspaa::CaptureOwner> capture;
    ComPtr<ID3D11VertexShader> vertex, overlapVertex;
    ComPtr<ID3D11PixelShader> solid, filter, overlapSolid, earlyOverlapSolid;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> noDepth, push, testOne, testTwo;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    Texture full, post, sentinel;
    bool nonnegative = true, signedWithoutOverlap = false, verified = true, finiteRgb = true;
    bool conservativeFragments = false;
    uint64_t nextFrame = 0;
    unsigned checkedFrames = 0;
    unsigned diagnosticCalls = 0;
    decltype(dspaa::CaptureOwnerCreateInfo::effectShaderPolicy) specialEffect;

    Fixture() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11DeviceContext> immediate;
        dspaa::graphicsCheck(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                              levels, 2, D3D11_SDK_VERSION, &device, nullptr, &immediate), "Create debug D3D11 fixture device");
        dspaa::graphicsCheck(immediate.As(&context), "Get fixture context4");
        dspaa::graphicsCheck(device.As(&debug), "Get D3D11 debug queue");
        const auto initialized = MH_Initialize();
        require(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED, "Initialize fixture hook host");
        graphics = dspaa::acquireDx11Dx12(device.Get());
        auto code = compile("vertex", "vs_5_0");
        dspaa::graphicsCheck(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex), "Create fixture VS");
        code = compile("solid", "ps_5_0");
        dspaa::graphicsCheck(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &solid), "Create fixture solid PS");
        code = compile("filter", "ps_5_0");
        dspaa::graphicsCheck(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &filter), "Create fixture filter PS");
        code = compile("overlapVertex", "vs_5_0");
        dspaa::graphicsCheck(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &overlapVertex), "Create overlapping fixture VS");
        code = compile("overlapSolid", "ps_5_0");
        dspaa::graphicsCheck(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &overlapSolid), "Create overlapping fixture PS");
        code = compile("earlyOverlapSolid", "ps_5_0");
        dspaa::graphicsCheck(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &earlyOverlapSolid), "Create early-discard overlap fixture PS");
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 512; buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dspaa::graphicsCheck(device->CreateBuffer(&buffer, nullptr, &constants), "Create ranged fixture CB");
        D3D11_SAMPLER_DESC sample{}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxAnisotropy = 1; sample.ComparisonFunc = D3D11_COMPARISON_NEVER; sample.MaxLOD = D3D11_FLOAT32_MAX;
        dspaa::graphicsCheck(device->CreateSamplerState(&sample, &sampler), "Create fixture sampler");
        D3D11_RASTERIZER_DESC rasterDescription{}; rasterDescription.FillMode = D3D11_FILL_SOLID;
        rasterDescription.CullMode = D3D11_CULL_NONE; rasterDescription.DepthClipEnable = TRUE; rasterDescription.ScissorEnable = TRUE;
        dspaa::graphicsCheck(device->CreateRasterizerState(&rasterDescription, &raster), "Create fixture raster");
        D3D11_DEPTH_STENCIL_DESC stencil{}; stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
        stencil.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        stencil.BackFace = stencil.FrontFace;
        dspaa::graphicsCheck(device->CreateDepthStencilState(&stencil, &noDepth), "Create disabled fixture depth");
        stencil.StencilEnable = TRUE; stencil.StencilReadMask = stencil.StencilWriteMask = 255;
        stencil.FrontFace.StencilPassOp = stencil.BackFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
        dspaa::graphicsCheck(device->CreateDepthStencilState(&stencil, &push), "Create fixture stencil increment");
        stencil.FrontFace.StencilFunc = stencil.BackFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        stencil.FrontFace.StencilPassOp = stencil.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        stencil.StencilWriteMask = 0;
        dspaa::graphicsCheck(device->CreateDepthStencilState(&stencil, &testOne), "Create fixture stencil test"); testTwo = testOne;
        full = make(); post = make(); sentinel = make();
        D3D11_TEXTURE2D_DESC depthDescription{}; full.texture->GetDesc(&depthDescription);
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dspaa::graphicsCheck(device->CreateTexture2D(&depthDescription, nullptr, &depth), "Create fixture depth/stencil");
        dspaa::graphicsCheck(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "Create fixture DSV");
        dspaa::CaptureOwnerCreateInfo info; info.graphics = graphics; info.maximumInFlightFrames = 2; info.maximumFrameBytes = 64ull*1024*1024;
        info.uiShaderPolicy = [this](ID3D11PixelShader* actualShader) {
            // This fixture owns the actual PS programs and controls their alpha.
            // Nonfinite cases explicitly remove finite-RGB proof and opt in.
            // Only the single-triangle shader receives a no-self-overlap proof.
            return dspaa::CaptureUiShaderPolicy{verified && (actualShader == solid.Get() || actualShader == overlapSolid.Get() || actualShader == earlyOverlapSolid.Get()),
                                                nonnegative, signedWithoutOverlap && actualShader == solid.Get(), finiteRgb, conservativeFragments};
        };
        info.effectShaderPolicy = [this](ID3D11PixelShader* actual, const dspaa::CaptureScope& declared,
                                         const dspaa::capture::DrawArguments& arguments,
                                         dspaa::CaptureSupport& support) {
            if (specialEffect)
                return specialEffect(actual, declared, arguments, support);
            // This fixture owns the exact three-tap/constant PS programs.
            // A different actual PS must not inherit the declared pass's proof.
            if (!((declared.passId == 2 && actual == filter.Get()) ||
                  (declared.passId == 3 && actual == solid.Get())))
                return false;
            support = declared.support;
            return true;
        };
        info.unannotatedDraw = [this](ID3D11RenderTargetView*) {
            ++diagnosticCalls;
            throw std::runtime_error("Fixture diagnostic failure must not change application drawing");
        };
        capture = std::make_unique<dspaa::CaptureOwner>(info);
    }
    ~Fixture() {
        if (capture) capture->stop();
        try { if (debug) errors(); } catch (...) {}
        if (context) context->ClearState();
    }
    Texture make(UINT additionalBindings = 0) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | additionalBindings;
        Texture result;
        dspaa::graphicsCheck(device->CreateTexture2D(&description, nullptr, &result.texture),
                             "Create fixture color");
        dspaa::graphicsCheck(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv),
                             "Create fixture RTV");
        dspaa::graphicsCheck(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv),
                             "Create fixture SRV");
        return result;
    }
    void bind(const Texture& target, bool withDepth = false) {
        ID3D11ShaderResourceView* empty = nullptr; context->PSSetShaderResources(0, 1, &empty);
        auto* rtv = target.rtv.Get(); context->OMSetRenderTargets(1, &rtv, withDepth ? dsv.Get() : nullptr);
        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        const D3D11_RECT scissor{0,0,width,height}; context->RSSetViewports(1,&viewport); context->RSSetScissorRects(1,&scissor); context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0); context->PSSetShader(solid.Get(),nullptr,0);
        context->OMSetDepthStencilState(noDepth.Get(),0);
        auto* cb = constants.Get(); const UINT first = 16, count = 16; context->PSSetConstantBuffers1(0,1,&cb,&first,&count);
        auto* sample = sampler.Get(); context->PSSetSamplers(0,1,&sample);
        auto* retained = sentinel.srv.Get(); context->VSSetShaderResources(7,1,&retained); context->PSSetShaderResources(23,1,&retained); context->CSSetShaderResources(11,1,&retained);
    }
    void parameters(std::array<float,4> color, std::array<float,4> bounds = {0,0,width,height}, float selectedPrimitive = 0) {
        std::array<float,128> data{};
        // Distinct zero prefix detects restoring b0 without its 256-byte offset.
        std::copy(color.begin(),color.end(),data.begin()+64); std::copy(bounds.begin(),bounds.end(),data.begin()+68);
        data[72] = width; data[73] = height; data[74] = selectedPrimitive; context->UpdateSubresource(constants.Get(),0,nullptr,data.data(),0,0);
    }
    void blend(D3D11_BLEND source, D3D11_BLEND destination, UINT8 write = D3D11_COLOR_WRITE_ENABLE_ALL, bool enabled = true, UINT sampleMask = UINT_MAX) {
        D3D11_BLEND_DESC description{}; auto& target = description.RenderTarget[0];
        target.BlendEnable = enabled; target.SrcBlend = target.SrcBlendAlpha = source;
        target.DestBlend = target.DestBlendAlpha = destination; target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD; target.RenderTargetWriteMask = write;
        ComPtr<ID3D11BlendState> state; dspaa::graphicsCheck(device->CreateBlendState(&description,&state),"Create fixture blend");
        context->OMSetBlendState(state.Get(),nullptr,sampleMask);
    }
    void bindings(const Texture& target, ID3D11PixelShader* expected, bool withDepth = false) {
        ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11DepthStencilView> depthView;
        context->OMGetRenderTargets(1,&rtv,&depthView);
        require(rtv.Get()==target.rtv.Get() && depthView.Get()==(withDepth?dsv.Get():nullptr),"Capture did not restore original OM views");
        ComPtr<ID3D11PixelShader> pixel; context->PSGetShader(&pixel,nullptr,nullptr); require(pixel.Get()==expected,"Capture did not restore original PS");
        ComPtr<ID3D11Buffer> cb; UINT first=0,count=0; context->PSGetConstantBuffers1(0,1,&cb,&first,&count);
        require(cb.Get()==constants.Get() && first==16 && count==16,"Capture lost b0 range binding");
        ComPtr<ID3D11ShaderResourceView> view;
        context->VSGetShaderResources(7,1,&view); require(view.Get()==sentinel.srv.Get(),"Capture lost VS SRV"); view.Reset();
        context->PSGetShaderResources(23,1,&view); require(view.Get()==sentinel.srv.Get(),"Capture lost PS SRV"); view.Reset();
        context->CSGetShaderResources(11,1,&view); require(view.Get()==sentinel.srv.Get(),"Capture lost CS SRV");
    }
    void begin(bool withDepth = false) {
        bind(full,withDepth); context->ClearRenderTargetView(full.rtv.Get(),world.data());
        if (withDepth) context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        require(capture->beginFrame({++nextFrame,1,width,height}),"Begin fixture frame");
        require(capture->seedCleanRoot(full.captured()),"Seed fixture clean root");
    }
    void uiScope() { dspaa::CaptureScope scope; scope.kind=dspaa::CaptureScopeKind::FullOnlyUiCoverage; scope.passId=1; require(capture->beginScope(scope),"Begin UI fixture scope"); }
    void draw(std::array<float,4> color, std::array<float,4> bounds = {0,0,width,height}) { parameters(color,bounds); context->Draw(3,0); }
    void end() { require(capture->endScope(),"End fixture scope"); }
    dspaa::CapturedUiLease seal(const Texture& target, uint64_t epoch = 1) {
        auto lease = capture->seal(target.captured(epoch)); require(static_cast<bool>(lease),capture->status().reason.c_str());
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"Create fixture completion event");
        const auto hr=lease->readyFence->SetEventOnCompletion(lease->readyValue,event);
        if (FAILED(hr)) { CloseHandle(event); dspaa::graphicsCheck(hr,"Arm fixture completion"); }
        context->Flush(); const auto done=WaitForSingleObject(event,30000); CloseHandle(event);
        require(done==WAIT_OBJECT_0,"Fixture capture fence did not complete");
        const auto completed=lease->readyFence->GetCompletedValue();
        require(completed!=UINT64_MAX && completed>=lease->readyValue,"Fixture fence event woke without GPU completion");
        ++checkedFrames; return lease;
    }
    Pixels read(ID3D11Texture2D* input) {
        require(input!=nullptr,"Missing captured plane");
        D3D11_TEXTURE2D_DESC description{}; input->GetDesc(&description);
        require(description.Width==width && description.Height==height && description.MipLevels==1 && description.ArraySize==1 && description.SampleDesc.Count==1,
                "Captured plane has unexpected dimensions/subresources");
        require(description.Format==DXGI_FORMAT_R32G32B32A32_FLOAT || description.Format==DXGI_FORMAT_R32_FLOAT || description.Format==DXGI_FORMAT_R16_FLOAT,
                "Captured plane has unexpected format");
        const auto format=description.Format; description.Usage=D3D11_USAGE_STAGING; description.BindFlags=0; description.CPUAccessFlags=D3D11_CPU_ACCESS_READ; description.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging; dspaa::graphicsCheck(device->CreateTexture2D(&description,nullptr,&staging),"Create fixture readback");
        context->CopyResource(staging.Get(),input); D3D11_MAPPED_SUBRESOURCE mapped{};
        dspaa::graphicsCheck(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read captured fixture output");
        Pixels result; result.channels=format==DXGI_FORMAT_R32G32B32A32_FLOAT?4:1; result.values.resize(width*height*result.channels);
        for (unsigned y=0;y<height;++y) {
            const auto* row=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
            if (format==DXGI_FORMAT_R16_FLOAT) {
                for (unsigned x=0;x<width;++x) {
                    uint16_t half; std::memcpy(&half,row+2*x,2);
                    if (half!=0 && half!=0x3c00) { context->Unmap(staging.Get(),0); throw std::runtime_error("Influence is not a binary support plane"); }
                    result.values[y*width+x]=half?1.f:0.f;
                }
            } else std::memcpy(result.values.data()+y*width*result.channels,row,width*result.channels*sizeof(float));
        }
        context->Unmap(staging.Get(),0); return result;
    }
    void complete(const dspaa::CapturedUiLease& lease) {
        require(lease->status.cleanComplete && lease->status.occlusionComplete && lease->status.influenceComplete,lease->status.reason.c_str());
    }
    void worldPlane(const Pixels& pixels) { for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) for (unsigned c=0;c<4;++c) close(pixels.at(x,y,c),world[c],"UI contaminated the clean world"); }
    unsigned errors() {
        unsigned count=0;
        for (UINT64 index=0;index<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++index) {
            SIZE_T bytes=0; debug->GetMessage(index,nullptr,&bytes); std::vector<unsigned char> data(bytes);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data()); debug->GetMessage(index,message,&bytes);
            if (message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION || message->Severity==D3D11_MESSAGE_SEVERITY_ERROR) {
                ++count; std::cerr<<message->pDescription<<'\n';
            }
        }
        return count;
    }
};
void sourceOver(Fixture& f) {
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({1,0,0,0.5f},{4,4,16,12}); f.bindings(f.full,f.solid.Get());
    f.draw({0,1,0,0.25f},{10,4,24,12}); f.end(); f.bindings(f.full,f.solid.Get());
    auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
    auto a=f.read(lease->occlusion.Get()), m=f.read(lease->influence.Get()), final=f.read(f.full.texture.Get());
    close(a.at(6,6),0.5f,"Source-over opacity used squared target alpha"); close(final.at(6,6,3),0.75f,"Original alpha blend was modified or replayed twice");
    close(a.at(12,6),0.625f,"Overlapping UI transmittance is wrong"); close(final.at(12,6,0),0.45f,"Original overlap color was changed");
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) close(m.at(x,y),(y>=4&&y<12&&x>=4&&x<24)?1.f:0.f,"UI support escaped/lost its geometry");
}
void replaceAndTinyAlpha(Fixture& f) {
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false);
    f.draw({0,0,1,0},{2,2,8,8}); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({1,0,0,0.0001f},{12,2,18,8}); f.end();
    auto lease=f.seal(f.full); f.complete(lease); auto a=f.read(lease->occlusion.Get()),m=f.read(lease->influence.Get());
    close(a.at(3,3),1,"Alpha-zero replace did not fully occlude"); close(m.at(3,3),1,"Alpha-zero replace lost influence");
    close(a.at(13,3),0.0001f,"Small opacity was rounded away",0.000001f); close(m.at(13,3),1,"Small-alpha support disappeared"); f.worldPlane(f.read(lease->clean.Get()));
}
void fp32AlphaBoundary(Fixture& f) {
    f.nonnegative=false; // Alpha support must not depend on a positive-RGB promise.
    unsigned missing=0;
    const std::array<std::array<float,2>,2> cases{{{1e-9f,1e8f},{1e-40f,1e38f}}};
    for (const auto& sample : cases) {
        f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
        f.draw({sample[1],0,0,sample[0]},{2,2,8,8});
        f.draw({-sample[1],0,0,sample[0]},{12,2,18,8});
        f.draw({sample[1],0,0,0},{22,2,28,8}); f.end();
        auto lease=f.seal(f.full); f.complete(lease);
        auto h=f.read(lease->clean.Get()),m=f.read(lease->influence.Get()),actual=f.read(f.full.texture.Get());
        f.worldPlane(h);
        const float positive=actual.at(3,3,0)-h.at(3,3,0), negative=actual.at(13,3,0)-h.at(13,3,0);
        std::cerr<<"alpha_boundary alpha="<<sample[0]<<" hdr="<<sample[1]<<" positive_delta="<<positive
                 <<" negative_delta="<<negative<<" positive_m="<<m.at(3,3)<<" negative_m="<<m.at(13,3)<<'\n';
        if (sample[0]==cases[0][0]) {
            close(positive,0.1f,"FP32 alpha/HDR counterexample did not produce its real positive contribution");
            close(negative,-0.1f,"FP32 alpha/HDR counterexample did not produce its real negative contribution");
        }
        // The subnormal case observes the GPU's actual denormal behavior. A real
        // contribution must never disappear from M; no CPU epsilon defines it.
        if ((positive!=0 && m.at(3,3)!=1) || (negative!=0 && m.at(13,3)!=1)) ++missing;
        close(m.at(23,3),0,"Zero alpha invented an HDR influence rectangle");
        close(actual.at(23,3,0),world[0],"Zero-alpha HDR draw changed the original image");
    }
    f.nonnegative=true;
    require(missing==0,"FP32 alpha/HDR source-over contribution is missing from influence");
}
void signedCancellation(Fixture& f) {
    f.nonnegative=false; f.signedWithoutOverlap=true;
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ONE);
    f.draw({-0.5f,0,0,0},{2,2,8,8}); f.draw({0.5f,0,0,0},{2,2,8,8}); f.draw({0,0,0,0},{12,2,18,8}); f.end();
    auto lease=f.seal(f.full); f.complete(lease); auto a=f.read(lease->occlusion.Get()),m=f.read(lease->influence.Get()),final=f.read(f.full.texture.Get());
    close(final.at(3,3,0),world[0],"Signed fixture did not cancel in actual Final"); close(m.at(3,3),1,"Per-draw influence was erased by signed cancellation");
    close(m.at(13,3),0,"Zero contribution invented support"); close(a.at(3,3),0,"Additive contribution became opacity");
    require(lease->status.signedContributionDraws==3,"Signed fallback was not exercised");
    f.nonnegative=true; f.signedWithoutOverlap=false;
}
void stencil(Fixture& f) {
    f.begin(true); f.uiScope(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,0,false); f.context->OMSetDepthStencilState(f.push.Get(),0);
    f.draw({0,0,0,1},{4,4,20,12});
    f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.context->OMSetDepthStencilState(f.testOne.Get(),1); f.draw({1,0,0,0.5f});
    f.context->ClearDepthStencilView(f.dsv.Get(),D3D11_CLEAR_STENCIL,1,2);
    f.context->OMSetDepthStencilState(f.testTwo.Get(),2); f.draw({0,1,0,0.25f}); f.end(); f.bindings(f.full,f.solid.Get(),true);
    auto lease=f.seal(f.full); f.complete(lease); auto a=f.read(lease->occlusion.Get()),final=f.read(f.full.texture.Get());
    close(a.at(6,6),0.625f,"Private stencil did not start from pre-draw state"); close(a.at(1,1),0.25f,"Stencil clear was not mirrored");
    close(final.at(6,6,0),0.45f,"Capture mutated the application's stencil"); close(final.at(1,1,0),0.15f,"Original stencil clear/color sequence changed");
}
void writableDepthOverlap(Fixture& f) {
    for (unsigned depthWrite=0;depthWrite<2;++depthWrite) {
        D3D11_DEPTH_STENCIL_DESC description{};
        description.DepthEnable=depthWrite!=0; description.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; description.DepthFunc=D3D11_COMPARISON_LESS;
        description.StencilEnable=depthWrite==0; description.StencilReadMask=description.StencilWriteMask=255;
        description.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_INCR_SAT,D3D11_COMPARISON_EQUAL};
        description.BackFace=description.FrontFace;
        ComPtr<ID3D11DepthStencilState> state;
        dspaa::graphicsCheck(f.device->CreateDepthStencilState(&description,&state),"Create writable overlapping depth/stencil fixture");
        for (unsigned colored=0;colored<2;++colored) {
            f.begin(true); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
            f.context->OMSetDepthStencilState(state.Get(),0);
            f.context->VSSetShader(f.overlapVertex.Get(),nullptr,0); f.context->PSSetShader(f.overlapSolid.Get(),nullptr,0);
            f.parameters({1e8f,0,0,1e-9f},{4,4,20,12},static_cast<float>(colored));
            // Two overlapping primitives in ONE draw: first writes stencil=1 /
            // z=.25, then the second fails. Color in primitive 0 detects reusing
            // post-state; color in primitive 1 detects disabling these writes.
            f.context->Draw(6,0); f.end(); f.bindings(f.full,f.overlapSolid.Get(),true);
            auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
            auto actual=f.read(f.full.texture.Get()),m=f.read(lease->influence.Get());
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
                const bool contribution=colored==0 && x>=4 && x<20 && y>=4 && y<12;
                close(actual.at(x,y,0),world[0]+(contribution?0.1f:0.f),"Writable-overlap original raster behavior changed");
                close(m.at(x,y),contribution?1.f:0.f,
                      colored==0 ? "Alpha replay used depth/stencil post-state" : "Alpha replay disabled intra-draw depth/stencil writes");
            }
        }
    }
}
void filterAndPartial(Fixture& f) {
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ONE); f.draw({0.3f,0,0,0},{8,4,12,12}); f.end();
    require(f.capture->declareTexture(f.post.captured()),"Declare fixture effect output");
    f.bind(f.post); f.parameters({0,0,0,0}); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false);
    auto* source=f.full.srv.Get(); f.context->PSSetShaderResources(0,1,&source); f.context->PSSetShader(f.filter.Get(),nullptr,0);
    dspaa::CaptureScope scope; scope.kind=dspaa::CaptureScopeKind::DualColor; scope.passId=2; scope.fullOverwrite=true;
    scope.support.basis="Fixture filter() source: three point-clamp taps at x=-1,0,+1";
    dspaa::CaptureSamplingInput input; input.domains.push_back({}); input.domains[0].radiusTexels={1,0};
    scope.support.inputs.push_back(input); scope.support.occlusionResourceSlot=0;
    require(f.capture->beginScope(scope),"Begin three-tap color scope"); f.context->Draw(3,0); f.end(); f.bindings(f.post,f.filter.Get());
    scope={}; scope.kind=dspaa::CaptureScopeKind::PartialWrite; scope.passId=3; scope.support.basis="Fixture solid() constant color, actual one-column raster scissor";
    f.context->PSSetShader(f.solid.Get(),nullptr,0); f.parameters({0,1,0,1});
    const D3D11_RECT left{0,0,1,height}; f.context->RSSetScissorRects(1,&left);
    require(f.capture->beginScope(scope),"Begin partial border scope"); f.context->Draw(3,0); f.end();
    auto lease=f.seal(f.post); f.complete(lease); auto h=f.read(lease->clean.Get()),a=f.read(lease->occlusion.Get()),m=f.read(lease->influence.Get());
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        close(h.at(x,y,0),x==0?0.f:world[0],"Partial write destroyed clean interior");
        close(h.at(x,y,1),x==0?1.f:world[1],"Partial border was not replayed");
        close(a.at(x,y),0,"Bloom support was misrepresented as occlusion");
        close(m.at(x,y),(y>=4&&y<12&&x>=7&&x<=12)?1.f:0.f,"Three-tap support footprint is incorrect");
    }
}
void missingProof(Fixture& f) {
    f.verified=false; f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.draw({1,0,0,0.5f}); f.end();
    auto lease=f.seal(f.full);
    require(lease->clean && !lease->occlusion && !lease->influence && !lease->status.reason.empty(),"Unverified shader was silently accepted");
    close(f.read(f.full.texture.Get()).at(4,4,0),0.6f,"Fail-closed capture suppressed or duplicated the original draw"); f.verified=true;
}
void mismatchedEffectProof(Fixture& f) {
    f.begin(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false);
    dspaa::CaptureScope scope; scope.kind=dspaa::CaptureScopeKind::DualColor; scope.passId=2; scope.fullOverwrite=true;
    scope.support.basis="Fixture claims three-tap filter, but binds solid PS";
    require(f.capture->beginScope(scope),"Begin mismatched effect fixture"); f.draw({0,0,1,1}); f.end();
    auto lease=f.seal(f.full);
    require(!lease->clean && !lease->occlusion && !lease->influence && lease->status.colorReplays==0,
            "Effect declaration bypassed the actual shader proof");
    close(f.read(f.full.texture.Get()).at(4,4,2),1,"Rejected effect suppressed the original draw");
}
void missingFiniteRgb(Fixture& f) {
    f.finiteRgb=false; f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({std::numeric_limits<float>::infinity(),0,0,0}); f.end();
    auto lease=f.seal(f.full);
    require(lease->clean && lease->occlusion && !lease->influence,"Alpha-only support accepted unverified nonfinite RGB");
    f.worldPlane(f.read(lease->clean.Get()));close(f.read(lease->occlusion.Get()).at(4,4),0,"Finite-RGB rejection changed opacity");
    std::cout<<"nonfinite_rgb_zero_alpha actual_final="<<f.read(f.full.texture.Get()).at(4,4,0)<<" influence_exposed=false\n";
    f.finiteRgb=true;
}
void fragmentNonfiniteRgb(Fixture& f) {
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    const float nan=std::numeric_limits<float>::quiet_NaN(), inf=std::numeric_limits<float>::infinity();
    for (float alpha : {0.f,0.5f,1.f}) {
        const std::array<float,4> color{nan,inf,-inf,alpha};
        f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
        f.draw(color,{4,4,20,12}); f.end(); f.bindings(f.full,f.solid.Get());
        auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
        auto a=f.read(lease->occlusion.Get()),m=f.read(lease->influence.Get()),actual=f.read(f.full.texture.Get());
        require(lease->status.conservativeFragmentPromotions==1 && lease->status.conservativeFragmentReplays==1,
                "Nonfinite source-over did not expose its conservative support mode");
        // Compare actual color against the SAME draw with capture inactive; do
        // not assume a particular NaN payload or infinity-clamping result.
        f.bind(f.post); f.context->ClearRenderTargetView(f.post.rtv.Get(),world.data());
        f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.draw(color,{4,4,20,12});
        const auto reference=f.read(f.post.texture.Get());
        for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
            const bool covered=x>=4 && x<20 && y>=4 && y<12;
            close(m.at(x,y),covered?1.f:0.f,"Fragment support lost nonfinite/zero-alpha raster or escaped clip");
            close(a.at(x,y),covered?alpha:0.f,"Fragment support changed independent geometric opacity");
            for (unsigned c=0;c<4;++c) sameValue(actual.at(x,y,c),reference.at(x,y,c),"Capture changed the original nonfinite draw");
        }
    }
    f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
}
void fragmentMixedScopes(Fixture& f) {
    const float nan=std::numeric_limits<float>::quiet_NaN();
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({1e8f,0,0,1e-9f},{2,2,8,8}); // T rounds to 1; only old MAX-alpha support preserves this M.
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    f.draw({nan,0,0,0},{10,2,16,8});
    f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
    f.draw({1,0,0,0},{18,2,24,8}); // After promotion, finite alpha-zero source-over is intentionally wider too.
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    f.draw({nan,0,0,0},{26,2,30,8}); f.end();
    f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
    f.uiScope(); f.draw({1,0,0,0},{2,10,8,14}); f.end(); // Scope flush must restore precise mode.
    auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
    auto m=f.read(lease->influence.Get()),a=f.read(lease->occlusion.Get()),actual=f.read(f.full.texture.Get());
    require(lease->status.conservativeFragmentPromotions==1 && lease->status.conservativeFragmentReplays==3,
            "Shader/proof changes re-promoted scratch or leaked fragment mode across scope flush");
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const bool old=x>=2 && x<8, first=x>=10 && x<16, finite=x>=18 && x<24, second=x>=26 && x<30;
        close(m.at(x,y),(y>=2&&y<8&&(old||first||finite||second))?1.f:0.f,
              "Promotion lost previous M, narrowed later fragment support, or failed to reset on scope flush");
        close(a.at(x,y),0,"Conservative footprint became physical occlusion");
    }
    close(actual.at(3,3,0),world[0]+0.1f,"Promotion changed the already-rendered tiny-alpha HDR contribution");
    close(actual.at(19,3,0),world[0],"Promoted finite zero-alpha draw changed original color");
}
void fragmentStencilClip(Fixture& f) {
    f.begin(true); f.uiScope(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,0,false);
    f.context->OMSetDepthStencilState(f.push.Get(),0); f.draw({0,0,0,1},{4,4,20,12});
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.context->OMSetDepthStencilState(f.testOne.Get(),1);
    f.draw({std::numeric_limits<float>::quiet_NaN(),0,0,0.5f},{8,2,24,10}); f.end(); f.bindings(f.full,f.solid.Get(),true);
    auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
    auto m=f.read(lease->influence.Get()),a=f.read(lease->occlusion.Get()),actual=f.read(f.full.texture.Get());
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const bool covered=x>=8 && x<20 && y>=4 && y<10;
        close(m.at(x,y),covered?1.f:0.f,"Fragment mask ignored stencil fail or actual PS clip");
        close(a.at(x,y),covered?0.5f:0.f,"Stencil/clip fallback changed opacity");
        close(actual.at(x,y,3),covered?0.75f:1.f,"Stencil/clip fallback changed the original draw count");
    }
    // With capture inactive, test the application's stencil without changing it.
    f.bind(f.post,true); f.context->ClearRenderTargetView(f.post.rtv.Get(),world.data());
    f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false);
    f.context->OMSetDepthStencilState(f.testOne.Get(),1); f.draw({0,1,0,1});
    const auto tested=f.read(f.post.texture.Get());
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
        close(tested.at(x,y,1),(x>=4&&x<20&&y>=4&&y<12)?1.f:world[1],"Fragment replay mutated application stencil");
    f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
}
void fragmentSampleMask(Fixture& f) {
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_COLOR_WRITE_ENABLE_ALL,true,0);
    f.draw({std::numeric_limits<float>::quiet_NaN(),0,0,0.5f}); f.end();
    UINT mask=UINT_MAX; f.context->OMGetBlendState(nullptr,nullptr,&mask); require(mask==0,"Support work lost the original SampleMask");
    auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get())); f.worldPlane(f.read(f.full.texture.Get()));
    auto m=f.read(lease->influence.Get()),a=f.read(lease->occlusion.Get());
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        close(m.at(x,y),0,"SampleMask=0 invented a fragment footprint"); close(a.at(x,y),0,"SampleMask=0 changed opacity");
    }
    f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
}
void fragmentWritableOverlap(Fixture& f) {
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    for (unsigned depthWrite=0;depthWrite<2;++depthWrite) {
        D3D11_DEPTH_STENCIL_DESC description{};
        description.DepthEnable=depthWrite!=0; description.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; description.DepthFunc=D3D11_COMPARISON_LESS;
        description.StencilEnable=depthWrite==0; description.StencilReadMask=description.StencilWriteMask=255;
        description.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_INCR_SAT,D3D11_COMPARISON_EQUAL};
        description.BackFace=description.FrontFace;
        ComPtr<ID3D11DepthStencilState> write, verify;
        dspaa::graphicsCheck(f.device->CreateDepthStencilState(&description,&write),"Create fragment writable-overlap DSV state");
        description.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO; description.DepthFunc=D3D11_COMPARISON_EQUAL; description.StencilWriteMask=0;
        description.FrontFace.StencilPassOp=description.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
        dspaa::graphicsCheck(f.device->CreateDepthStencilState(&description,&verify),"Create read-only original DSV verification state");
        for (unsigned discarded=0;discarded<2;++discarded) {
            f.begin(true); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
            f.context->OMSetDepthStencilState(write.Get(),0);
            f.context->VSSetShader(f.overlapVertex.Get(),nullptr,0); f.context->PSSetShader(f.earlyOverlapSolid.Get(),nullptr,0);
            f.parameters({std::numeric_limits<float>::quiet_NaN(),0,0,0.5f},{4,4,20,12},static_cast<float>(discarded));
            // Early D/S: first writes z=.25/stencil=1 even when it discards;
            // second then fails. This distinguishes disabled writes from using
            // the post-state, despite the sink marking alpha-zero fragments too.
            f.context->Draw(6,0); f.end(); f.bindings(f.full,f.earlyOverlapSolid.Get(),true);
            auto lease=f.seal(f.full); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
            auto m=f.read(lease->influence.Get()),a=f.read(lease->occlusion.Get()),actual=f.read(f.full.texture.Get());
            require(lease->status.conservativeFragmentReplays==1 && lease->status.alphaDepthCopies==1,
                    "Writable fragment replay did not use an independent pre-state DSV");
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
                const bool covered=discarded==1 && x>=4 && x<20 && y>=4 && y<12;
                close(m.at(x,y),covered?1.f:0.f,discarded==0?"Fragment replay disabled intra-draw DSV writes":"Fragment replay used DSV post-state");
                close(a.at(x,y),covered?0.5f:0.f,"Writable fragment replay changed geometric opacity");
                close(actual.at(x,y,3),covered?0.75f:1.f,"Writable fragment replay changed original color execution");
            }
            // Original early writes cover the complete triangle, including PS
            // clip/discard. Verify them through a read-only equality test.
            f.bind(f.post,true); f.context->ClearRenderTargetView(f.post.rtv.Get(),world.data());
            f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false);
            f.context->OMSetDepthStencilState(verify.Get(),depthWrite?0:1); f.context->VSSetShader(f.overlapVertex.Get(),nullptr,0);
            f.draw({0,1,0,1}); const auto tested=f.read(f.post.texture.Get());
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
                close(tested.at(x,y,1),1,"Fragment replay changed the application's pre/post DSV semantics");
        }
    }
    f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
}
void fragmentOptInGates(Fixture& f) {
    const float nan=std::numeric_limits<float>::quiet_NaN();
    f.finiteRgb=false; f.nonnegative=false; f.conservativeFragments=true;
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.draw({nan,0,0,0},{2,2,8,8});
    f.conservativeFragments=false; f.draw({nan,0,0,0},{12,2,18,8}); f.end();
    {
        auto lease=f.seal(f.full);
        require(lease->clean && lease->occlusion && !lease->influence && lease->status.conservativeFragmentReplays==1,
                "A previous promotion authorized an unknown-RGB draw without opt-in");
    }
    f.conservativeFragments=true; f.verified=false;
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.draw({nan,0,0,nan}); f.end();
    auto lease=f.seal(f.full);
    require(lease->clean && !lease->occlusion && !lease->influence && lease->status.conservativeFragmentReplays==0,
            "Fragment opt-in bypassed the unit-alpha proof");
    require(std::isnan(f.read(f.full.texture.Get()).at(4,4,3)),"Unknown-alpha rejection suppressed the original draw");
    f.verified=true; f.finiteRgb=true; f.nonnegative=true; f.conservativeFragments=false;
}
void sharedInvalidation(Fixture& f) {
    f.begin(); dspaa::CaptureScope scope; scope.kind=dspaa::CaptureScopeKind::SharedPreparation; scope.passId=4;
    require(f.capture->beginScope(scope),"Begin shared preparation scope");
    f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false); f.draw({0,0,1,1}); f.end();
    require(!f.capture->seal(f.full.captured()),"Shared preparation reused a stale clean twin");
    require(!f.capture->status().cleanComplete,"Shared invalidation did not reach status");
}
void unscopedInvalidation(Fixture& f) {
    const auto diagnostics = f.diagnosticCalls;
    f.begin();
    f.blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({0, 0, 1, 0.5f});
    f.bindings(f.full, f.solid.Get());
    require(f.diagnosticCalls == diagnostics + 1,
            "Unscoped write did not reach its isolated diagnostic callback");
    require(!f.capture->seal(f.full.captured()) && !f.capture->status().cleanComplete,
            "Unscoped fast path reused a stale clean twin");
    close(f.read(f.full.texture.Get()).at(4, 4, 2), 0.5f + 0.5f * world[2],
          "Throwing diagnostic suppressed or duplicated the original draw");
}
void consumedUiValue(Fixture& f, const Texture& destination) {
    f.begin();
    f.uiScope();
    f.blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({1, 0, 0, 0.5f}, {4, 4, 20, 12});
    f.end();
    require(f.capture->declareTexture(destination.captured()), "Declare independent consumed color");
    f.context->CopyResource(destination.texture.Get(), f.full.texture.Get());
    require(f.capture->currentColor(destination.texture.Get()).texture != nullptr,
            "Color copy did not preserve the consumed value");
}
void unscopedConsumedValue(Fixture& f) {
    consumedUiValue(f, f.post);
    f.draw({0, 0, 1, 0.5f});
    require(!f.capture->currentColor(f.full.texture.Get()).texture,
            "Unscoped overwrite retained the old input value");
    require(f.capture->currentColor(f.post.texture.Get()).texture != nullptr,
            "A dead input overwrite invalidated its independent consumer");
    auto lease = f.seal(f.post);
    f.complete(lease);
    f.worldPlane(f.read(lease->clean.Get()));
    const auto a = f.read(lease->occlusion.Get()), m = f.read(lease->influence.Get()),
               actual = f.read(f.post.texture.Get());
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const bool covered = x >= 4 && x < 20 && y >= 4 && y < 12;
            close(a.at(x, y), covered ? 0.5f : 0.f,
                  "Source reuse destroyed the consumer's independent opacity");
            close(m.at(x, y), covered ? 1.f : 0.f,
                  "Source reuse destroyed the consumer's independent influence");
            close(actual.at(x, y, 0), covered ? 0.6f : world[0],
                  "Source reuse changed the already consumed application color");
        }
    close(f.read(f.full.texture.Get()).at(1, 1, 2), 0.5f + 0.5f * world[2],
          "Dead-input invalidation suppressed or duplicated the original draw");
}
void unscopedMissingInput(Fixture& f) {
    consumedUiValue(f, f.post);
    f.draw({0, 0, 1, 0.5f});
    require(!f.capture->bindCleanInput(f.post.captured(2), f.full.captured()),
            "Handoff consumed an invalidated input value");
    auto handoff = f.seal(f.post);
    require(!handoff->clean && !handoff->occlusion && !handoff->influence,
            "Rejected handoff exposed complete planes");
    handoff.reset();

    consumedUiValue(f, f.post);
    f.draw({0, 0, 1, 0.5f});
    f.bind(f.post);
    f.parameters({0, 0, 0, 0});
    f.blend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_ALL, false);
    auto* source = f.full.srv.Get();
    f.context->PSSetShaderResources(0, 1, &source);
    f.context->PSSetShader(f.filter.Get(), nullptr, 0);
    dspaa::CaptureScope scope;
    scope.kind = dspaa::CaptureScopeKind::DualColor;
    scope.passId = 2;
    scope.fullOverwrite = true;
    scope.support.basis = "Fixture three-tap filter must reject its invalidated source value";
    dspaa::CaptureSamplingInput input;
    input.domains.push_back({});
    input.domains[0].radiusTexels = {1, 0};
    scope.support.inputs.push_back(input);
    scope.support.occlusionResourceSlot = 0;
    require(f.capture->beginScope(scope), "Begin invalidated-input effect scope");
    f.context->Draw(3, 0);
    f.end();
    require(!f.capture->lastScopeOutput().texture, "Failed color dependency invented a scope output");
    auto effect = f.seal(f.post);
    require(!effect->clean && !effect->occlusion && !effect->influence,
            "Color replay consumed an invalidated input as shared full color");
    close(f.read(f.post.texture.Get()).at(6, 6, 2), 0.5f + 0.25f * world[2],
          "Rejected color dependency suppressed the original effect");
}
void originalOnlyUavInvalidation(Fixture& f, const dspaa::CaptureScope* scope = nullptr,
                                 bool indirect = false) {
    auto destination = f.make(D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11UnorderedAccessView> uav;
    dspaa::graphicsCheck(f.device->CreateUnorderedAccessView(destination.texture.Get(), nullptr, &uav),
                         "Create original-only write UAV");
    const auto code = compile("writeUav", "ps_5_0");
    ComPtr<ID3D11PixelShader> writer;
    dspaa::graphicsCheck(
        f.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &writer),
        "Create original-only UAV PS");
    ComPtr<ID3D11Buffer> arguments;
    if (indirect) {
        const std::array<UINT, 4> values{3, 1, 0, 0};
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(sizeof(values));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        const D3D11_SUBRESOURCE_DATA initial{values.data(), 0, 0};
        dspaa::graphicsCheck(f.device->CreateBuffer(&description, &initial, &arguments),
                             "Create original-only indirect draw arguments");
    }
    consumedUiValue(f, destination);
    require(f.capture->declareTexture(f.post.captured()), "Declare independent value C before UAV write");
    f.context->CopyResource(f.post.texture.Get(), destination.texture.Get());
    f.draw({0, 0, 1, 0.5f});
    require(f.capture->currentColor(destination.texture.Get()).texture != nullptr,
            "UAV fixture lost B before its actual write");
    // The RTV's A value is already invalid; only the OM UAV can invalidate B.
    // C consumed the previous B value and must survive the later original write.
    const UINT targetCount = 1, uavSlot = targetCount;
    auto* target = f.full.rtv.Get();
    auto* view = uav.Get();
    f.context->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, &target, nullptr, uavSlot, 1, &view,
                                                         nullptr);
    f.context->PSSetShader(writer.Get(), nullptr, 0);
    const auto before = f.capture->status();
    if (scope)
        require(f.capture->beginScope(*scope), "Begin original-only UAV scope");
    f.parameters({0, 0, 1, 0.5f});
    if (indirect)
        f.context->DrawInstancedIndirect(arguments.Get(), 0);
    else
        f.context->Draw(3, 0);
    if (scope)
        f.end();
    const auto after = f.capture->status();
    require(after.colorReplays == before.colorReplays && after.coverageReplays == before.coverageReplays,
            "Original-only UAV draw was replayed privately");
    require(after.cleanComplete && after.occlusionComplete && after.influenceComplete,
            "An original-only value overwrite poisoned independent branches globally");
    f.bindings(f.full, writer.Get());
    ComPtr<ID3D11UnorderedAccessView> retained;
    f.context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, uavSlot, 1, &retained);
    require(retained.Get() == uav.Get(),
            "Original-only invalidation changed the application's OM UAV binding");
    view = nullptr;
    f.context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr,
                                                         nullptr, uavSlot, 1, &view, nullptr);
    require(!f.capture->currentColor(destination.texture.Get()).texture,
            "Original-only OM UAV write retained a stale independent value");
    require(f.capture->currentColor(f.post.texture.Get()).texture != nullptr,
            "UAV write invalidated an independently consumed C value");
    require(!f.capture->seal(destination.captured()) && !f.capture->status().cleanComplete,
            "UAV-modified final color reused its stale private planes");
    close(f.read(destination.texture.Get()).at(6, 6, 2), 1,
          "Original-only UAV write did not reach the original resource");
    close(f.read(f.post.texture.Get()).at(6, 6, 0), 0.6f,
          "UAV write changed the independent consumer's original color");
    close(f.read(f.full.texture.Get()).at(1, 1, 2), 0.75f + 0.25f * world[2],
          "UAV invalidation suppressed or duplicated the original color draw");
}
void sharedUavInvalidation(Fixture& f) {
    for (const auto kind :
         {dspaa::CaptureScopeKind::SharedPreparation, dspaa::CaptureScopeKind::ExternalBlurPublication}) {
        dspaa::CaptureScope scope;
        scope.kind = kind;
        scope.passId = 4;
        originalOnlyUavInvalidation(f, &scope);
        originalOnlyUavInvalidation(f, &scope, true);
    }
}
void nativeClipTransfer(Fixture& f) {
    using Vertex = dspaa::proof::ClipBlitVertex;
    const std::array<Vertex, 4> quad{
        {{{-1, 1, 0}, {1, 0}}, {{1, 1, 0}, {0, 0}}, {{-1, -1, 0}, {1, 1}}, {{1, -1, 0}, {0, 1}}}};
    // Binding offset, draw start and negative base vertex are deliberately
    // independent. None of the three may be silently treated as zero.
    const std::array<Vertex, 8> vertices{Vertex{}, Vertex{}, quad[0], quad[1],
                                         quad[2],  quad[2],  quad[1], quad[3]};
    auto shadow = std::make_shared<dspaa::proof::ConstantShadow>(f.device.Get(), f.context.Get());
    dspaa::capture::attachWrites(shadow);
    struct Cleanup {
        Fixture& f;
        std::shared_ptr<dspaa::proof::ConstantShadow> shadow;
        ~Cleanup() {
            f.specialEffect = {};
            dspaa::capture::detachWrites(shadow.get());
            shadow->stop();
        }
    } cleanup{f, shadow};
    auto makeBuffer = [&](const void* data, UINT bytes, UINT bindings) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = bytes;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = bindings;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = data;
        ComPtr<ID3D11Buffer> buffer;
        dspaa::graphicsCheck(f.device->CreateBuffer(&description, &initial, &buffer),
                             "Create native blit IA fixture");
        require(shadow->created(buffer.Get(), data), "Observe native blit IA fixture bytes");
        return buffer;
    };
    auto vertexBuffer = makeBuffer(vertices.data(), sizeof(vertices), D3D11_BIND_VERTEX_BUFFER);
    // Large indices with a negative base must still address the actual vertices.
    constexpr uint16_t lastIndex = std::numeric_limits<uint16_t>::max() - 1;
    const std::array<uint16_t, 8> indices16{
        0, 0, lastIndex - 5, lastIndex - 4, lastIndex - 3, lastIndex - 2, lastIndex - 1, lastIndex};
    const std::array<uint32_t, 8> indices32{0, 0, 2, 3, 4, 5, 6, 7};
    auto index16 = makeBuffer(indices16.data(), sizeof(indices16), D3D11_BIND_INDEX_BUFFER);
    auto index32 = makeBuffer(indices32.data(), sizeof(indices32), D3D11_BIND_INDEX_BUFFER);
    const auto vertexCode = compile("clipVertex", "vs_5_0"), pixelCode = compile("clipPixel", "ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    dspaa::graphicsCheck(f.device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                      vertexCode->GetBufferSize(), nullptr, &vertex),
                         "Create native blit fixture VS");
    dspaa::graphicsCheck(f.device->CreatePixelShader(pixelCode->GetBufferPointer(),
                                                     pixelCode->GetBufferSize(), nullptr, &pixel),
                         "Create native blit fixture PS");
    const D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
         D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D11InputLayout> layout;
    dspaa::graphicsCheck(f.device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                                                     vertexCode->GetBufferPointer(),
                                                     vertexCode->GetBufferSize(), &layout),
                         "Create native blit fixture layout");
    dspaa::proof::observeBlitLayout(layout.Get(), elements, static_cast<unsigned>(std::size(elements)));
    const D3D11_INPUT_ELEMENT_DESC splitElements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT,
         D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D11InputLayout> splitLayout;
    dspaa::graphicsCheck(f.device->CreateInputLayout(
                             splitElements, static_cast<UINT>(std::size(splitElements)),
                             vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), &splitLayout),
                         "Create per-slot APPEND blit layout");
    dspaa::proof::observeBlitLayout(splitLayout.Get(), splitElements,
                                    static_cast<unsigned>(std::size(splitElements)));
    for (unsigned kind = 0; kind < 4; ++kind) {
        const bool indexed = (kind & 1u) != 0, instanced = (kind & 2u) != 0;
        const int base = indexed ? (kind == 1 ? 1 - static_cast<int>(indices16[2]) : -1) : 0;
        unsigned observations = 0;
        std::string rejection;
        f.specialEffect = [&](ID3D11PixelShader* actual, const dspaa::CaptureScope& scope,
                              const dspaa::capture::DrawArguments& args, dspaa::CaptureSupport& support) {
            ++observations;
            require(actual == pixel.Get() && scope.passId == 0x1000,
                    "Blit fixture mixed its actual shader contract");
            ComPtr<ID3D11VertexShader> actualVertex;
            f.context->VSGetShader(&actualVertex, nullptr, nullptr);
            require(actualVertex.Get() == vertex.Get(), "Blit fixture mixed its actual VS contract");
            require(args.indexed == indexed && args.count == 6 && args.start == 1 && args.instances == 1 &&
                        args.firstInstance == (instanced ? 9u : 0u) && args.baseVertex == base,
                    "Draw hook changed actual IA arguments");
            return dspaa::proof::inspectClipBlit(f.context.Get(), *shadow, args, width, height, support,
                                                 rejection);
        };
        const float untouched[]{0.9f, 0.8f, 0.7f, 0.6f};
        f.context->ClearRenderTargetView(f.post.rtv.Get(), untouched);
        f.begin();
        f.uiScope();
        f.blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
        f.draw({1, 0, 0, 0.5f}, {4, 4, 20, 12});
        f.end();
        require(f.capture->declareTexture(f.post.captured()), "Declare native blit output");
        f.bind(f.post);
        f.blend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_ALL, false);
        auto* input = f.full.srv.Get();
        f.context->PSSetShaderResources(0, 1, &input);
        ID3D11Buffer* buffers[]{vertexBuffer.Get(), vertexBuffer.Get()};
        const UINT strides[]{sizeof(Vertex), sizeof(Vertex)},
            offsets[]{sizeof(Vertex), sizeof(Vertex) + offsetof(Vertex, uv)};
        f.context->IASetVertexBuffers(0, static_cast<UINT>(std::size(buffers)), buffers, strides, offsets);
        f.context->IASetInputLayout(kind == 3 ? splitLayout.Get() : layout.Get());
        f.context->VSSetShader(vertex.Get(), nullptr, 0);
        f.context->PSSetShader(pixel.Get(), nullptr, 0);
        f.context->IASetIndexBuffer(kind == 3 ? index32.Get() : index16.Get(),
                                    kind == 3 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                    kind == 3 ? sizeof(uint32_t) : sizeof(uint16_t));
        dspaa::CaptureScope scope;
        scope.kind = dspaa::CaptureScopeKind::DualColor;
        scope.passId = 0x1000;
        scope.fullOverwrite = true;
        require(f.capture->beginScope(scope), "Begin native blit fixture scope");
        if (kind == 0)
            f.context->Draw(6, 1);
        else if (kind == 1)
            f.context->DrawIndexed(6, 1, base);
        else if (kind == 2)
            f.context->DrawInstanced(6, 1, 1, 9);
        else
            f.context->DrawIndexedInstanced(6, 1, 1, base, 9);
        require(rejection.empty(), rejection.c_str());
        require(observations == 1, "Native blit draw was missed or its private replay re-entered proof");
        f.end();
        f.bindings(f.post, pixel.Get());
        ComPtr<ID3D11InputLayout> actualLayout;
        f.context->IAGetInputLayout(&actualLayout);
        require(actualLayout.Get() == (kind == 3 ? splitLayout.Get() : layout.Get()),
                "Capture changed the application IA layout");
        for (UINT slot = 0; slot < std::size(buffers); ++slot) {
            ComPtr<ID3D11Buffer> actualBuffer;
            UINT actualStride = 0, actualOffset = 0;
            f.context->IAGetVertexBuffers(slot, 1, &actualBuffer, &actualStride, &actualOffset);
            require(actualBuffer.Get() == buffers[slot] && actualStride == strides[slot] &&
                        actualOffset == offsets[slot],
                    "Capture changed an application IA binding");
        }
        auto lease = f.seal(f.post);
        f.complete(lease);
        const auto clean = f.read(lease->clean.Get());
        const auto opacity = f.read(lease->occlusion.Get()), influence = f.read(lease->influence.Get()),
                   actual = f.read(f.post.texture.Get());
        std::cout << "native_clip kind=" << kind << " clean00=" << clean.at(0, 0)
                  << " final00=" << actual.at(0, 0) << " cleanLast=" << clean.at(width - 1, height - 1)
                  << " finalLast=" << actual.at(width - 1, height - 1) << " errors=" << f.errors() << '\n';
        f.worldPlane(clean);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const bool covered = x >= width - 20 && x < width - 4 && y >= 4 && y < 12;
                close(opacity.at(x, y), covered ? 0.5f : 0.f, "Observed mirrored UV lost geometric opacity");
                close(influence.at(x, y), covered ? 1.f : 0.f, "Observed mirrored UV lost UI influence");
                close(actual.at(x, y, 0), covered ? 0.6f : world[0],
                      "Native blit hook changed the original color copy");
            }
        // The same valid shaders/bytes cannot authorize a later clipped draw.
        const D3D11_RECT clipped{0, 0, width / 2, height};
        f.context->RSSetScissorRects(1, &clipped);
        dspaa::CaptureSupport denied;
        std::string why;
        const dspaa::capture::DrawArguments args{indexed, 6, 1, 1, instanced ? 9u : 0u, base};
        require(!dspaa::proof::inspectClipBlit(f.context.Get(), *shadow, args, width, height, denied, why) &&
                    denied.basis.empty(),
                "Native copy accepted a partial scissor as full-output geometry");
        f.specialEffect = {};
    }
    const D3D11_RECT fullScissor{0, 0, width, height};
    f.context->RSSetScissorRects(1, &fullScissor);
    auto cutIndices = indices16;
    cutIndices.back() = std::numeric_limits<uint16_t>::max();
    f.context->UpdateSubresource(index16.Get(), 0, nullptr, cutIndices.data(), 0, 0);
    f.context->IASetIndexBuffer(index16.Get(), DXGI_FORMAT_R16_UINT, sizeof(uint16_t));
    dspaa::CaptureSupport denied;
    std::string why;
    const dspaa::capture::DrawArguments cut{true, 6, 1, 1, 0, 1 - static_cast<int>(indices16[2])};
    require(!dspaa::proof::inspectClipBlit(f.context.Get(), *shadow, cut, width, height, denied, why) &&
                denied.basis.empty(),
            "Maximum IA index falsely proved a complete rectangle after a negative base");
}
void missingSignedProof(Fixture& f) {
    f.nonnegative=false; f.signedWithoutOverlap=false;
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ONE); f.draw({-0.5f,0,0,0}); f.end();
    auto lease=f.seal(f.full);
    require(lease->clean && lease->occlusion && !lease->influence && lease->status.cleanComplete && lease->status.occlusionComplete,
            "Missing signed support incorrectly invalidated independent clean/opacity planes or exposed a false mask");
    close(f.read(lease->occlusion.Get()).at(4,4),0,"Signed additive changed opacity");
    close(f.read(f.full.texture.Get()).at(4,4,0),-0.3f,"Signed original draw was suppressed"); f.nonnegative=true;
}
void rasterQuery(Fixture& f) {
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION,0}; ComPtr<ID3D11Query> query;
    dspaa::graphicsCheck(f.device->CreateQuery(&description,&query),"Create fixture raster query");
    require(f.capture->privateRasterAllowed(),"Private raster gate rejected a query-free live owner");
    f.context->Begin(query.Get()); require(!f.capture->privateRasterAllowed(),"Private raster gate ignored an active occlusion query");
    f.draw({1,0,0,0.5f}); f.context->End(query.Get());
    require(f.capture->privateRasterAllowed(),"Private raster gate did not recover after query end"); f.end();
    auto lease=f.seal(f.full);
    require(!lease->clean && !lease->occlusion && !lease->influence,"Capture replayed inside a raster-dependent query");
    close(f.read(f.full.texture.Get()).at(4,4,0),0.6f,"Rejected query frame changed original drawing");
    UINT64 samples=0;
    require(f.context->GetData(query.Get(),&samples,sizeof(samples),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && samples==width*height,
            "Capture changed the application's occlusion result");
}
void allocationEpoch(Fixture& f) {
    f.begin(); require(f.capture->declareTexture(f.full.captured(2)),"Redeclare fixture allocation epoch");
    require(!f.capture->seal(f.full.captured(1)),"Stale allocation epoch was silently accepted");
    require(!f.capture->status().cleanComplete,"Epoch mismatch did not invalidate capture status");
}
void implicitDrawTarget(Fixture& f) {
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({1,0,0,0.5f},{4,4,20,12}); f.end();
    require(!f.capture->currentColor(f.post.texture.Get()).texture,"An untouched allocation was promoted to clean color");
    dspaa::CaptureScope scope; scope.kind=dspaa::CaptureScopeKind::DualColor; scope.passId=2;
    scope.fullOverwrite=true; scope.implicitOutputEpoch=77;
    scope.support.basis="Fixture filter; trusted full write into a target bound after scope begin";
    dspaa::CaptureSamplingInput input; input.domains.push_back({}); input.domains[0].radiusTexels={1,0};
    scope.support.inputs.push_back(input); scope.support.occlusionResourceSlot=0;
    // full is still bound here. post has deliberately NOT been declared.
    require(f.capture->beginScope(scope),"Begin implicit-target scope");
    require(!f.capture->lastScopeOutput().texture,"A scope without a completed draw invented an output");
    f.bind(f.post); f.parameters({0,0,0,0}); f.blend(D3D11_BLEND_ONE,D3D11_BLEND_ZERO,D3D11_COLOR_WRITE_ENABLE_ALL,false);
    auto* source=f.full.srv.Get(); f.context->PSSetShaderResources(0,1,&source); f.context->PSSetShader(f.filter.Get(),nullptr,0);
    f.context->Draw(3,0); f.end(); f.bindings(f.post,f.filter.Get());
    const auto actualOutput=f.capture->lastScopeOutput();
    require(actualOutput.texture.Get()==f.post.texture.Get() && actualOutput.epoch==77,"Capture guessed the scope-entry RTV instead of the actual draw output");
    const auto currentOutput=f.capture->currentColor(f.post.texture.Get());
    require(currentOutput.texture.Get()==f.post.texture.Get() && currentOutput.epoch==77,"Final sealing guessed a surface epoch instead of retaining its observed color value");
    auto lease=f.seal(f.post,currentOutput.epoch); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
    require(!f.capture->lastScopeOutput().texture,"Retired scope output leaked into a later frame");
    const auto m=f.read(lease->influence.Get()),a=f.read(lease->occlusion.Get());
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        close(m.at(x,y),(y>=4&&y<12&&x>=3&&x<=20)?1.f:0.f,"Implicit output lost the actual filter's UI support");
        close(a.at(x,y),(y>=4&&y<12&&x>=4&&x<20)?0.5f:0.f,"Implicit output changed geometric opacity");
    }
}
void sameAllocationCameraHandoff(Fixture& f) {
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA);
    f.draw({1,0,0,0.5f},{4,4,20,12}); f.end();
    require(f.capture->bindCleanInput(f.full.captured(2),f.full.captured(1)),"Same-allocation camera handoff failed");
    auto lease=f.seal(f.full,2); f.complete(lease); f.worldPlane(f.read(lease->clean.Get()));
    const auto a=f.read(lease->occlusion.Get()),m=f.read(lease->influence.Get()),actual=f.read(f.full.texture.Get());
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const bool covered=x>=4&&x<20&&y>=4&&y<12;
        close(a.at(x,y),covered?0.5f:0.f,"New camera epoch discarded prior geometric opacity");
        close(m.at(x,y),covered?1.f:0.f,"New camera epoch discarded prior UI influence");
        close(actual.at(x,y,0),covered?0.6f:world[0],"Camera handoff altered original color");
    }
}
void retiredApplicationReferences(Fixture& f) {
    f.context->ClearState(); f.graphics->drain();
    require(f.capture->releaseApplicationReferences(),"Release completed fixture application references");
    const auto references=[&]() { f.full.texture->AddRef(); return f.full.texture->Release(); };
    const auto baseline=references();
    f.begin(); require(!f.capture->releaseApplicationReferences(),"Reference release accepted an active capture arena");
    f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.draw({1,0,0,0.5f},{4,4,20,12}); f.end();
    auto lease=f.seal(f.full); f.complete(lease);
    f.context->ClearState(); f.graphics->drain();
    require(references()>baseline,"Reference fixture did not retain an application texture");
    require(f.capture->releaseApplicationReferences(),"Retired references were not released while a frame lease remained live");
    require(references()==baseline,"Retired capture arena still holds the application's texture");
    f.worldPlane(f.read(lease->clean.Get())); close(f.read(lease->occlusion.Get()).at(6,6),0.5f,"Releasing application references destroyed private opacity");
    close(f.read(lease->influence.Get()).at(6,6),1,"Releasing application references destroyed private influence");
    // A subsequent frame must not reuse the first lease's private arena.
    f.begin(); f.uiScope(); f.blend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA); f.draw({0,1,0,0.25f},{24,2,30,8}); f.end();
    auto next=f.seal(f.full); f.complete(next);
    require(f.capture->releaseApplicationReferences(),"Second retired frame could not release application references");
    f.worldPlane(f.read(lease->clean.Get())); auto old=f.read(lease->influence.Get());
    close(old.at(6,6),1,"Live old lease lost its influence after arena reuse"); close(old.at(26,4),0,"New frame overwrote a live old lease");
}
void helperCrossStageRestore(Fixture& f) {
    f.bind(f.full); ComPtr<ID3D11Device5> device; dspaa::graphicsCheck(f.device.As(&device),"Get helper fixture device5");
    dspaa::capture::Gpu gpu(device.Get(),f.context.Get()); auto t=gpu.create(width,height,DXGI_FORMAT_R32_FLOAT);
    auto ui=gpu.create(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT); gpu.clear(*t,1);
    auto* alias=ui->srv.Get(); f.context->VSSetShaderResources(7,1,&alias); f.context->CSSetShaderResources(11,1,&alias);
    gpu.seedUi(*t,*ui); ComPtr<ID3D11ShaderResourceView> restored;
    f.context->VSGetShaderResources(7,1,&restored); require(restored.Get()==alias,"Helper lost implicitly unbound VS alias"); restored.Reset();
    f.context->CSGetShaderResources(11,1,&restored); require(restored.Get()==alias,"Helper lost implicitly unbound CS alias");
    f.bind(f.full); // Release aliases before temporary helper planes retire.
    f.graphics->drain();
}
} // namespace
int main() {
    try {
        Fixture fixture;
        fp32AlphaBoundary(fixture);
        sourceOver(fixture); replaceAndTinyAlpha(fixture); signedCancellation(fixture); stencil(fixture); writableDepthOverlap(fixture);
        filterAndPartial(fixture); mismatchedEffectProof(fixture); missingFiniteRgb(fixture); missingProof(fixture); missingSignedProof(fixture); rasterQuery(fixture); sharedInvalidation(fixture); unscopedInvalidation(fixture);
        unscopedConsumedValue(fixture);
        unscopedMissingInput(fixture);
        originalOnlyUavInvalidation(fixture);
        sharedUavInvalidation(fixture);
        nativeClipTransfer(fixture);
        fragmentNonfiniteRgb(fixture); fragmentMixedScopes(fixture); fragmentStencilClip(fixture);
        fragmentSampleMask(fixture); fragmentWritableOverlap(fixture); fragmentOptInGates(fixture);
        sourceOver(fixture); // Recovery after an invalidated frame, and safe arena reuse.
        allocationEpoch(fixture); sameAllocationCameraHandoff(fixture); implicitDrawTarget(fixture);
        retiredApplicationReferences(fixture); helperCrossStageRestore(fixture);
        require(fixture.capture->stop() && fixture.capture->stop(),"Capture stop was not idempotent/quiescent");
        require(!fixture.capture->privateRasterAllowed(),"Stopped owner still allowed private raster work");
        const auto errors=fixture.errors(); require(errors==0,"D3D11 debug layer reported errors");
        std::cout<<"{\"capture_ui_probe\":\"passed\",\"sealed_frames\":"<<fixture.checkedFrames<<",\"d3d11_errors\":"<<errors
                 <<",\"window_created\":false,\"signed_per_draw\":true,\"stencil_prestate\":true,\"partial_write\":true,\"small_alpha\":true,\"writable_dsv_prestate\":true,\"conservative_fragment_support\":true,\"retired_application_refs\":true}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"UI capture probe failed: "<<error.what()<<'\n'; return 1; }
}
