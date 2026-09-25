#include "gpu.h"
#include "hooks.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dspaa::capture {
namespace {
DXGI_FORMAT typed(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    default: return format;
    }
}
template<class T, size_t Size> void release(std::array<T*, Size>& values) {
    for (auto*& value : values) if (value) { value->Release(); value = nullptr; }
}
constexpr char shader[] = R"(
cbuffer Parameters : register(b0) {
    float4 transformMatrix;
    float4 biasOffset;
    int4 sizes; // source width/height, destination width/height
    int4 mode; // x/y dilation step, address U/V
    int4 limits; // conservative source footprint radius, unused, unused
};
Texture2D<float4> source0 : register(t0);
Texture2D<float4> source1 : register(t1);
Texture2D<float4> source2 : register(t2);
SamplerState originalSampler : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
int address(int p, int n, int kind) {
    if (kind == 1) return (p % n + n) % n;
    if (kind == 2) { int q = (p % (2*n) + 2*n) % (2*n); return q < n ? q : 2*n-1-q; }
    if (kind == 5) p = p < 0 ? -p-1 : p;
    return clamp(p, 0, n-1);
}
float value(int2 p) {
    return source0.Load(int3(address(p.x, sizes.x, mode.z), address(p.y, sizes.y, mode.w), 0)).r;
}
float4 seedPS(float4 p : SV_Position) : SV_Target { return float4(0, 0, 0, source0.Load(int3(int2(p.xy), 0)).r); }
float4 resetPS(float4 p : SV_Position) : SV_Target { return float4(0, 0, 0, source0.Load(int3(int2(p.xy), 0)).a); }
float4 tPS(float4 p : SV_Position) : SV_Target { return source0.Load(int3(int2(p.xy), 0)).aaaa; }
float4 mPS(float4 p : SV_Position) : SV_Target {
    float4 ui = source0.Load(int3(int2(p.xy), 0));
    // Independent alpha support avoids cancellation at T=1. Bit tests also
    // preserve nonzero data which a float comparison could denormal-flush.
    float support = mode.x != 0 ? source2.Load(int3(int2(p.xy), 0)).a : 0;
    // Mode 2 is deliberately wider than alpha magnitude: every surviving
    // fragment wrote zero into a plane whose alpha began at one, even at alpha=0.
    bool alpha = mode.x == 2 ? (1-support) != 0 : (asuint(support) & 0x7fffffff) != 0;
    bool contribution = any((asuint(ui.rgb) & 0x7fffffff) != 0);
    float m = max(source1.Load(int3(int2(p.xy), 0)).r, (ui.a != 1 || alpha || contribution) ? 1 : 0);
    return m.xxxx;
}
float4 invertPS(float4 p : SV_Position) : SV_Target { float a = 1-source0.Load(int3(int2(p.xy), 0)).r; return a.xxxx; }
float4 maxPS(float4 p : SV_Position) : SV_Target {
    float m = max(source0.Load(int3(int2(p.xy), 0)).r, source1.Load(int3(int2(p.xy), 0)).r); return m.xxxx;
}
float4 dilatePS(float4 p : SV_Position) : SV_Target {
    int2 center = int2(p.xy); float m = max(value(center), max(value(center-mode.xy), value(center+mode.xy))); return m.xxxx;
}
float2 sourceUv(float2 p) {
    float2 uv = p / float2(sizes.zw);
    return float2(dot(transformMatrix.xy,uv),dot(transformMatrix.zw,uv)) + biasOffset.xy + biasOffset.zw/float2(sizes.xy);
}
float4 affineMaxPS(float4 p : SV_Position) : SV_Target {
    int2 pixel = int2(floor(sourceUv(p.xy)*float2(sizes.xy)));
    // A border sample wholly outside the declared footprint cannot see UI.
    // Near the border, clamping the pre-dilated plane conservatively contains
    // every valid source texel; it never invents a nonzero constant border.
    if ((mode.z == 4 && (pixel.x+limits.x < 0 || pixel.x-limits.x >= sizes.x)) ||
        (mode.w == 4 && (pixel.y+limits.y < 0 || pixel.y-limits.y >= sizes.y))) return 0;
    float m = value(pixel); return m.xxxx;
}
float4 affineTPS(float4 p : SV_Position) : SV_Target { return source0.SampleLevel(originalSampler, sourceUv(p.xy), 0).rrrr; }
)";
ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> result, errors;
    const auto hr = D3DCompile(shader, sizeof(shader)-1, "DSPAASR capture support", nullptr, nullptr, entry, profile,
                              D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &errors);
    if (FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    graphicsCheck(hr, "Compile capture support shader"); return result;
}
void validate(const CaptureSampleDomain& domain, unsigned width, unsigned height) {
    for (float value : domain.matrix) if (!std::isfinite(value)) throw std::invalid_argument("Non-finite capture support matrix");
    for (float value : domain.bias) if (!std::isfinite(value)) throw std::invalid_argument("Non-finite capture support bias");
    for (float value : domain.offsetTexels) if (!std::isfinite(value)) throw std::invalid_argument("Non-finite capture support offset");
    for (unsigned x = 0; x < 2; ++x) for (unsigned y = 0; y < 2; ++y) {
        const double px = (static_cast<double>(domain.matrix[0])*x + static_cast<double>(domain.matrix[1])*y + domain.bias[0])*width + domain.offsetTexels[0];
        const double py = (static_cast<double>(domain.matrix[2])*x + static_cast<double>(domain.matrix[3])*y + domain.bias[1])*height + domain.offsetTexels[1];
        if (std::abs(px) > 536870911.0 || std::abs(py) > 536870911.0)
            throw std::invalid_argument("Capture support coordinates exceed safe shader integer addressing");
    }
    if (domain.radiusTexels[0] > width || domain.radiusTexels[1] > height)
        throw std::invalid_argument("Capture support radius exceeds its declared finite source domain");
}
void validateSampler(const D3D11_SAMPLER_DESC& description) {
    const auto filter = static_cast<unsigned>(description.Filter);
    if ((filter & 0x80u) || (filter & 0x40u) || filter > 0x55u)
        throw std::invalid_argument("Comparison/anisotropic/reduction support needs a separate verified sampling contract");
    if (description.AddressU < D3D11_TEXTURE_ADDRESS_WRAP || description.AddressU > D3D11_TEXTURE_ADDRESS_MIRROR_ONCE ||
        description.AddressV < D3D11_TEXTURE_ADDRESS_WRAP || description.AddressV > D3D11_TEXTURE_ADDRESS_MIRROR_ONCE)
        throw std::invalid_argument("Unknown capture sampler addressing");
}
} // namespace
StateGuard::StateGuard(ID3D11DeviceContext4* value) : context(value) {
    context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), &depthView);
    context->OMGetBlendState(&blend, blendFactor.data(), &sampleMask);
    context->OMGetDepthStencilState(&depthState, &stencilReference);
    context->VSGetShaderResources(0, 128, resources[0].data()); context->HSGetShaderResources(0, 128, resources[1].data());
    context->DSGetShaderResources(0, 128, resources[2].data()); context->GSGetShaderResources(0, 128, resources[3].data());
    context->PSGetShaderResources(0, 128, resources[4].data()); context->CSGetShaderResources(0, 128, resources[5].data());
    context->VSGetShader(&vertex, classes[0].data(), &classCounts[0]);
    context->HSGetShader(&hull, classes[1].data(), &classCounts[1]);
    context->DSGetShader(&domain, classes[2].data(), &classCounts[2]);
    context->GSGetShader(&geometry, classes[3].data(), &classCounts[3]);
    context->PSGetShader(&pixelShader, classes[4].data(), &classCounts[4]);
    context->RSGetState(&raster); context->RSGetViewports(&viewportCount, viewports.data()); context->RSGetScissorRects(&scissorCount, scissors.data());
    context->IAGetInputLayout(&layout); context->IAGetPrimitiveTopology(&topology);
    context->PSGetConstantBuffers1(0, 1, &pixelConstants, &firstConstant, &constantCount);
    context->PSGetSamplers(0, 1, &sampler);
    ComPtr<ID3D11Predicate> predicate; BOOL predicateValue = FALSE;
    context->GetPredication(&predicate, &predicateValue);
    std::array<ID3D11Buffer*, 4> output{}; context->SOGetTargets(4, output.data());
    pureRaster = !predicate && !hull && !domain && !geometry;
    for (auto* buffer : output) if (buffer) pureRaster = false;
    release(output);
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
    std::array<ID3D11UnorderedAccessView*, 64> unordered{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, unordered.data());
    for (auto* view : unordered) if (view) pureRaster = false;
    release(unordered);
    // Merely bound CS UAVs are not accessed by these raster-only helpers.
    // Private targets cannot alias application UAV resources; do not clear or
    // rebind CS UAVs, whose hidden counters could not be restored.
}
void StateGuard::restore() noexcept {
    if (restored) return;
    restored = true;
    // No helper/replay is allowed to mutate a rejected state. In particular,
    // OMSetRenderTargets here would itself destroy an active UAV binding.
    if (!pureRaster) return;
    Bypass guard;
    std::array<ID3D11ShaderResourceView*, 128> empty{};
    context->VSSetShaderResources(0, 128, empty.data()); context->HSSetShaderResources(0, 128, empty.data());
    context->DSSetShaderResources(0, 128, empty.data()); context->GSSetShaderResources(0, 128, empty.data());
    context->PSSetShaderResources(0, 128, empty.data()); context->CSSetShaderResources(0, 128, empty.data());
    context->OMSetRenderTargets(8, targets.data(), depthView.Get());
    context->OMSetBlendState(blend.Get(), blendFactor.data(), sampleMask);
    context->OMSetDepthStencilState(depthState.Get(), stencilReference);
    context->VSSetShader(vertex.Get(), classes[0].data(), classCounts[0]);
    context->HSSetShader(hull.Get(), classes[1].data(), classCounts[1]);
    context->DSSetShader(domain.Get(), classes[2].data(), classCounts[2]);
    context->GSSetShader(geometry.Get(), classes[3].data(), classCounts[3]);
    context->PSSetShader(pixelShader.Get(), classes[4].data(), classCounts[4]);
    context->RSSetState(raster.Get()); context->RSSetViewports(viewportCount, viewports.data()); context->RSSetScissorRects(scissorCount, scissors.data());
    context->IASetInputLayout(layout.Get()); context->IASetPrimitiveTopology(topology);
    auto* constant = pixelConstants.Get(); auto* sample = sampler.Get();
    context->PSSetConstantBuffers1(0, 1, &constant, &firstConstant, &constantCount); context->PSSetSamplers(0, 1, &sample);
    context->VSSetShaderResources(0, 128, resources[0].data()); context->HSSetShaderResources(0, 128, resources[1].data());
    context->DSSetShaderResources(0, 128, resources[2].data()); context->GSSetShaderResources(0, 128, resources[3].data());
    context->PSSetShaderResources(0, 128, resources[4].data()); context->CSSetShaderResources(0, 128, resources[5].data());
}
StateGuard::~StateGuard() {
    restore(); release(targets);
    for (auto& row : resources) release(row);
    for (auto& row : classes) release(row);
}
Gpu::Gpu(ID3D11Device5* value, ID3D11DeviceContext4* immediate) : device(value), context(immediate) {
    UINT support = 0;
    graphicsCheck(device->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT, &support), "Query float capture blend support");
    if (!(support & D3D11_FORMAT_SUPPORT_BLENDABLE)) throw std::runtime_error("GPU cannot blend the required FP32 capture target");
    auto code = compile("vs", "vs_5_0");
    graphicsCheck(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex), "Create capture vertex shader");
    auto pixel = [&](const char* entry, ComPtr<ID3D11PixelShader>& output) {
        const auto binary = compile(entry, "ps_5_0");
        graphicsCheck(device->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr, &output), "Create capture pixel shader");
    };
    pixel("seedPS", seed); pixel("resetPS", resetRgb); pixel("tPS", extractT); pixel("mPS", extractM); pixel("invertPS", invert);
    pixel("maxPS", maximum); pixel("dilatePS", dilate); pixel("affineMaxPS", affineMax); pixel("affineTPS", affineT);
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(Parameters); buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    graphicsCheck(device->CreateBuffer(&buffer, nullptr, &constants), "Create capture support constants");
    D3D11_RASTERIZER_DESC rasterDescription{}; rasterDescription.FillMode = D3D11_FILL_SOLID; rasterDescription.CullMode = D3D11_CULL_NONE;
    rasterDescription.DepthClipEnable = TRUE;
    graphicsCheck(device->CreateRasterizerState(&rasterDescription, &raster), "Create capture raster state");
    D3D11_DEPTH_STENCIL_DESC depthDescription{}; depthDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depthDescription.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    depthDescription.BackFace = depthDescription.FrontFace;
    graphicsCheck(device->CreateDepthStencilState(&depthDescription, &noDepth), "Create capture depth-disabled state");
    D3D11_BLEND_DESC blend{}; auto& target = blend.RenderTarget[0];
    target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE; target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD; target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    graphicsCheck(device->CreateBlendState(&blend, &overwrite), "Create capture overwrite blend");
}
bool Gpu::supported(ID3D11Texture2D* texture) {
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
    return description.Width && description.Height && description.MipLevels == 1 && description.ArraySize == 1 &&
           description.SampleDesc.Count == 1 && !(description.MiscFlags & D3D11_RESOURCE_MISC_TILED);
}
ImagePtr Gpu::create(unsigned width, unsigned height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC description{}; description.Width = width; description.Height = height; description.Format = format;
    description.MipLevels = 1; description.ArraySize = 1; description.SampleDesc.Count = 1; description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    auto image = std::make_shared<Image>(); image->width = width; image->height = height; image->format = format;
    graphicsCheck(device->CreateTexture2D(&description, nullptr, &image->texture), "Create private capture plane");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = typed(format); srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    graphicsCheck(device->CreateShaderResourceView(image->texture.Get(), &srv, &image->srv), "Create capture plane SRV");
    D3D11_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format = typed(format); rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    graphicsCheck(device->CreateRenderTargetView(image->texture.Get(), &rtv, &image->rtv), "Create capture plane RTV");
    return image;
}
ImagePtr Gpu::clone(ID3D11Texture2D* source) {
    if (!supported(source)) throw std::invalid_argument("Capture currently requires single-sample, single-mip, single-layer 2D color");
    D3D11_TEXTURE2D_DESC description{}; source->GetDesc(&description);
    return create(description.Width, description.Height, description.Format);
}
ComPtr<ID3D11ShaderResourceView> Gpu::view(const Image& image, ID3D11ShaderResourceView* original) {
    D3D11_SHADER_RESOURCE_VIEW_DESC description{}; original->GetDesc(&description);
    if (description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || description.Texture2D.MostDetailedMip != 0 ||
        (description.Texture2D.MipLevels != 1 && description.Texture2D.MipLevels != UINT_MAX))
        throw std::invalid_argument("Capture input uses an unsupported SRV subresource domain");
    ComPtr<ID3D11ShaderResourceView> view;
    graphicsCheck(device->CreateShaderResourceView(image.texture.Get(), &description, &view), "Create matching clean input view"); return view;
}
ComPtr<ID3D11RenderTargetView> Gpu::view(const Image& image, ID3D11RenderTargetView* original) {
    D3D11_RENDER_TARGET_VIEW_DESC description{}; original->GetDesc(&description);
    if (description.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || description.Texture2D.MipSlice != 0)
        throw std::invalid_argument("Capture output uses an unsupported RTV subresource domain");
    ComPtr<ID3D11RenderTargetView> view;
    graphicsCheck(device->CreateRenderTargetView(image.texture.Get(), &description, &view), "Create matching clean output view"); return view;
}
void Gpu::draw(ID3D11PixelShader* program, const Image& output, const Image* first, const Image* second,
               const Parameters& parameters, ID3D11SamplerState* sampler, const Image* third) {
    Bypass bypass; StateGuard restore(context);
    if (!restore.pureRaster) throw std::runtime_error("Capture helper encountered UAV/SO/predication/tessellation/geometry state");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    graphicsCheck(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map capture support constants");
    std::memcpy(mapped.pData, &parameters, sizeof(parameters)); context->Unmap(constants.Get(), 0);
    ID3D11ShaderResourceView* empty[3]{}; context->PSSetShaderResources(0, 3, empty);
    auto* target = output.rtv.Get(); context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(overwrite.Get(), nullptr, UINT_MAX); context->OMSetDepthStencilState(noDepth.Get(), 0);
    context->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(output.width), static_cast<float>(output.height), 0, 1};
    context->RSSetViewports(1, &viewport); context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0); context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0); context->GSSetShader(nullptr, nullptr, 0); context->PSSetShader(program, nullptr, 0);
    auto* constant = constants.Get(); context->PSSetConstantBuffers(0, 1, &constant); context->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* resources[] = {first ? first->srv.Get() : nullptr, second ? second->srv.Get() : nullptr,
                                           third ? third->srv.Get() : nullptr};
    context->PSSetShaderResources(0, 3, resources); context->Draw(3, 0); ++draws;
}
void Gpu::clear(const Image& image, float value) { Bypass bypass; const float color[] = {value,value,value,value}; context->ClearRenderTargetView(image.rtv.Get(), color); }
void Gpu::seedUi(const Image& t, const Image& ui) { draw(seed.Get(), ui, &t, nullptr, {}); }
void Gpu::resetUiRgb(const Image& previous, const Image& destination) { draw(resetRgb.Get(), destination, &previous, nullptr, {}); }
void Gpu::extractUi(const Image& ui, const Image* alphaSupport, const Image& previous, const Image& t, const Image& m, bool conservativeFragments) {
    if (conservativeFragments && !alphaSupport) throw std::logic_error("Fragment support mode has no initialized alpha sink");
    draw(extractT.Get(), t, &ui, nullptr, {});
    Parameters p; p.mode[0] = alphaSupport ? (conservativeFragments ? 2 : 1) : 0;
    draw(extractM.Get(), m, &ui, &previous, p, nullptr, alphaSupport ? alphaSupport : &previous);
}
void Gpu::opacity(const Image& t, const Image& output) { draw(invert.Get(), output, &t, nullptr, {}); }
void Gpu::unite(const Image& first, const Image& second, const Image& output) { draw(maximum.Get(), output, &first, &second, {}); }
D3D11_SAMPLER_DESC Gpu::samplerDescription(ID3D11DeviceContext* context, unsigned slot) {
    if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) throw std::invalid_argument("Capture sampler slot is out of range");
    ComPtr<ID3D11SamplerState> state; context->PSGetSamplers(slot, 1, &state);
    D3D11_SAMPLER_DESC description{};
    if (state) state->GetDesc(&description);
    else { description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; description.AddressU = description.AddressV = description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; }
    validateSampler(description); return description;
}
ImagePtr Gpu::support(const Image& source, const CaptureSamplingInput& input, const Image& shape,
                      const D3D11_SAMPLER_DESC& sampler, const AllocateImage& allocate) {
    if (input.domains.empty() || input.domains.size() > 64) throw std::invalid_argument("Capture pass has no bounded sampling-support domains");
    validateSampler(sampler);
    auto accumulated = allocate(shape.width, shape.height, DXGI_FORMAT_R16_FLOAT); clear(*accumulated, 0);
    for (const auto& domain : input.domains) {
        validate(domain, source.width, source.height);
        const unsigned filterRadius = (static_cast<unsigned>(sampler.Filter) & 0x14u) ? 1u : 0u;
        const unsigned radii[] = {domain.radiusTexels[0]+filterRadius, domain.radiusTexels[1]+filterRadius};
        ImagePtr current;
        const Image* read = &source;
        for (unsigned axis = 0; axis < 2; ++axis) {
            unsigned covered = 0;
            while (covered < radii[axis]) {
                const unsigned step = std::min(radii[axis]-covered, covered*2+1);
                auto next = allocate(source.width, source.height, DXGI_FORMAT_R16_FLOAT);
                Parameters p; p.sizes = {static_cast<int>(source.width),static_cast<int>(source.height),static_cast<int>(source.width),static_cast<int>(source.height)};
                p.mode = {axis == 0 ? static_cast<int>(step) : 0, axis == 1 ? static_cast<int>(step) : 0,
                          sampler.AddressU == D3D11_TEXTURE_ADDRESS_WRAP ? 1 : 3, sampler.AddressV == D3D11_TEXTURE_ADDRESS_WRAP ? 1 : 3};
                draw(dilate.Get(), *next, read, nullptr, p);
                current = std::move(next); read = current.get(); covered += step;
            }
        }
        auto transformed = allocate(shape.width, shape.height, DXGI_FORMAT_R16_FLOAT);
        Parameters p; p.matrix = domain.matrix; p.biasOffset = {domain.bias[0],domain.bias[1],domain.offsetTexels[0],domain.offsetTexels[1]};
        p.sizes = {static_cast<int>(source.width),static_cast<int>(source.height),static_cast<int>(shape.width),static_cast<int>(shape.height)};
        p.mode = {0,0,static_cast<int>(sampler.AddressU),static_cast<int>(sampler.AddressV)};
        p.limits = {static_cast<int>(radii[0]),static_cast<int>(radii[1]),0,0};
        draw(affineMax.Get(), *transformed, read, nullptr, p);
        auto result = allocate(shape.width, shape.height, DXGI_FORMAT_R16_FLOAT);
        unite(*accumulated, *transformed, *result); accumulated = std::move(result);
    }
    return accumulated;
}
ImagePtr Gpu::occlusion(const Image& source, const CaptureSampleDomain& domain, const Image& shape,
                        const D3D11_SAMPLER_DESC& sampler, const AllocateImage& allocate) {
    validate(domain, source.width, source.height); validateSampler(sampler);
    if (domain.radiusTexels[0] || domain.radiusTexels[1]) throw std::invalid_argument("Effect support radius is not geometric opacity");
    D3D11_SAMPLER_DESC sampling = sampler;
    sampling.MinLOD = sampling.MaxLOD = 0; sampling.MaxAnisotropy = 1; sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
    std::fill(std::begin(sampling.BorderColor), std::end(sampling.BorderColor), 1.f);
    ComPtr<ID3D11SamplerState> state;
    graphicsCheck(device->CreateSamplerState(&sampling, &state), "Create occlusion transport sampler");
    auto result = allocate(shape.width, shape.height, DXGI_FORMAT_R32_FLOAT);
    Parameters p; p.matrix = domain.matrix; p.biasOffset = {domain.bias[0],domain.bias[1],domain.offsetTexels[0],domain.offsetTexels[1]};
    p.sizes = {static_cast<int>(source.width),static_cast<int>(source.height),static_cast<int>(shape.width),static_cast<int>(shape.height)};
    draw(affineT.Get(), *result, &source, nullptr, p, state.Get()); return result;
}
} // namespace dspaa::capture
