#pragma once
#include "runtime.h"
#include "graphics/dx11-dx12.h"
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace dspaa::detail {
using Microsoft::WRL::ComPtr;
constexpr DWORD slTimeoutMilliseconds = 30000;
void slCheck(sl::Result result, const char* operation);
std::string slError(sl::Result result);
struct SlModule {
    HMODULE value = nullptr;
    ~SlModule() { if (value) FreeLibrary(value); }
};
struct SlFileLock {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~SlFileLock() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct SlControlWork {
    std::promise<void> done;
    std::future<void> completion = done.get_future();
    std::thread thread;
};
struct SlApi {
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slSetD3DDevice* setDevice = nullptr;
    PFun_slUpgradeInterface* upgrade = nullptr;
    PFun_slGetNativeInterface* native = nullptr;
    PFun_slIsFeatureSupported* supported = nullptr;
    PFun_slIsFeatureLoaded* loaded = nullptr;
    PFun_slSetFeatureLoaded* load = nullptr;
    PFun_slGetFeatureFunction* feature = nullptr;
    PFun_slGetNewFrameToken* newFrame = nullptr;
    PFun_slSetConstants* constants = nullptr;
    PFun_slSetTagForFrame* tags = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFun_slDLSSGGetState* dlssState = nullptr;
    PFun_slDLSSGSetOptions* dlssOptions = nullptr;
    PFun_slReflexSleep* sleep = nullptr;
    PFun_slReflexGetState* reflexState = nullptr;
    PFun_slReflexSetOptions* reflexOptions = nullptr;
    PFun_slPCLSetMarker* marker = nullptr;
    PFun_slPCLGetState* pclState = nullptr;
    PFun_slPCLSetOptions* pclOptions = nullptr;
};
struct SlTicket {
    const uint64_t applicationFrameId;
    const uint32_t sdkFrameId;
    sl::FrameToken* const token;
    bool begun = false, claimed = false, presented = false, generationReady = false;
    uint32_t markers = 0, pendingMarkers = 0;
    SlTicket(uint64_t application, sl::FrameToken* sdk)
        : applicationFrameId(application), sdkFrameId(static_cast<uint32_t>(*sdk)), token(sdk) {}
};
// Internal shared lifetime, not a second public SDK interface. The custom shared
// deleter intentionally retains this complete object when retirement is unknown.
struct SlRuntimeState {
    SlRuntimeCreateInfo creation;
    SlApi api;
    SlModule interposer, callbackModule;
    std::vector<std::unique_ptr<SlFileLock>> files;
    std::filesystem::path pluginPath, logPath;
    const wchar_t* pluginPaths[1]{};
    std::array<sl::Feature, 3> features{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    ComPtr<ID3D12Device> device, proxyDevice;
    ComPtr<IDXGIFactory> factory;
    ComPtr<IDXGIFactory2> proxyFactory;
    std::atomic<bool> initialized{false}, attached{false}, dlssLoaded{false};
    bool ownsProcess = false;
    std::atomic<bool> dlssSupported{false}, reflexSupported{false}, pclSupported{false};
    std::atomic<bool> lowLatencyAvailable{false}, flashIndicatorDriverControlled{false};
    std::atomic<uint32_t> statsWindowMessage{0};
    std::atomic<bool> active{false}, quarantined{false}, reflexConfigured{false};
    std::atomic<SlReflexMode> reflexMode{SlReflexMode::Off};
    void* presenterOwner = nullptr;
    mutable std::mutex frameMutex, sdkMutex, errorMutex;
    std::condition_variable callsFinished;
    unsigned frameCalls = 0;
    uint64_t lastBegun = 0, accepted = 0, rejected = 0, tokenAllocations = 0;
    bool haveBegun = false;
    std::map<uint64_t, std::shared_ptr<SlTicket>> tickets;
    std::array<std::weak_ptr<SlTicket>, sl::MAX_FRAMES_IN_FLIGHT> tokenLeases;
    std::string reason;
    std::unique_ptr<SlControlWork> control;

    explicit SlRuntimeState(const SlRuntimeCreateInfo& info) : creation(info) {}
    void initialize();
    void attach(ID3D12Device* nativeDevice, IDXGIFactory* nativeFactory);
    void featureFunctions();
    void activate(void* owner);
    void deactivate(void* owner); // Only after chain/proxy queues have been retired and released.
    void stopFrameCalls();
    void applyReflex(SlReflexMode mode, uint32_t frameLimitMicroseconds); // Caller holds sdkMutex.
    void leaveFrameCall() noexcept;
    bool begin(uint64_t applicationFrameId);
    bool mark(uint64_t applicationFrameId, SlMarker marker);
    bool setReflex(SlReflexMode mode, uint32_t frameLimitMicroseconds);
    std::shared_ptr<SlTicket> claim(uint64_t applicationFrameId);
    bool readyForPresent(const std::shared_ptr<SlTicket>& ticket);
    void presented(const std::shared_ptr<SlTicket>& ticket);
    void clearTags(const SlTicket& ticket);
    void pruneLocked();
    void report(const char* text) noexcept;
    void quarantine(const char* text) noexcept;
    SlRuntimeStatus status() const;
    void bounded(std::function<void()> operation);
    PresentRetirement stop() noexcept;
};
} // namespace dspaa::detail
