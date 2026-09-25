#pragma once
#include "owner.h"
#include <d3dcompiler.h>
#include <array>

namespace dspaa::capture {
using Microsoft::WRL::ComPtr;
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool usedThisFrame = false;
};
using ImagePtr = std::shared_ptr<Image>;
using AllocateImage = std::function<ImagePtr(unsigned, unsigned, DXGI_FORMAT)>;
// Captures every binding that these helpers can change, including SRVs which
// output binding can implicitly null on OTHER shader stages. UAV/SO/predication
// are rejected rather than resetting counters which cannot be queried back.
class StateGuard {
  public:
    explicit StateGuard(ID3D11DeviceContext4* context);
    ~StateGuard();
    StateGuard(const StateGuard&) = delete;
    std::array<ID3D11RenderTargetView*, 8> targets{};
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11BlendState> blend;
    std::array<float, 4> blendFactor{};
    UINT sampleMask = 0, stencilReference = 0;
    ComPtr<ID3D11DepthStencilState> depthState;
    std::array<std::array<ID3D11ShaderResourceView*, 128>, 6> resources{};
    ComPtr<ID3D11PixelShader> pixelShader;
    bool pureRaster = true;
    void restore() noexcept;
  private:
    ID3D11DeviceContext4* context;
    bool restored = false;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    std::array<std::array<ID3D11ClassInstance*, 256>, 5> classes{};
    std::array<UINT, 5> classCounts{256, 256, 256, 256, 256};
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    std::array<D3D11_VIEWPORT, 16> viewports{};
    std::array<D3D11_RECT, 16> scissors{};
    UINT viewportCount = 16, scissorCount = 16;
    ComPtr<ID3D11Buffer> pixelConstants;
    UINT firstConstant = 0, constantCount = 0;
    ComPtr<ID3D11SamplerState> sampler;
};
class Gpu {
  public:
    Gpu(ID3D11Device5* device, ID3D11DeviceContext4* context);
    ImagePtr create(unsigned width, unsigned height, DXGI_FORMAT format);
    ImagePtr clone(ID3D11Texture2D* source);
    ComPtr<ID3D11ShaderResourceView> view(const Image& image, ID3D11ShaderResourceView* original);
    ComPtr<ID3D11RenderTargetView> view(const Image& image, ID3D11RenderTargetView* original);
    void clear(const Image& image, float value);
    void seedUi(const Image& transmittance, const Image& ui);
    void resetUiRgb(const Image& previous, const Image& destination);
    // conservativeFragments selects a clear-1/zero-alpha sink instead of the
    // ordinary clear-0/MAX-alpha support. Both are unioned with prior influence.
    void extractUi(const Image& ui, const Image* alphaSupport, const Image& previousInfluence, const Image& transmittance, const Image& influence,
                   bool conservativeFragments = false);
    void opacity(const Image& transmittance, const Image& destination);
    ImagePtr support(const Image& source, const CaptureSamplingInput& input, const Image& destinationShape,
                     const D3D11_SAMPLER_DESC& sampler, const AllocateImage& allocate);
    ImagePtr occlusion(const Image& source, const CaptureSampleDomain& transform, const Image& destinationShape,
                       const D3D11_SAMPLER_DESC& sampler, const AllocateImage& allocate);
    void unite(const Image& first, const Image& second, const Image& destination);
    static bool supported(ID3D11Texture2D* texture);
    static D3D11_SAMPLER_DESC samplerDescription(ID3D11DeviceContext* context, unsigned slot);
    uint64_t drawCount() const noexcept { return draws; }
  private:
    struct Parameters {
        std::array<float, 4> matrix{1, 0, 0, 1};
        std::array<float, 4> biasOffset{};
        std::array<int, 4> sizes{};
        std::array<int, 4> mode{};
        std::array<int, 4> limits{};
    };
    ID3D11Device5* device;
    ID3D11DeviceContext4* context;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> seed, resetRgb, extractT, extractM, invert, maximum, dilate, affineMax, affineT;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11BlendState> overwrite;
    uint64_t draws = 0;
    void draw(ID3D11PixelShader* program, const Image& output, const Image* first, const Image* second,
              const Parameters& parameters, ID3D11SamplerState* sampler = nullptr, const Image* third = nullptr);
};
} // namespace dspaa::capture
