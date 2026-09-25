#pragma once
#include "capture-api.h"
#include "capture/owner.h"
#include "facade.h"
#include <array>
#include <string>

namespace dspaa {
struct PresentationSubmission;
struct CaptureCommand {
    DspAaCaptureCommand metadata{};
    CaptureTexture source, previous;
    CaptureScope scope;
    std::string basis;
};
std::unique_ptr<CaptureCommand> copyCaptureCommand(const DspAaCaptureCommand* command,
    const DspAaCaptureInput* inputs,const DspAaCaptureDomain* domains,const char* basis);
class FrameCapture {
  public:
    FrameCapture(std::shared_ptr<Dx11Dx12> graphics,PresentationLog log);
    ~FrameCapture();
    void surface(ID3D11Texture2D* shadow,uint64_t generation);
    // Abort capture, retire its original-application reads, and drop internal
    // shadow refs before the facade checks the application's ResizeBuffers contract.
    bool prepareSurfaceChange() noexcept;
    void begin(uint64_t applicationFrameId,bool enabled) noexcept;
    void execute(const CaptureCommand& command) noexcept;
    void finish(PresentationSubmission& submission) noexcept;
    DspAaCaptureStatus status() const;
    DspAaDepthCopyResult depthCopyResult() const;
    bool stop() noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void* queueCaptureCommand(const DspAaCaptureCommand* command,const DspAaCaptureInput* inputs,
    const DspAaCaptureDomain* domains,const char* basis);
void cancelCaptureCommand(void* token) noexcept;
} // namespace dspaa
