#pragma once
#include <windows.h>
#include <memory>

namespace dspaa {
// Unity may cache its handle once for the lifetime of the facade. Keep that
// handle stable while the backend's independently owned waitable handle changes.
// Relay only requested application-frame grants, never spin-drain native signals.
class PresentationLatency {
  public:
    PresentationLatency();
    ~PresentationLatency();
    PresentationLatency(const PresentationLatency&) = delete;
    PresentationLatency& operator=(const PresentationLatency&) = delete;
    bool bind(HANDLE ownedBackendHandle) noexcept;
    bool pause() noexcept;
    HANDLE duplicateHostHandle() const noexcept;
    void presented() noexcept;
    bool active() const noexcept;
  private:
    struct Shared;
    struct Run;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<Run> run_;
};
} // namespace dspaa
