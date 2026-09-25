#pragma once
#include "graphics/dx11-dx12.h"
#include <functional>
#include <memory>

namespace dspaa {
// Opt-in diagnostics only. Copies whole same-frame camera boundary images into
// bounded private staging resources; never changes an application binding/image.
class CaptureTrace {
  public:
    CaptureTrace(std::shared_ptr<Dx11Dx12> graphics, std::function<void(const char*)> log);
    ~CaptureTrace();
    void observe(uint64_t frame, uint64_t generation, uint64_t pass, ID3D11RenderTargetView* view) noexcept;
    void collect() noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dspaa
