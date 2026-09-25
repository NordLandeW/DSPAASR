#include "backend.h"
#include "fsr/presenter.h"
#include "sl/presenter.h"

namespace dspaa {
namespace {
template<class Presenter> class SdkBackend : public PresentBackend {
  protected:
    std::unique_ptr<Presenter> presenter;
  public:
    HRESULT resize(const DXGI_SWAP_CHAIN_DESC1& desc, uint64_t generation) noexcept override { return presenter->resize(desc, generation); }
    PresentRetirement stop() noexcept override { return presenter->stop(); }
    IDXGISwapChain4* queries() const noexcept override { return presenter->swapChainForQueries(); }
    HRESULT setFullscreen(BOOL value, IDXGIOutput* output) noexcept override { return presenter->setFullscreenState(value, output); }
    HRESULT resizeTarget(const DXGI_MODE_DESC* value) noexcept override { return value ? presenter->resizeTarget(*value) : E_INVALIDARG; }
    HRESULT setColorSpace(DXGI_COLOR_SPACE_TYPE value) noexcept override { return presenter->setColorSpace(value); }
    HRESULT setHdrMetadata(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) noexcept override { return presenter->setHdrMetadata(type, size, data); }
    HRESULT setMaximumFrameLatency(UINT value) noexcept override { return presenter->setMaximumFrameLatency(value); }
    HRESULT setSourceSize(UINT width, UINT height) noexcept override { return presenter->setSourceSize(width, height); }
    HRESULT setRotation(DXGI_MODE_ROTATION value) noexcept override { return presenter->setRotation(value); }
    HRESULT setMatrixTransform(const DXGI_MATRIX_3X2_F* value) noexcept override { return value ? presenter->setMatrixTransform(*value) : E_INVALIDARG; }
    HRESULT setBackgroundColor(const DXGI_RGBA* value) noexcept override { return value ? presenter->setBackgroundColor(*value) : E_INVALIDARG; }
};
class FsrBackend final : public SdkBackend<FsrPresenter> {
  public:
    explicit FsrBackend(const PresenterCreateInfo& value) {
        FsrPresenterCreateInfo info;
        info.window = value.window; info.factory = value.factory; info.device = value.device; info.gameQueue = value.queue;
        info.swapChain = value.description; info.fullscreen = value.fullscreen; info.runtimeDirectory = value.runtimeDirectory;
        info.generation = value.generation;
        presenter = std::make_unique<FsrPresenter>(info);
    }
    HRESULT present(FrameLease frame, const PresentArguments& arguments, bool generate) noexcept override { return presenter->present(std::move(frame), arguments, generate); }
    BackendObservation observation() const override {
        const auto s = presenter->status(); BackendObservation result;
        result.generating = s.generationActive; result.maximumGeneratedFrames = 1;
        result.applicationPresents = s.applicationPresents; result.generatedSubmissions = s.generatedCallbacks;
        result.quarantined = s.quarantined; result.reason = s.reason; return result;
    }
};
class SlBackend final : public SdkBackend<SlPresenter> {
    std::shared_ptr<SlRuntime> runtime;
    SlGenerationRequest request;
  public:
    SlBackend(const PresenterCreateInfo& value, std::shared_ptr<SlRuntime> service) : runtime(std::move(service)) {
        SlPresenterCreateInfo info;
        info.runtime = runtime; info.window = value.window; info.swapChain = value.description;
        info.fullscreen = value.fullscreen; info.generation = value.generation;
        presenter = std::make_unique<SlPresenter>(info);
    }
    void configure(const PresentationConfiguration& value) override {
        request.mode = value.mode ? SlGenerationMode::Dynamic : SlGenerationMode::Fixed;
        request.generatedFrames = value.generatedFrames; request.dynamicTargetFrameRate = value.dynamicTargetFrameRate;
        if (!runtime->setReflex(static_cast<SlReflexMode>(value.reflex), value.frameLimitMicroseconds))
            throw std::runtime_error("Could not apply the selected Reflex mode/limiter");
    }
    HRESULT present(FrameLease frame, const PresentArguments& arguments, bool generate) noexcept override {
        auto current = request; if (!generate) current.mode = SlGenerationMode::Off;
        return presenter->present(std::move(frame), arguments, current);
    }
    BackendObservation observation() const override {
        const auto s = presenter->status(); BackendObservation result;
        result.generating = s.active != SlGenerationMode::Off; result.mode = s.active == SlGenerationMode::Dynamic ? 1u : 0u;
        result.maximumGeneratedFrames = s.maximumGeneratedFrames; result.dynamic = s.dynamicSupported; result.vsync = s.vsyncSupported;
        result.sdkStatus = s.sdkStatus; result.applicationPresents = s.applicationPresents; result.sdkReportedPresents = s.sdkReportedPresents;
        result.generatedSubmissions = s.sdkReportedPresents > s.applicationPresents ? s.sdkReportedPresents - s.applicationPresents : 0;
        result.quarantined = s.quarantined; result.reason = s.reason; return result;
    }
};
} // namespace
std::unique_ptr<PresentBackend> createFsrBackend(const PresenterCreateInfo& info) { return std::make_unique<FsrBackend>(info); }
std::unique_ptr<PresentBackend> createSlBackend(const PresenterCreateInfo& info, std::shared_ptr<SlRuntime> runtime) {
    return std::make_unique<SlBackend>(info, std::move(runtime));
}
} // namespace dspaa
