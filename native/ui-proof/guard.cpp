#include "guard.h"
#include "blit.h"
#include "capture/hooks.h"
#include "constants.h"
#include "default-ui.h"
#include "effect-pins.h"
#include "effects.h"
#include "shadow.h"
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>
#include <vector>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID shaderKey{0xa41ac69b, 0xc695, 0x498b, {0x92, 0x4f, 0x39, 0x04, 0x8c, 0x0c, 0x11, 0x21}};
constexpr GUID layoutKey{0xa41ac69b, 0xc695, 0x498b, {0x92, 0x4f, 0x39, 0x04, 0x8c, 0x0c, 0x11, 0x22}};
enum class Kind : uint32_t {
    Unknown,
    WidgetVs,
    NavigationVs,
    TextVs,
    TranslucentVs,
    WidgetAlpha,
    WidgetAdditive,
    Navigation,
    TextAlpha,
    TextAdditive,
    Translucent,
    Dashboard,
    DefaultVs,
    DefaultPremultiplied
};
struct ShaderFacts {
    Kind kind = Kind::Unknown;
    char hash[65]{};
};
struct LayoutFacts {
    bool color32 = false;
};
template <class T> bool data(ID3D11DeviceChild* child, REFGUID key, T& value) {
    UINT size = sizeof(value);
    return child && SUCCEEDED(child->GetPrivateData(key, &size, &value)) && size == sizeof(value);
}
ShaderFacts identify(const void* bytes, SIZE_T count, bool vertex) {
    ShaderFacts result;
    if (!bytes || !count || count > std::numeric_limits<ULONG>::max())
        return result;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return result;
    std::array<unsigned char, 32> digest{};
    const auto status =
        BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(static_cast<const unsigned char*>(bytes)),
                   static_cast<ULONG>(count), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        return result;
    constexpr char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < digest.size(); ++i) {
        result.hash[i * 2] = hex[digest[i] >> 4];
        result.hash[i * 2 + 1] = hex[digest[i] & 15];
    }
    const std::pair<const char*, Kind> vs[] = {
        {"4EFB1D2E69625325E72BF9F8BE691CD0BB617F529B45476198DA99439C8B17E4", Kind::WidgetVs},
        {"7BAE2FE4BECBAE50E41A68CF246CA69FF7731F1A8F23E04D5A6006AB8923EF80", Kind::NavigationVs},
        {"B62E9EFD8A2B6998BC87351E8B724353DED29A9B271C9911458CF3691508782C", Kind::TextVs},
        {"3D1189B9D29D5856637982D29525018F3869A4D86470D37B0A390C248D281D4C", Kind::TranslucentVs},
        {"BB6C244F507942E083B2328D0C3B4E2519D666D81805CC4F1EACB676F7CECB0C", Kind::DefaultVs}};
    const std::pair<const char*, Kind> ps[] = {
        {"8ECF949216A72CCD1876C6BDDECD8C0F30350760B9D18E64A708FF1BE4069FF6", Kind::WidgetAlpha},
        {"5D87E3C3507B018417B015C61CC62829F356D7863A3DB252DE42ED71874C5DFB", Kind::WidgetAdditive},
        {"07B8248FBFFD194A76A0F9DD1EAE89EF44D6AB57F2C45C1DCA08B32C8694287B", Kind::Navigation},
        {"CD0D5B7BA30DB5A3535901162AD1E422E32A705361067908CDC53EA089E4E494", Kind::TextAlpha},
        {"F182860B81EBA71D21BD9CC893171EB91DDF6536BB16A2CC4388612AE07F3782", Kind::TextAdditive},
        {"8A833F28E7BC58605EAD70ECB5B23EDEA711EF0AAA30C17474D6335A47ECC07C", Kind::Translucent},
        {"43CF3CFDD35AE60322ABEA09F56E1D2EDF9B6CEC29A7D01CB00F7DB8EE87F483", Kind::Translucent},
        {"6205AA6AA3C822E607830C1E1574874F1C2E21DABF6151497F26EC0ED1171754", Kind::Dashboard},
        // Exact non-stereo UI/Default variants: none, alpha clip, rect, both.
        // All four pair with DefaultVs; a shader name never grants admission.
        {"C3F5AA2458CBF03ED2AFACC862A26BA8AF0CFF47B8844409B688D6FB3835D5F0", Kind::DefaultPremultiplied},
        {"942B42EA9E10BFC8B5212419D6141F5069D98BE9FC26DFD698D4F937E36D4074", Kind::DefaultPremultiplied},
        {"A1FC44C15A7AD1C264788712EEC3218413B7A7E8FCA072B305EE8ECBBA9BF7DD", Kind::DefaultPremultiplied},
        {"6FF5417FDDF3DF7D4431E39BD03A2891B2B7E66E23B1AD6DB02F7B3AD6E5020B", Kind::DefaultPremultiplied}};
    const auto* begin = vertex ? vs : ps;
    const auto n = vertex ? std::size(vs) : std::size(ps);
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(result.hash, begin[i].first) == 0) {
            result.kind = begin[i].second;
            break;
        }
    return result;
}
struct DeviceObserver {
    virtual ~DeviceObserver() = default;
    virtual bool matches(ID3D11Device*) const noexcept = 0;
    virtual void buffer(ID3D11Buffer*, const D3D11_BUFFER_DESC&, const void*) noexcept = 0;
};
struct DeviceHook {
    void* target = nullptr;
    void* original = nullptr;
    bool enabled = false;
};
struct DeviceHooks {
    std::mutex mutex;
    std::weak_ptr<DeviceObserver> observer;
    DeviceHook buffer, layout, vertex, pixel;
};
DeviceHooks& hooks() {
    static auto* value = new DeviceHooks;
    return *value;
}
std::shared_ptr<DeviceObserver> observer(ID3D11Device* device) {
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    auto value = h.observer.lock();
    return value && value->matches(device) ? value : nullptr;
}
HRESULT STDMETHODCALLTYPE createBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* initial, ID3D11Buffer** output) {
    using F = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                          const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
    const auto result = reinterpret_cast<F>(hooks().buffer.original)(self, desc, initial, output);
    try {
        if (SUCCEEDED(result) && output && *output && desc)
            if (auto owner = observer(self))
                owner->buffer(*output, *desc, initial ? initial->pSysMem : nullptr);
    } catch (...) {
    }
    return result;
}
HRESULT STDMETHODCALLTYPE createLayout(ID3D11Device* self, const D3D11_INPUT_ELEMENT_DESC* elements,
                                       UINT count, const void* signature, SIZE_T bytes,
                                       ID3D11InputLayout** output) {
    using F = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*,
                                          SIZE_T, ID3D11InputLayout**);
    const auto result =
        reinterpret_cast<F>(hooks().layout.original)(self, elements, count, signature, bytes, output);
    try {
        if (SUCCEEDED(result) && output && *output && elements && observer(self)) {
            LayoutFacts proof;
            unsigned colors = 0;
            for (UINT i = 0; i < count; ++i)
                if (elements[i].SemanticName && _stricmp(elements[i].SemanticName, "COLOR") == 0 &&
                    elements[i].SemanticIndex == 0) {
                    ++colors;
                    proof.color32 = elements[i].Format == DXGI_FORMAT_R8G8B8A8_UNORM;
                }
            proof.color32 = proof.color32 && colors == 1;
            (*output)->SetPrivateData(layoutKey, sizeof(proof), &proof);
            dspaa::proof::observeBlitLayout(*output, elements, count);
        }
    } catch (...) {
    }
    return result;
}
template <class Shader>
HRESULT shader(bool vertex, ID3D11Device* self, const void* bytes, SIZE_T count, ID3D11ClassLinkage* linkage,
               Shader** output) {
    using F = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, Shader**);
    const auto result = reinterpret_cast<F>(vertex ? hooks().vertex.original : hooks().pixel.original)(
        self, bytes, count, linkage, output);
    try {
        if (SUCCEEDED(result) && output && *output && observer(self)) {
            const auto proof = identify(bytes, count, vertex);
            (*output)->SetPrivateData(shaderKey, sizeof(proof), &proof);
        }
    } catch (...) {
    }
    return result;
}
HRESULT STDMETHODCALLTYPE createVertex(ID3D11Device* s, const void* b, SIZE_T n, ID3D11ClassLinkage* c,
                                       ID3D11VertexShader** o) {
    return shader(true, s, b, n, c, o);
}
HRESULT STDMETHODCALLTYPE createPixel(ID3D11Device* s, const void* b, SIZE_T n, ID3D11ClassLinkage* c,
                                      ID3D11PixelShader** o) {
    return shader(false, s, b, n, c, o);
}
void install(DeviceHook& hook, void* target, void* replacement) {
    if (hook.target && hook.target != target)
        throw std::runtime_error("Another UI proof device implementation is unsupported");
    if (hook.enabled)
        return;
    if (!hook.target) {
        if (MH_CreateHook(target, replacement, &hook.original) != MH_OK)
            throw std::runtime_error("Cannot install UI proof device observer");
        hook.target = target;
    }
    if (MH_EnableHook(target) != MH_OK)
        throw std::runtime_error("Cannot enable UI proof device observer");
    hook.enabled = true;
}
bool unit(float x) {
    return std::isfinite(x) && x >= 0 && x <= 1;
}
bool unorm(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}
const EffectLayout* effectLayout(uint64_t passId, bool pixel, const ShaderFacts& shader) {
    using Key = std::tuple<uint64_t, bool, std::string_view>;
    static const auto index = [] {
        std::map<Key, const EffectLayout*> result;
        for (const auto& pin : effectPins) {
            if (pin.layout >= std::size(effectLayouts))
                throw std::runtime_error("Effect pin layout is out of range");
            const Key key{pin.passId, pin.pixel, std::string_view(pin.sha256, 64)};
            auto [found, inserted] = result.emplace(key, &effectLayouts[pin.layout]);
            if (!inserted && found->second != &effectLayouts[pin.layout])
                throw std::runtime_error("Ambiguous effect DXBC binding metadata");
        }
        return result;
    }();
    const auto found = index.find(Key{passId, pixel, std::string_view(shader.hash, 64)});
    return found == index.end() ? nullptr : found->second;
}
bool effectPair(uint64_t passId, const ShaderFacts& vertex, const ShaderFacts& pixel) {
    const std::string_view vs(vertex.hash, 64), ps(pixel.hash, 64);
    // These passes contain more than one compiled VS interface. Individual
    // membership does not authorize a cross-pair with different varying slots.
    if (passId == 0x1000)
        return (vs == "22B5E3649275F591A3E9259BE7F73193AD7E58556915BE065A6C1AA2DCF813F4" &&
                ps == "A3E0427AEC540A1619E169A5384330620E9620EE1F1A93FBF9C14B628867A904") ||
               (vs == "66EAB5C84F6FF65D1B26F6E05A3081F6EAF508B19BA6AEC9FC0041410ACB0998" &&
                ps == "76E5E517046A4A01AC6B3F89B27BDC317418EB9E9D7499745993076131065648");
    if (passId == 0x1302)
        return (vs == "AFF50ECF260D4FBA913CE83E5D36214893D46C0A9D56CBB001F225BB4CC8F760" &&
                ps == "33012E1CBED576765BFD3D2495C4C7A5274EECECFC5B00AD5C349DC070C34B15") ||
               (vs == "1B5050A9A3704E6E4DFFE051E9F481D7DB9D28FC86E257F6CAFB22192B29DEC7" &&
                ps == "F95B62538C9EB914280DF7FE535D0C192C6E249D022C321B62725332737AF371");
    static const auto uniqueVertices = [] {
        std::map<uint64_t, std::string_view> result;
        for (const auto& pin : effectPins)
            if (!pin.pixel) {
                const std::string_view hash(pin.sha256, 64);
                auto [found, inserted] = result.emplace(pin.passId, hash);
                if (!inserted && found->second != hash)
                    found->second = {};
            }
        return result;
    }();
    const auto found = uniqueVertices.find(passId);
    return found != uniqueVertices.end() && found->second == vs;
}
} // namespace

struct UiShaderProof::Impl final : DeviceObserver {
    std::shared_ptr<Dx11Dx12> graphics;
    std::shared_ptr<proof::ConstantShadow> shadow;
    std::function<void(const char*)> log;
    mutable std::mutex statusMutex;
    UiProofStatus observation;
    bool stopped = false, retired = false; // Serialized by the graphics lock.
    // Moving pool origins must not consume the diagnostic budget every frame.
    // Deduplicate by pass/pin/failure and binding layout (not firstConstant),
    // with a fixed maximum of 16 records for this owner, never reset per frame.
    using ConstantFailureKey = std::tuple<uint64_t, bool, unsigned, unsigned, unsigned, unsigned, unsigned,
                                          unsigned, bool, std::string>;
    std::vector<ConstantFailureKey> constantFailures;
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string, unsigned>> effectFailures;
    void effectFailure(ID3D11PixelShader* pixel, const CaptureScope& scope,
                       const capture::DrawArguments& arguments, const std::string& reason) noexcept {
        constexpr size_t maximumDiagnostics = 16;
        if (!log || effectFailures.size() >= maximumDiagnostics)
            return;
        try {
            auto* context = graphics->context11();
            ComPtr<ID3D11VertexShader> vertex;
            context->VSGetShader(&vertex, nullptr, nullptr);
            const auto ps = UiShaderProof::shaderFingerprint(pixel),
                       vs = UiShaderProof::shaderFingerprint(vertex.Get());
            ComPtr<ID3D11BlendState> blend;
            context->OMGetBlendState(&blend, nullptr, nullptr);
            D3D11_BLEND_DESC blendDescription{};
            if (blend)
                blend->GetDesc(&blendDescription);
            const unsigned colorWrite =
                blend ? blendDescription.RenderTarget[0].RenderTargetWriteMask : D3D11_COLOR_WRITE_ENABLE_ALL;
            const auto key = std::make_tuple(scope.passId, reason, ps, vs, colorWrite);
            if (std::find(effectFailures.begin(), effectFailures.end(), key) != effectFailures.end())
                return;
            effectFailures.push_back(key);
            ComPtr<ID3D11DepthStencilState> depth;
            UINT stencilReference = 0;
            context->OMGetDepthStencilState(&depth, &stencilReference);
            D3D11_DEPTH_STENCIL_DESC depthDescription{};
            if (depth)
                depth->GetDesc(&depthDescription);
            else {
                depthDescription.DepthEnable = TRUE;
                depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                depthDescription.DepthFunc = D3D11_COMPARISON_LESS;
            }
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->IAGetPrimitiveTopology(&topology);
            char message[1024];
            std::snprintf(message, sizeof(message),
                          "capture.effect-unverified pass=0x%llX indexed=%u count=%u start=%u base=%d "
                          "instances=%u topology=%u colorWrite=0x%X depthEnabled=%u "
                          "depthWrite=%u depthFunc=%u stencilEnabled=%u stencilRef=%u PS=%s VS=%s reason=%s",
                          static_cast<unsigned long long>(scope.passId), arguments.indexed ? 1u : 0u,
                          arguments.count, arguments.start, arguments.baseVertex, arguments.instances,
                          static_cast<unsigned>(topology), colorWrite, depthDescription.DepthEnable ? 1u : 0u,
                          static_cast<unsigned>(depthDescription.DepthWriteMask),
                          static_cast<unsigned>(depthDescription.DepthFunc),
                          depthDescription.StencilEnable ? 1u : 0u, stencilReference, ps.c_str(), vs.c_str(),
                          reason.c_str());
            log(message);
        } catch (...) {
        }
    }
    Impl(std::shared_ptr<Dx11Dx12> bridge, std::function<void(const char*)> output)
        : graphics(std::move(bridge)),
          shadow(std::make_shared<proof::ConstantShadow>(graphics->device11(), graphics->context11())),
          log(std::move(output)) {}
    bool matches(ID3D11Device* device) const noexcept override {
        return device == graphics->device11();
    }
    void buffer(ID3D11Buffer* value, const D3D11_BUFFER_DESC&, const void* initial) noexcept override {
        shadow->created(value, initial);
    }
    void constantFailure(uint64_t pass, bool vertex, unsigned slot, unsigned offset, unsigned components,
                         UINT first, UINT count, const D3D11_BUFFER_DESC& description, bool ownedFacts,
                         const char* failure) noexcept {
        // Ordinary UI inspection has no effect pass ID. Do not label it with an
        // invented pass; these bounded diagnostics describe actual effect pins.
        if (!pass || !log)
            return;
        try {
            const ConstantFailureKey key{pass,
                                         vertex,
                                         slot,
                                         offset,
                                         components,
                                         count,
                                         description.ByteWidth,
                                         static_cast<unsigned>(description.Usage),
                                         ownedFacts,
                                         failure};
            {
                std::lock_guard lock(statusMutex);
                if (constantFailures.size() >= 16 ||
                    std::find(constantFailures.begin(), constantFailures.end(), key) !=
                        constantFailures.end())
                    return;
                constantFailures.push_back(key);
            }
            char message[512];
            std::snprintf(message, sizeof(message),
                          "capture.constant-unavailable pass=0x%llX stage=%s slot=%u pinOffset=%u "
                          "components=%u first=%u count=%u ByteWidth=%u usage=%u ownedFacts=%u failure=%s",
                          static_cast<unsigned long long>(pass), vertex ? "VS" : "PS", slot, offset,
                          components, first, count, description.ByteWidth,
                          static_cast<unsigned>(description.Usage), ownedFacts ? 1u : 0u, failure);
            log(message); // Never log buffer contents or call the logger while holding statusMutex.
        } catch (...) {
        }
    }
    bool constants(bool vertex, unsigned slot, unsigned offset, unsigned components,
                   std::array<float, 4>& output, uint64_t pass = 0) {
        output = {};
        ComPtr<ID3D11Buffer> bound;
        UINT first = 0, count = 0;
        proof::ShadowReadInfo metadata;
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
            metadata.failure = "slot-range";
        else {
            if (vertex)
                graphics->context11()->VSGetConstantBuffers1(slot, 1, &bound, &first, &count);
            else
                graphics->context11()->PSGetConstantBuffers1(slot, 1, &bound, &first, &count);
            // The capture owner already proved no active query/predication before
            // invoking shader policy. The helper still checks its copy predicate.
            if (shadow->read(bound.Get(), first, count, offset, components, output, metadata))
                return true;
        }
        constantFailure(pass, vertex, slot, offset, components, first, count, metadata.description,
                        metadata.owned, metadata.failure ? metadata.failure : "bound-read-failure");
        return false;
    }
    bool constants(bool vertex, unsigned reg, std::array<float, 4>& output) {
        if (reg >= 4096) {
            output = {};
            return false;
        }
        return constants(vertex, 0, reg * 16, 4, output);
    }
    bool texture(unsigned resourceSlot, unsigned samplerSlot) {
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11SamplerState> sampler;
        graphics->context11()->PSGetShaderResources(resourceSlot, 1, &view);
        graphics->context11()->PSGetSamplers(samplerSlot, 1, &sampler);
        if (!view || !sampler)
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        view->GetDesc(&description);
        if (description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !unorm(description.Format))
            return false;
        D3D11_SAMPLER_DESC sampling{};
        sampler->GetDesc(&sampling);
        if ((static_cast<unsigned>(sampling.Filter) & 0x80u) != 0)
            return false; // Comparison sampling is a different contract.
        if (sampling.AddressU == D3D11_TEXTURE_ADDRESS_BORDER ||
            sampling.AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
            sampling.AddressW == D3D11_TEXTURE_ADDRESS_BORDER)
            for (float value : sampling.BorderColor)
                if (!unit(value))
                    return false;
        return true;
    }
    bool effectTexture(const EffectTexturePin& pin, unsigned& width, unsigned& height) {
        width = height = 0;
        if (pin.resourceSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
            pin.samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
            return false;
        auto* context = graphics->context11();
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11SamplerState> sampler;
        context->PSGetShaderResources(pin.resourceSlot, 1, &view);
        context->PSGetSamplers(pin.samplerSlot, 1, &sampler);
        if (!view)
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        view->GetDesc(&description);
        // The current capture owner leases one mip/layer per allocation. Do not
        // return its base dimensions for a different mip or a mipmapped read.
        if (description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            description.Texture2D.MostDetailedMip ||
            (description.Texture2D.MipLevels != 1 && description.Texture2D.MipLevels != UINT_MAX))
            return false;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture)))
            return false;
        D3D11_TEXTURE2D_DESC allocation{};
        texture->GetDesc(&allocation);
        if (allocation.MipLevels != 1 || allocation.ArraySize != 1 || allocation.SampleDesc.Count != 1 ||
            !allocation.Width || !allocation.Height)
            return false;
        if (!unorm(description.Format) && description.Format != DXGI_FORMAT_R16_FLOAT &&
            description.Format != DXGI_FORMAT_R16G16_FLOAT &&
            description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
            description.Format != DXGI_FORMAT_R32_FLOAT && description.Format != DXGI_FORMAT_R32G32_FLOAT &&
            description.Format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
            description.Format != DXGI_FORMAT_R11G11B10_FLOAT)
            return false;
        D3D11_SAMPLER_DESC sampling{};
        if (sampler)
            sampler->GetDesc(&sampling);
        else {
            sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampling.AddressU = sampling.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        }
        // Point/linear only. Anisotropy, comparison and reduction have different
        // footprints or arithmetic; the support propagator also checks this.
        if ((static_cast<unsigned>(sampling.Filter) & ~0x15u) != 0 ||
            sampling.AddressU < D3D11_TEXTURE_ADDRESS_WRAP ||
            sampling.AddressU > D3D11_TEXTURE_ADDRESS_MIRROR_ONCE ||
            sampling.AddressV < D3D11_TEXTURE_ADDRESS_WRAP ||
            sampling.AddressV > D3D11_TEXTURE_ADDRESS_MIRROR_ONCE)
            return false;
        width = allocation.Width;
        height = allocation.Height;
        return true;
    }
    bool inspectEffect(ID3D11PixelShader* pixel, const CaptureScope& scope,
                       const capture::DrawArguments& arguments, CaptureSupport& support, std::string& reason,
                       std::array<char, 65>& shaderHash) {
        support = {};
        auto reject = [&](const char* text) {
            reason = text;
            return false;
        };
        if (scope.kind != CaptureScopeKind::DualColor && scope.kind != CaptureScopeKind::PartialWrite)
            return reject("Effect proof received a non-color scope");
        auto* context = graphics->context11();
        ComPtr<ID3D11PixelShader> current;
        ComPtr<ID3D11VertexShader> vertex;
        UINT pixelClasses = 0, vertexClasses = 0;
        context->PSGetShader(&current, nullptr, &pixelClasses);
        context->VSGetShader(&vertex, nullptr, &vertexClasses);
        if (current.Get() != pixel || pixelClasses || vertexClasses)
            return reject("Effect shader binding or linkage changed");
        ComPtr<ID3D11GeometryShader> geometry;
        context->GSGetShader(&geometry, nullptr, nullptr);
        ComPtr<ID3D11HullShader> hull;
        context->HSGetShader(&hull, nullptr, nullptr);
        ComPtr<ID3D11DomainShader> domain;
        context->DSGetShader(&domain, nullptr, nullptr);
        if (geometry || hull || domain)
            return reject("Effect proof has an unverified geometry stage");
        ShaderFacts ps, vs;
        if (!data(pixel, shaderKey, ps) || !data(vertex.Get(), shaderKey, vs))
            return reject("Effect shader predates DXBC observation");
        std::copy(std::begin(ps.hash), std::end(ps.hash), shaderHash.begin());
        // This engine-internal pair takes clip-space positions and raw UVs from
        // IA, not _MainTex_ST. Verify the real geometry instead of inventing CBs
        // or assuming that a matching Copy PS implies a fullscreen identity map.
        if (scope.passId == 0x1000 &&
            std::string_view(vs.hash) == "6B3ED052E7846FC4BA2BB450C0FA6466EA69567965217ED8C9BB3505CA151CE6" &&
            std::string_view(ps.hash) == "BC4AEE2598ED82ED1D584DC8B5B26AACC9628827F25FE707893BB2978AF09E23") {
            if (scope.kind != CaptureScopeKind::DualColor || !scope.fullOverwrite)
                return reject("Native clip blit requires a full-output color contract");
            unsigned width = 0, height = 0;
            const EffectTexturePin source{EffectTextureRole::Main, 0, 0};
            if (!effectTexture(source, width, height))
                return reject("Native clip blit source/SRV/sampler is unverified");
            return proof::inspectClipBlit(context, *shadow, arguments, width, height, support, reason);
        }
        const auto* vertexLayout = effectLayout(scope.passId, false, vs);
        const auto* pixelLayout = effectLayout(scope.passId, true, ps);
        if (!vertexLayout || !pixelLayout)
            return reject("Actual effect VS/PS is not pinned for this pass");
        if (!effectPair(scope.passId, vs, ps))
            return reject("Individually pinned effect shaders do not form a verified pair");
        const ReadEffectUniform readUniform = [&](bool vertexStage, const EffectUniformPin& pin,
                                                  std::array<float, 4>& output) {
            return constants(vertexStage, pin.bufferSlot, pin.byteOffset, pin.components, output,
                             scope.passId);
        };
        const ReadEffectTexture readTexture = [&](const EffectTexturePin& pin, unsigned& width,
                                                  unsigned& height) {
            return effectTexture(pin, width, height);
        };
        return resolveEffectSupport(scope, *vertexLayout, *pixelLayout, readUniform, readTexture, support,
                                    reason);
    }
    CaptureUiShaderPolicy inspect(ID3D11PixelShader* pixel, const char*& reason,
                                  std::array<char, 65>& shaderHash) {
        auto reject = [&](const char* text) {
            reason = text;
            return CaptureUiShaderPolicy{};
        };
        ShaderFacts ps;
        if (!data(pixel, shaderKey, ps))
            return reject("UI pixel shader predates proof observation");
        std::copy(std::begin(ps.hash), std::end(ps.hash), shaderHash.begin());
        if (ps.kind == Kind::Dashboard)
            return {true, false}; // Verified constant alpha=1; RGB is deliberately not certified.
        if (ps.kind == Kind::Unknown)
            return reject("Unverified UI pixel shader");
        auto* context = graphics->context11();
        ComPtr<ID3D11VertexShader> vertex;
        context->VSGetShader(&vertex, nullptr, nullptr);
        ComPtr<ID3D11GeometryShader> geometry;
        context->GSGetShader(&geometry, nullptr, nullptr);
        ComPtr<ID3D11HullShader> hull;
        context->HSGetShader(&hull, nullptr, nullptr);
        ComPtr<ID3D11DomainShader> domain;
        context->DSGetShader(&domain, nullptr, nullptr);
        if (geometry || hull || domain)
            return reject("UI proof cannot assume VS outputs through an unverified geometry stage");
        ShaderFacts vs;
        if (!data(vertex.Get(), shaderKey, vs))
            return reject("UI vertex shader is unverified");
        ComPtr<ID3D11InputLayout> layout;
        context->IAGetInputLayout(&layout);
        LayoutFacts input;
        if (!data(layout.Get(), layoutKey, input) || !input.color32)
            return reject("UI COLOR0 is not verified UNORM Color32");
        if (ps.kind == Kind::DefaultPremultiplied) {
            if (vs.kind != Kind::DefaultVs || !texture(0, 0))
                return reject("Default UI requires its exact VS and UNORM main texture/sampler");
            std::array<float, 4> color{}, sampleAdd{}, gamma{};
            // These offsets belong to each stage's actual b0 binding window.
            // The stages need not share a buffer, slice, or update revision.
            if (!constants(true, 0, 32, 4, color) || !constants(false, 0, 48, 4, sampleAdd) ||
                !constants(true, 0, 104, 1, gamma))
                return reject("Default UI tint/sample-add/gamma CB words are not observed at their bindings");
            if (const auto* failure = proof::defaultUiFailure(color, sampleAdd, gamma[0]))
                return reject(failure);
            CaptureUiShaderPolicy policy;
            policy.unitAlphaVerified = true;
            policy.nonnegativeRgbVerified = true;
            policy.finiteRgbVerified = true;
            return policy; // Existing contribution path; no new fragment/signed fallback.
        }
        if (ps.kind == Kind::Translucent) {
            std::array<float, 4> sampleAdd{};
            if (vs.kind != Kind::TranslucentVs || !texture(0, 0) || !constants(false, 2, sampleAdd) ||
                sampleAdd[3] != 0)
                return reject(
                    "Translucent alpha requires its exact VS, UNORM main texture and zero alpha sample-add");
            // Color32 proves alpha, not finite RGB. Use the explicit actual-
            // fragment fallback: surviving alpha-zero geometry may enter M,
            // while A and the original RGB draw retain their existing meaning.
            CaptureUiShaderPolicy policy;
            policy.unitAlphaVerified = true;
            policy.conservativeFragmentSupportAllowed = true;
            return policy;
        }
        const bool navigation = ps.kind == Kind::Navigation;
        const bool text = ps.kind == Kind::TextAlpha || ps.kind == Kind::TextAdditive;
        const bool atlas = navigation || text;
        const auto expectedVertex = navigation ? Kind::NavigationVs : text ? Kind::TextVs : Kind::WidgetVs;
        if (vs.kind != expectedVertex || !texture(0, atlas ? 1 : 0) || (atlas && !texture(1, 0)))
            return reject("UI shader pair or sampled texture/sampler range is unverified");
        std::array<float, 4> color{}, parameters{};
        if (!constants(true, atlas ? 3 : 2, color) || !constants(false, atlas ? 4 : 3, parameters))
            return reject("UI constant-buffer cells are not yet observed at this binding offset");
        if (!unit(color[3]))
            return reject("UI material alpha is outside [0,1]");
        if (navigation && !unit(parameters[1]))
            return reject("Navigation Sharp violates its alpha-range proof");
        if (ps.kind == Kind::WidgetAlpha &&
            (!unit(parameters[1]) || !std::isfinite(parameters[2]) || parameters[2] <= 0))
            return reject("Widget Sharp/AlphaPower violate the OUTPUT alpha-range proof");
        if (ps.kind == Kind::TextAlpha) {
            // This older text shader also uses sqrt(Multiplier) in its alpha
            // exponent, and selects a Sharp-bias before clamping its lower bound.
            if (!std::isfinite(parameters[0]) || parameters[0] < 0 || !std::isfinite(parameters[1]) ||
                !std::isfinite(parameters[2]))
                return reject("Text alpha exponent/sharp constants are unverified");
            const float sharp =
                std::max(.4f, parameters[1] > .65f ? parameters[1] - parameters[2] : parameters[1]);
            if (!unit(sharp))
                return reject("Text effective Sharp is outside its convex alpha proof");
        }
        // Zero alpha does not make NaN/Inf RGB harmless: FP32 blending may still
        // propagate it. Signed finite HDR is allowed, but every arithmetic bound
        // must hold independently of the nonnegative/additive policy.
        if (!std::isfinite(parameters[0]))
            return reject("UI RGB multiplier is not finite");
        bool nonnegative = parameters[0] >= 0;
        for (unsigned c = 0; c < 3; ++c) {
            if (!std::isfinite(color[c]) ||
                std::abs(static_cast<double>(color[c])) *
                        std::max(1.0, std::abs(static_cast<double>(parameters[0]))) * (atlas ? 8.0 : 2.0) >
                    std::numeric_limits<float>::max())
                return reject("UI RGB output lacks a finite arithmetic bound");
            nonnegative = nonnegative && color[c] >= 0;
        }
        return {true, nonnegative, false, true};
    }
};
UiShaderProof::UiShaderProof(std::shared_ptr<Dx11Dx12> graphics, std::function<void(const char*)> log) {
    if (!graphics)
        throw std::invalid_argument("UI proof requires the existing graphics bridge");
    auto next = std::make_shared<Impl>(std::move(graphics), std::move(log));
    auto& h = hooks();
    {
        std::lock_guard lock(h.mutex);
        if (!h.observer.expired())
            throw std::runtime_error("Another UI proof owner is active");
        h.observer = next;
        auto** methods = *reinterpret_cast<void***>(next->graphics->device11());
        install(h.buffer, methods[3], reinterpret_cast<void*>(createBuffer));
        install(h.layout, methods[11], reinterpret_cast<void*>(createLayout));
        install(h.vertex, methods[12], reinterpret_cast<void*>(createVertex));
        install(h.pixel, methods[15], reinterpret_cast<void*>(createPixel));
    }
    capture::attachWrites(next->shadow);
    impl_ = std::move(next);
}
UiShaderProof::~UiShaderProof() {
    // Explicit stop owns graphics commands and its result. Constructor unwind
    // or an omitted stop must not start a drain from a destructor; the shadow
    // retains any unproven GPU work without issuing context commands.
    if (impl_)
        capture::detachWrites(impl_->shadow.get());
}
bool UiShaderProof::stop() noexcept {
    if (!impl_)
        return true;
    capture::detachWrites(impl_->shadow.get());
    try {
        auto lock = impl_->graphics->lock();
        if (impl_->stopped)
            return impl_->retired;
        impl_->stopped = true;
        try {
            impl_->retired = impl_->shadow->stop();
            if (!impl_->retired) {
                // Only shutdown waits. Admission is already closed, and this
                // same-context drain covers every issued cold-copy signal.
                impl_->graphics->drain();
                impl_->retired = impl_->shadow->stop();
            }
        } catch (...) {
            // Unknown retirement remains charged/retained and propagates to
            // FrameCapture and the presentation owner, not just this log.
        }
        const auto s = impl_->shadow->stats();
        if (impl_->log) {
            char message[1024];
            std::snprintf(
                message, sizeof(message),
                "capture.constant-shadow bytes=%llu peak=%llu WCbytes=%llu WCcopies=%llu WCns=%llu "
                "queued=%llu published=%llu stale=%llu pending=%llu failed=%llu attachFailures=%llu "
                "IAhits=%llu IAmisses=%llu IApages=%llu IAevicted=%llu IAWCbytes=%llu IAWCcopies=%llu "
                "retired=%u",
                static_cast<unsigned long long>(s.bytes), static_cast<unsigned long long>(s.peakBytes),
                static_cast<unsigned long long>(s.wcBytes), static_cast<unsigned long long>(s.wcCopies),
                static_cast<unsigned long long>(s.wcNanoseconds), static_cast<unsigned long long>(s.queued),
                static_cast<unsigned long long>(s.published), static_cast<unsigned long long>(s.stale),
                static_cast<unsigned long long>(s.pending), static_cast<unsigned long long>(s.failed),
                static_cast<unsigned long long>(s.attachmentFailures),
                static_cast<unsigned long long>(s.iaReadHits),
                static_cast<unsigned long long>(s.iaReadMisses),
                static_cast<unsigned long long>(s.iaResidentPages),
                static_cast<unsigned long long>(s.iaEvictedPages),
                static_cast<unsigned long long>(s.iaWcBytes), static_cast<unsigned long long>(s.iaWcCopies),
                impl_->retired ? 1u : 0u);
            try {
                impl_->log(message);
            } catch (...) {
            }
        }
        return impl_->retired;
    } catch (...) {
        return false;
    }
}
std::string UiShaderProof::shaderFingerprint(ID3D11DeviceChild* shader) {
    ShaderFacts facts;
    return data(shader, shaderKey, facts) ? facts.hash : "unobserved";
}
void UiShaderProof::beginFrame() {
    std::lock_guard lock(impl_->statusMutex);
    impl_->observation.reason.clear();
    impl_->observation.shaderHash.clear();
}
CaptureUiShaderPolicy UiShaderProof::inspect(ID3D11PixelShader* shader) noexcept {
    try {
        if (impl_->stopped)
            return {};
        impl_->shadow->collect();
        const char* reason = "";
        std::array<char, 65> shaderHash{};
        const auto result = impl_->inspect(shader, reason, shaderHash);
        std::lock_guard lock(impl_->statusMutex);
        ++impl_->observation.inspected;
        if (result.unitAlphaVerified)
            ++impl_->observation.accepted;
        if (impl_->observation.reason.empty()) {
            impl_->observation.reason = reason;
            impl_->observation.shaderHash = shaderHash.data();
        }
        return result;
    } catch (const std::exception& error) {
        std::lock_guard lock(impl_->statusMutex);
        ++impl_->observation.inspected;
        if (impl_->observation.reason.empty())
            impl_->observation.reason = error.what();
    } catch (...) {
    }
    return {};
}
bool UiShaderProof::inspectEffect(ID3D11PixelShader* shader, const CaptureScope& scope,
                                  const capture::DrawArguments& arguments, CaptureSupport& support) noexcept {
    support = {};
    try {
        if (impl_->stopped)
            return false;
        impl_->shadow->collect();
        std::string reason;
        std::array<char, 65> shaderHash{};
        const bool accepted = impl_->inspectEffect(shader, scope, arguments, support, reason, shaderHash);
        if (!accepted)
            impl_->effectFailure(shader, scope, arguments, reason);
        std::lock_guard lock(impl_->statusMutex);
        ++impl_->observation.inspected;
        if (accepted)
            ++impl_->observation.accepted;
        if (impl_->observation.reason.empty()) {
            impl_->observation.reason = std::move(reason);
            impl_->observation.shaderHash = shaderHash.data();
        }
        return accepted;
    } catch (const std::exception& error) {
        try {
            std::lock_guard lock(impl_->statusMutex);
            ++impl_->observation.inspected;
            if (impl_->observation.reason.empty())
                impl_->observation.reason = error.what();
        } catch (...) {
        }
    } catch (...) {
    }
    support = {};
    return false;
}
UiProofStatus UiShaderProof::status() const {
    std::lock_guard lock(impl_->statusMutex);
    auto result = impl_->observation;
    const auto shadow = impl_->shadow->stats();
    result.copiedConstantBytes = shadow.copiedBytes;
    result.invalidations = shadow.invalidations;
    return result;
}
} // namespace dspaa
