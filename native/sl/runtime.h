#pragma once
#include "present/frame.h"
#include <filesystem>
#include <memory>
#include <string>

namespace dspaa {
namespace detail { struct SlRuntimeState; }
class SlPresenter;
enum class SlMarker {
    SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd,
    PresentStart, PresentEnd, LatencyPing, TriggerFlash
};
enum class SlReflexMode { Off, On, OnWithBoost };
struct SlRuntimeCreateInfo {
    std::filesystem::path runtimeDirectory, logDirectory;
    std::string projectId = "cd751b03-626f-4384-a83e-30279c066322";
    std::string engineVersion = "2022.3.62f3c1";
};
struct SlRuntimeStatus {
    bool initialized = false, attached = false, presentationActive = false;
    bool dlssSupported = false, reflexSupported = false, pclSupported = false;
    bool lowLatencyAvailable = false, flashIndicatorDriverControlled = false, quarantined = false;
    uint32_t statsWindowMessage = 0;
    uint64_t acceptedFrameTickets = 0, rejectedFrameTickets = 0;
    std::string reason;
};
// Construct before any owned presentation-device/chain hooks, never in DllMain.
// Attach, presenter activation/deactivation and shutdown require the caller's
// graphics-quiescent lifecycle boundary; that cannot be inferred from a mutex
// inside this SDK adapter. Device/factory ownership is retained until shutdown.
class SlRuntime {
  public:
    explicit SlRuntime(const SlRuntimeCreateInfo& info);
    ~SlRuntime();
    SlRuntime(const SlRuntime&) = delete;
    SlRuntime& operator=(const SlRuntime&) = delete;
    void attach(ID3D12Device* nativeDevice, IDXGIFactory* nativeFactory);
    // Main thread, before input sampling. Allocates at most one immutable ticket
    // for this application ID and calls Reflex sleep even when Reflex mode is Off.
    // Inactive Native/FSR paths do neither. Never call this from Present.
    bool beginFrame(uint64_t applicationFrameId) noexcept;
    // Actual engine timing only; the presenter does not manufacture or duplicate
    // these markers. The host uses PresentArguments::boundary for RenderSubmitEnd,
    // PresentStart and PresentEnd at the actual chain boundary, also on FG-Off frames.
    bool marker(uint64_t applicationFrameId, SlMarker marker) noexcept;
    bool setReflex(SlReflexMode mode, uint32_t frameLimitMicroseconds = 0) noexcept;
    SlRuntimeStatus status() const;
    // All presenters must already have proved retirement. Quarantine retains the
    // whole SDK owner and forbids reinitialization in this process.
    PresentRetirement shutdown() noexcept;
  private:
    std::shared_ptr<detail::SlRuntimeState> state_;
    friend class SlPresenter;
};
} // namespace dspaa
