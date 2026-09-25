#pragma once
#include "frame.h"
#include "channel.h"
#include <filesystem>
#include <optional>

namespace dspaa {
struct PresenterCreateInfo {
    HWND window = nullptr;
    IDXGIFactory2* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    DXGI_SWAP_CHAIN_DESC1 description{};
    std::optional<DXGI_SWAP_CHAIN_FULLSCREEN_DESC> fullscreen;
    IDXGIOutput* restrictOutput = nullptr;
    std::filesystem::path runtimeDirectory;
    uint64_t generation = 0;
};
// Exactly one externally serialized owner controls a real chain. Query views do
// not transfer Present/Resize/teardown authority to the D3D11-facing facade.
class PresentBackend {
  public:
    virtual ~PresentBackend() = default;
    virtual HRESULT present(FrameLease frame, const PresentArguments& arguments, bool generate) noexcept = 0;
    virtual HRESULT resize(const DXGI_SWAP_CHAIN_DESC1& description, uint64_t generation) noexcept = 0;
    virtual PresentRetirement stop() noexcept = 0;
    virtual IDXGISwapChain4* queries() const noexcept = 0;
    virtual void configure(const PresentationConfiguration&) {}
    virtual BackendObservation observation() const { return {}; }
    virtual HRESULT setFullscreen(BOOL value, IDXGIOutput* output) noexcept = 0;
    virtual HRESULT resizeTarget(const DXGI_MODE_DESC* value) noexcept = 0;
    virtual HRESULT setColorSpace(DXGI_COLOR_SPACE_TYPE value) noexcept = 0;
    virtual HRESULT setHdrMetadata(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) noexcept = 0;
    virtual HRESULT setMaximumFrameLatency(UINT value) noexcept = 0;
    virtual HRESULT setSourceSize(UINT width, UINT height) noexcept = 0;
    virtual HRESULT setRotation(DXGI_MODE_ROTATION value) noexcept = 0;
    virtual HRESULT setMatrixTransform(const DXGI_MATRIX_3X2_F* value) noexcept = 0;
    virtual HRESULT setBackgroundColor(const DXGI_RGBA* value) noexcept = 0;
};
std::unique_ptr<PresentBackend> createNativePresenter(const PresenterCreateInfo& info);
std::unique_ptr<PresentBackend> createFsrBackend(const PresenterCreateInfo& info);
std::unique_ptr<PresentBackend> createSlBackend(const PresenterCreateInfo& info, std::shared_ptr<SlRuntime> runtime);
} // namespace dspaa
