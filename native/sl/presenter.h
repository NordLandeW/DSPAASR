#pragma once
#include "runtime.h"
#include <optional>
#include <stdexcept>

namespace dspaa {
enum class SlGenerationMode { Off, Fixed, Dynamic };
struct SlGenerationRequest {
    SlGenerationMode mode = SlGenerationMode::Off;
    // Additional generated frames: 1 means 2x, 3 means 4x. Not a multiplier.
    unsigned generatedFrames = 1;
    float dynamicTargetFrameRate = 0; // Zero asks the SDK to use the monitor.
};
struct SlPresenterCreateInfo {
    std::shared_ptr<SlRuntime> runtime;
    HWND window = nullptr;
    DXGI_SWAP_CHAIN_DESC1 swapChain{};
    std::optional<DXGI_SWAP_CHAIN_FULLSCREEN_DESC> fullscreen;
    uint64_t generation = 0;
};
struct SlPresenterCreationError : std::runtime_error {
    const PresentRetirement retirement;
    SlPresenterCreationError(const std::string& message, PresentRetirement state)
        : std::runtime_error(message), retirement(state) {}
};
struct SlPresenterStatus {
    SlGenerationMode requested = SlGenerationMode::Off, active = SlGenerationMode::Off;
    unsigned maximumGeneratedFrames = 0, minimumDimension = 0;
    bool dynamicSupported = false, vsyncSupported = false, quarantined = false;
    uint32_t sdkStatus = 0;
    HRESULT asynchronousPresentError = S_OK;
    uint64_t applicationPresents = 0, sdkReportedPresents = 0, lastApplicationFrameId = 0;
    std::string reason;
};
// Externally serialize every public presenter operation. Main-thread BeginFrame
// and marker calls on its runtime may overlap; lifecycle changes require engine
// quiescence. This owner creates its queue through the upgraded proxy device and
// owns the HWND's sole real swapchain. No other backend may overlap it.
class SlPresenter {
  public:
    explicit SlPresenter(const SlPresenterCreateInfo& info);
    ~SlPresenter();
    SlPresenter(const SlPresenter&) = delete;
    SlPresenter& operator=(const SlPresenter&) = delete;
    HRESULT present(FrameLease frame, const PresentArguments& arguments, const SlGenerationRequest& request) noexcept;
    HRESULT resize(const DXGI_SWAP_CHAIN_DESC1& description, uint64_t generation) noexcept;
    PresentRetirement stop() noexcept;
    HRESULT setFullscreenState(BOOL fullscreen, IDXGIOutput* output) noexcept;
    HRESULT resizeTarget(const DXGI_MODE_DESC& description) noexcept;
    HRESULT setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept;
    HRESULT setHdrMetadata(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) noexcept;
    HRESULT setMaximumFrameLatency(UINT latency) noexcept;
    HRESULT setSourceSize(UINT width, UINT height) noexcept;
    HRESULT setRotation(DXGI_MODE_ROTATION rotation) noexcept;
    HRESULT setMatrixTransform(const DXGI_MATRIX_3X2_F& matrix) noexcept;
    HRESULT setBackgroundColor(const DXGI_RGBA& color) noexcept;
    // Query-only borrowed native interface; never route hooked APIs through it.
    // A latency handle may be requested ONLY if the create flags assigned its
    // ownership to the application. The caller closes that returned handle.
    IDXGISwapChain4* swapChainForQueries() const noexcept;
    SlPresenterStatus status() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dspaa
