#pragma once
#include "api.h"
#include "facade.h"
#include "frame.h"
#include "sl/runtime.h"
#include <atomic>
#include <d3d11.h>
#include <mutex>
#include <optional>

namespace dspaa {
class WorldColor;
struct PresentationConfiguration {
    uint32_t backend = 0, mode = 0, generatedFrames = 1, reflex = 1;
    float dynamicTargetFrameRate = 0;
    uint32_t frameLimitMicroseconds = 0;
    uint64_t revision = 0;
};
struct PresentationSubmission {
    DspAaPresentationInputs metadata{};
    std::array<Microsoft::WRL::ComPtr<ID3D11Resource>, 5> resources;
    std::shared_ptr<const void> worldLifetime;
    // Evidence from the captured world RTV, not inferred from swapchain color space.
    bool hudlessSrgbView = false;
};
struct BackendObservation {
    bool generating = false, dynamic = false, vsync = false, quarantined = false;
    uint32_t mode = 0, maximumGeneratedFrames = 0, sdkStatus = 0;
    uint64_t applicationPresents = 0, generatedSubmissions = 0, sdkReportedPresents = 0;
    std::string reason;
};
// The mailbox never owns a DXGI chain or calls its controls. The facade is its
// sole graphics owner. Main-thread sleep never holds the mailbox lock, so the
// preceding render frame can still complete its Present.
class PresentationChannel {
  public:
    PresentationChannel(std::shared_ptr<SlRuntime> runtime, bool fsrAvailable, PresentationLog log);
    bool configure(const DspAaPresentationConfiguration& configuration);
    PresentationConfiguration configuration() const;
    bool begin(DspAaPresentationBegin& frame);
    bool marker(uint64_t id, SlMarker marker) noexcept;
    void publish(std::unique_ptr<PresentationSubmission> submission);
    std::unique_ptr<PresentationSubmission> consume();
    void surface(uint64_t generation, uint32_t width, uint32_t height);
    void observe(uint32_t backend, uint64_t frame, const BackendObservation& observation);
    void reason(const char* text, bool quarantined = false);
    void detach() noexcept;
    DspAaPresentationStatus status() const;
    std::shared_ptr<SlRuntime> runtime() const {
        return runtime_;
    }
    void bindWorldColor(const std::shared_ptr<WorldColor>& world);
    std::shared_ptr<WorldColor> worldColor() const;

  private:
    std::shared_ptr<SlRuntime> runtime_;
    PresentationLog log_;
    mutable std::mutex mutex_;
    PresentationConfiguration configuration_;
    std::weak_ptr<WorldColor> world_;
    std::unique_ptr<PresentationSubmission> pending_;
    DspAaPresentationStatus status_{};
    uint64_t nextFrame_ = 0, lastPublished_ = 0;
    bool attached_ = true;
};
// Called once at the authorized early entry, before presentation hooks/devices.
// Missing/unavailable SL never prevents the independent Native or FSR paths.
void initializePresentationRuntime(const std::filesystem::path& runtime, const std::filesystem::path& logs,
                                   PresentationLog log) noexcept;
std::shared_ptr<SlRuntime> earlyPresentationRuntime();
std::string presentationRuntimeFailure();
std::shared_ptr<PresentationChannel> registerPresentationChannel(const std::filesystem::path& runtime,
                                                                 PresentationLog log);
std::shared_ptr<PresentationChannel> presentationChannel();
void* queuePresentationInputs(const DspAaPresentationInputs* frame);
void cancelPresentationInputs(void* token);
DspAaRenderEvent presentationRenderEvent();
} // namespace dspaa
