#include "display-color.h"
#include "display-layout.h"
#include "graphics/dx11-dx12.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace dspaa {
namespace {
// Load storage values explicitly. Only the captured RTV's sRGB evidence enables
// linear-light filtering; swapchain color-space metadata is not a conversion rule.
// Decoding before interpolation and encoding afterwards reproduces an sRGB
// sampling/view pair without requiring a typeless cross-API shared allocation.
constexpr char shader[] = R"hlsl(
Texture2D<float4> sourceImage : register(t0);
cbuffer Layout : register(b0) { float2 origin; float2 extent; uint srgbFilter; };
float4 fullscreen(uint id : SV_VertexID) : SV_Position {
    return float4((id & 1) * 4.0 - 1.0, (id & 2) * -2.0 + 1.0, 0, 1);
}
float3 decode(float3 v) {
    return float3(v.x <= .04045 ? v.x / 12.92 : pow((v.x + .055) / 1.055, 2.4),
                  v.y <= .04045 ? v.y / 12.92 : pow((v.y + .055) / 1.055, 2.4),
                  v.z <= .04045 ? v.z / 12.92 : pow((v.z + .055) / 1.055, 2.4));
}
float3 encode(float3 v) {
    return float3(v.x <= .0031308 ? v.x * 12.92 : 1.055 * pow(v.x, 1.0 / 2.4) - .055,
                  v.y <= .0031308 ? v.y * 12.92 : 1.055 * pow(v.y, 1.0 / 2.4) - .055,
                  v.z <= .0031308 ? v.z * 12.92 : 1.055 * pow(v.z, 1.0 / 2.4) - .055);
}
float4 fetch(int2 p, int2 size) {
    float4 v = sourceImage.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
    if (srgbFilter) v.rgb = decode(v.rgb);
    return v;
}
float4 resample(float4 position : SV_Position) : SV_Target {
    uint w, h; sourceImage.GetDimensions(w, h);
    float2 p = (position.xy - origin) / extent * float2(w, h) - .5;
    int2 i = int2(floor(p)); float2 f = frac(p);
    float4 a = lerp(fetch(i, int2(w, h)), fetch(i + int2(1, 0), int2(w, h)), f.x);
    float4 b = lerp(fetch(i + int2(0, 1), int2(w, h)), fetch(i + 1, int2(w, h)), f.x);
    float4 v = lerp(a, b, f.y);
    if (srgbFilter) v.rgb = encode(v.rgb);
    return v;
}
)hlsl";
ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> result, errors;
    if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "DSPAASR display-domain color", nullptr, nullptr, entry,
                          target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result,
                          &errors)))
        throw std::runtime_error(errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                      errors->GetBufferSize())
                                        : "Compile display-domain color");
    return result;
}
DXGI_FORMAT sampleFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}
} // namespace
void DisplayColor::initialize(ID3D12Device* device, DXGI_FORMAT format) {
    if (!allocator_) {
        graphicsCheck(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)),
            "Create display-color allocator");
        graphicsCheck(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                                IID_PPV_ARGS(&commands_)),
                      "Create display-color commands");
        graphicsCheck(commands_->Close(), "Close initial display-color commands");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        graphicsCheck(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views_)), "Create display-color SRV");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        graphicsCheck(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets_)),
                      "Create display-color RTV");
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {1, &range};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, 5};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 2;
        root.pParameters = parameters;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> bytes, errors;
        graphicsCheck(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors),
                      "Serialize display-color root");
        graphicsCheck(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                                  IID_PPV_ARGS(&root_)),
                      "Create display-color root");
    }
    if (pipeline_ && pipelineFormat_ == format)
        return;
    const auto vs = compile("fullscreen", "vs_5_1"), ps = compile("resample", "ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[0].SrcBlend = p.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    p.BlendState.RenderTarget[0].DestBlend = p.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    p.BlendState.RenderTarget[0].BlendOp = p.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    p.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.FrontFace = p.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    p.SampleMask = UINT_MAX;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = format;
    p.SampleDesc.Count = 1;
    graphicsCheck(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline_)),
                  "Create display-color pipeline");
    pipelineFormat_ = format;
}
PresentImage DisplayColor::convert(ID3D12Device* device, ID3D12CommandQueue* queue,
                                   const PresentImage& source, unsigned width, unsigned height,
                                   DXGI_FORMAT format, RECT rect, bool srgbFilter) {
    rect = displayRect(width, height, rect);
    initialize(device, format);
    if (output_) {
        const auto d = output_->GetDesc();
        if (d.Width != width || d.Height != height || d.Format != format)
            output_.Reset();
    }
    if (!output_) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        graphicsCheck(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&output_)),
                      "Create display-color output");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = sampleFormat(source.resource->GetDesc().Format);
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source.resource.Get(), &view,
                                     views_->GetCPUDescriptorHandleForHeapStart());
    D3D12_RENDER_TARGET_VIEW_DESC target{};
    target.Format = format;
    target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto rtv = targets_->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(output_.Get(), &target, rtv);
    graphicsCheck(allocator_->Reset(), "Reset retired display-color allocator");
    graphicsCheck(commands_->Reset(allocator_.Get(), pipeline_.Get()), "Reset display-color commands");
    auto* list = commands_.Get();
    Dx11Dx12::transition(list, source.resource.Get(), source.state,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Dx11Dx12::transition(list, output_.Get(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float black[]{0, 0, 0, 1};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    list->SetGraphicsRootSignature(root_.Get());
    ID3D12DescriptorHeap* heaps[]{views_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, views_->GetGPUDescriptorHandleForHeapStart());
    struct Constants {
        float x, y, width, height;
        uint32_t srgb;
    } constants{static_cast<float>(rect.left), static_cast<float>(rect.top),
                static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top),
                srgbFilter ? 1u : 0u};
    list->SetGraphicsRoot32BitConstants(1, 5, &constants, 0);
    const D3D12_VIEWPORT viewport{constants.x, constants.y, constants.width, constants.height, 0, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    Dx11Dx12::transition(list, source.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         source.state);
    Dx11Dx12::transition(list, output_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_COMMON);
    graphicsCheck(list->Close(), "Close display-color commands");
    ID3D12CommandList* lists[]{list};
    queue->ExecuteCommandLists(1, lists);
    return {output_, D3D12_RESOURCE_STATE_COMMON};
}
} // namespace dspaa
