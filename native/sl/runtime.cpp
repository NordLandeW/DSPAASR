#include "internal.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace dspaa::detail {
namespace {
std::atomic<bool> processClaimed{false};
template<class Function> Function* exported(HMODULE module, const char* name) {
    auto* result = reinterpret_cast<Function*>(GetProcAddress(module, name));
    if (!result) throw std::runtime_error(std::string("Missing Streamline export: ") + name);
    return result;
}
template<class Function> Function* featureFunction(SlApi& api, sl::Feature feature, const char* name) {
    void* result = nullptr;
    slCheck(api.feature(feature, name, result), name);
    if (!result) throw std::runtime_error(std::string("Empty Streamline feature function: ") + name);
    return reinterpret_cast<Function*>(result);
}
sl::PCLMarker pclMarker(SlMarker value) {
    switch (value) {
    case SlMarker::SimulationStart: return sl::PCLMarker::eSimulationStart;
    case SlMarker::SimulationEnd: return sl::PCLMarker::eSimulationEnd;
    case SlMarker::RenderSubmitStart: return sl::PCLMarker::eRenderSubmitStart;
    case SlMarker::RenderSubmitEnd: return sl::PCLMarker::eRenderSubmitEnd;
    case SlMarker::PresentStart: return sl::PCLMarker::ePresentStart;
    case SlMarker::PresentEnd: return sl::PCLMarker::ePresentEnd;
    case SlMarker::LatencyPing: return sl::PCLMarker::ePCLatencyPing;
    case SlMarker::TriggerFlash: return sl::PCLMarker::eTriggerFlash;
    }
    throw std::invalid_argument("Invalid PCL marker");
}
struct FrameCall {
    SlRuntimeState* state;
    ~FrameCall() { state->leaveFrameCall(); }
};
constexpr uint32_t markerBit(SlMarker marker) { return 1u << static_cast<unsigned>(marker); }
void lockRuntimeFiles(SlRuntimeState& state) {
    constexpr const wchar_t* names[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
                                        L"sl.reflex.dll",     L"sl.pcl.dll",    L"nvngx_dlssg.dll"};
    for (const auto* name : names) {
        const auto path = state.pluginPath / name;
        auto file = std::make_unique<SlFileLock>();
        file->value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->value == INVALID_HANDLE_VALUE)
            graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Lock Streamline runtime DLL");
        // Prevent replacement while SDK workers may load/use these files. This
        // is lifetime ownership, not a hash, signature or release-version gate.
        state.files.push_back(std::move(file));
    }
}
} // namespace
std::string slError(sl::Result result) {
    switch (result) {
    case sl::Result::eOk: return "success";
    case sl::Result::eErrorDriverOutOfDate: return "NVIDIA driver does not meet the feature requirements";
    case sl::Result::eErrorOSOutOfDate: return "Windows does not meet the feature requirements";
    case sl::Result::eErrorOSDisabledHWS: return "Hardware-accelerated GPU scheduling is disabled";
    case sl::Result::eErrorNoSupportedAdapterFound:
    case sl::Result::eErrorAdapterNotSupported: return "The selected adapter does not support this feature";
    case sl::Result::eErrorFeatureNotSupported: return "The requested Streamline feature is unsupported";
    case sl::Result::eErrorFeatureMissing:
    case sl::Result::eErrorFeatureFailedToLoad: return "The requested Streamline feature did not load";
    case sl::Result::eErrorMissingConstants: return "Matching frame constants were not supplied";
    case sl::Result::eErrorDuplicatedConstants: return "Frame constants were supplied more than once";
    case sl::Result::eErrorNGXFailed: return "NGX initialization or execution failed";
    default: return "Streamline result " + std::to_string(static_cast<int>(result));
    }
}
void slCheck(sl::Result result, const char* operation) {
    if (result != sl::Result::eOk) throw std::runtime_error(std::string(operation) + ": " + slError(result));
}
void SlRuntimeState::report(const char* text) noexcept {
    try { std::lock_guard lock(errorMutex); reason = text; } catch (...) {}
}
void SlRuntimeState::quarantine(const char* text) noexcept {
    active.store(false); quarantined.store(true); report(text);
}
void SlRuntimeState::initialize() {
    if (GetModuleHandleW(L"sl.interposer.dll"))
        throw std::runtime_error("Another Streamline interposer is already loaded; its process-global state is not adopted");
    bool unclaimed = false;
    if (!processClaimed.compare_exchange_strong(unclaimed, true))
        throw std::runtime_error("This process already has a Streamline owner or quarantine");
    ownsProcess = true;
    pluginPath = std::filesystem::weakly_canonical(std::filesystem::absolute(creation.runtimeDirectory));
    if (!creation.logDirectory.empty()) logPath = std::filesystem::absolute(creation.logDirectory);
    if (creation.projectId.empty() || creation.engineVersion.empty())
        throw std::invalid_argument("Streamline requires this project's own Unity engine identity");
    lockRuntimeFiles(*this);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&slError), &callbackModule.value))
        graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Retain Streamline owner code");
    interposer.value = LoadLibraryExW((pluginPath / L"sl.interposer.dll").c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!interposer.value)
        graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Load Streamline interposer");
#define DSPAA_SL_EXPORT(field, name) api.field = exported<PFun_##name>(interposer.value, #name)
    DSPAA_SL_EXPORT(init, slInit); DSPAA_SL_EXPORT(shutdown, slShutdown);
    DSPAA_SL_EXPORT(setDevice, slSetD3DDevice); DSPAA_SL_EXPORT(upgrade, slUpgradeInterface);
    DSPAA_SL_EXPORT(native, slGetNativeInterface); DSPAA_SL_EXPORT(supported, slIsFeatureSupported);
    DSPAA_SL_EXPORT(loaded, slIsFeatureLoaded); DSPAA_SL_EXPORT(load, slSetFeatureLoaded);
    DSPAA_SL_EXPORT(feature, slGetFeatureFunction); DSPAA_SL_EXPORT(newFrame, slGetNewFrameToken);
    DSPAA_SL_EXPORT(constants, slSetConstants); DSPAA_SL_EXPORT(tags, slSetTagForFrame);
    DSPAA_SL_EXPORT(freeResources, slFreeResources);
#undef DSPAA_SL_EXPORT
    pluginPaths[0] = pluginPath.c_str();
    preferences.pathsToPlugins = pluginPaths; preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = logPath.empty() ? nullptr : logPath.c_str();
    preferences.showConsole = false; preferences.engine = sl::EngineType::eUnity;
    preferences.engineVersion = creation.engineVersion.c_str(); preferences.projectId = creation.projectId.c_str();
    preferences.applicationId = 0; preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.featuresToLoad = features.data(); preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    // Explicit flags: never inherit the SDK defaults which enable OTA/plugin replacement.
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eDisableDebugText;
    const auto result = api.init(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        // We rejected pre-existing modules above, so any partial initialization
        // belongs to this owner. A failed shutdown must retain its DLLs/state.
        const auto cleanup = api.shutdown();
        if (cleanup != sl::Result::eOk && cleanup != sl::Result::eErrorNotInitialized)
            quarantine("Streamline initialization and cleanup both failed");
        slCheck(result, "Initialize Streamline");
    }
    initialized = true;
}
void SlRuntimeState::featureFunctions() {
    if (reflexSupported) {
        api.sleep = featureFunction<PFun_slReflexSleep>(api, sl::kFeatureReflex, "slReflexSleep");
        api.reflexState = featureFunction<PFun_slReflexGetState>(api, sl::kFeatureReflex, "slReflexGetState");
        api.reflexOptions = featureFunction<PFun_slReflexSetOptions>(api, sl::kFeatureReflex, "slReflexSetOptions");
    }
    if (pclSupported) {
        api.marker = featureFunction<PFun_slPCLSetMarker>(api, sl::kFeaturePCL, "slPCLSetMarker");
        api.pclState = featureFunction<PFun_slPCLGetState>(api, sl::kFeaturePCL, "slPCLGetState");
        api.pclOptions = featureFunction<PFun_slPCLSetOptions>(api, sl::kFeaturePCL, "slPCLSetOptions");
    }
    if (dlssLoaded) {
        api.dlssState = featureFunction<PFun_slDLSSGGetState>(api, sl::kFeatureDLSS_G, "slDLSSGGetState");
        api.dlssOptions = featureFunction<PFun_slDLSSGSetOptions>(api, sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    }
}
void SlRuntimeState::attach(ID3D12Device* nativeDevice, IDXGIFactory* nativeFactory) {
    std::lock_guard sdkLock(sdkMutex);
    if (!initialized || quarantined || !nativeDevice || !nativeFactory)
        throw std::invalid_argument("Invalid Streamline attach lifecycle");
    if (attached) {
        if (device.Get() == nativeDevice && factory.Get() == nativeFactory) return;
        throw std::runtime_error("A live Streamline runtime cannot be moved to another device/factory");
    }
    device = nativeDevice; factory = nativeFactory;
    slCheck(api.setDevice(device.Get()), "Attach Streamline D3D12 device");
    auto luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    const auto supportedDlss = api.supported(sl::kFeatureDLSS_G, adapter);
    dlssSupported = supportedDlss == sl::Result::eOk;
    reflexSupported = api.supported(sl::kFeatureReflex, adapter) == sl::Result::eOk;
    pclSupported = api.supported(sl::kFeaturePCL, adapter) == sl::Result::eOk;
    if (!dlssSupported) report(slError(supportedDlss).c_str());
    // slUpgradeInterface creates a new owned proxy without consuming the input
    // native reference. Pass a BORROWED native pointer, then adopt the new proxy.
    void* upgraded = device.Get();
    slCheck(api.upgrade(&upgraded), "Upgrade Streamline D3D12 device");
    if (upgraded == device.Get()) throw std::runtime_error("Streamline did not supply the required device proxy");
    proxyDevice.Attach(static_cast<ID3D12Device*>(upgraded));
    upgraded = factory.Get();
    slCheck(api.upgrade(&upgraded), "Upgrade Streamline DXGI factory");
    if (upgraded == factory.Get()) throw std::runtime_error("Streamline did not supply the required factory proxy");
    ComPtr<IDXGIFactory> proxyBase;
    proxyBase.Attach(static_cast<IDXGIFactory*>(upgraded));
    graphicsCheck(proxyBase.As(&proxyFactory), "Query Streamline factory2 proxy");
    bool loaded = false;
    if (api.loaded(sl::kFeatureDLSS_G, loaded) == sl::Result::eOk && loaded) {
        slCheck(api.load(sl::kFeatureDLSS_G, false), "Leave DLSS FG unloaded before backend activation");
    }
    dlssLoaded = false;
    featureFunctions();
    if (reflexSupported) {
        sl::ReflexOptions options{};
        options.mode = sl::ReflexMode::eLowLatency;
        slCheck(api.reflexOptions(options), "Set initial Reflex options");
        sl::ReflexState state{};
        slCheck(api.reflexState(state), "Query Reflex capabilities");
        lowLatencyAvailable = state.lowLatencyAvailable;
        flashIndicatorDriverControlled = state.flashIndicatorDriverControlled;
    }
    if (pclSupported) {
        sl::PCLOptions options{}; // Use the custom message, never reserve F13-F15.
        slCheck(api.pclOptions(options), "Set initial PCL options");
        sl::PCLState state{};
        slCheck(api.pclState(state), "Query PCL ping message");
        statsWindowMessage = state.statsWindowMessage;
    }
    std::lock_guard lock(frameMutex);
    attached = true;
}
void SlRuntimeState::activate(void* owner) {
    std::lock_guard sdkLock(sdkMutex);
    if (!initialized || !attached || quarantined || presenterOwner || !owner)
        throw std::runtime_error("Streamline presentation owner is unavailable");
    if (!dlssSupported || !reflexSupported || !pclSupported)
        throw std::runtime_error("DLSS FG requires supported DLSS-G, Reflex and PCL on the selected adapter");
    slCheck(api.load(sl::kFeatureDLSS_G, true), "Load DLSS FG at a quiescent backend boundary");
    dlssLoaded = true;
    presenterOwner = owner;
    featureFunctions();
    // The presenter enables frame production only after its real chain exists.
}
void SlRuntimeState::stopFrameCalls() {
    active.store(false);
    std::unique_lock lock(frameMutex);
    if (!callsFinished.wait_for(lock, std::chrono::milliseconds(slTimeoutMilliseconds), [this] { return frameCalls == 0; })) {
        quarantine("Streamline frame/Reflex call did not retire; DLLs and token owners remain retained");
        throw std::runtime_error("Streamline frame-call retirement timed out");
    }
}
void SlRuntimeState::deactivate(void* owner) {
    stopFrameCalls();
    std::lock_guard sdkLock(sdkMutex);
    if (presenterOwner != owner) throw std::runtime_error("Streamline presenter ownership mismatch");
    slCheck(api.load(sl::kFeatureDLSS_G, false), "Unload retired DLSS FG backend");
    api.dlssOptions = nullptr; api.dlssState = nullptr; dlssLoaded = false;
    presenterOwner = nullptr;
    std::lock_guard lock(frameMutex);
    tickets.clear();
}
void SlRuntimeState::leaveFrameCall() noexcept {
    std::lock_guard lock(frameMutex);
    --frameCalls; callsFinished.notify_all();
}
void SlRuntimeState::pruneLocked() {
    for (auto it = tickets.begin(); it != tickets.end();) {
        const auto& ticket = it->second;
        if (ticket.use_count() == 1 && ticket->presented && (ticket->markers & markerBit(SlMarker::PresentEnd)))
            it = tickets.erase(it);
        else ++it;
    }
}
bool SlRuntimeState::begin(uint64_t applicationFrameId) {
    std::shared_ptr<SlTicket> ticket;
    {
        std::lock_guard lock(frameMutex);
        if (!active || quarantined || !applicationFrameId) return false;
        if (const auto found = tickets.find(applicationFrameId); found != tickets.end()) return found->second->begun;
        pruneLocked();
        if ((haveBegun && applicationFrameId <= lastBegun) ||
            !tokenLeases[tokenAllocations % tokenLeases.size()].expired()) {
            ++rejected; report("Application frame is repeated/out of order or the SDK's six-token pool is still leased");
            return false;
        }
        const auto sdkId = static_cast<uint32_t>(applicationFrameId);
        if (haveBegun && static_cast<uint32_t>(lastBegun) == sdkId) {
            ++rejected; report("Application frame aliases an active 32-bit Streamline frame index"); return false;
        }
        for (const auto& lease : tokenLeases) {
            const auto live = lease.lock();
            if (live && live->sdkFrameId == sdkId) {
                ++rejected; report("Application frame aliases a still-leased 32-bit Streamline token"); return false;
            }
        }
        sl::FrameToken* token = nullptr;
        slCheck(api.newFrame(token, &sdkId), "Allocate the before-input Streamline frame token");
        if (!token || static_cast<uint32_t>(*token) != sdkId)
            throw std::runtime_error("Streamline returned the wrong application frame token");
        ticket = std::make_shared<SlTicket>(applicationFrameId, token);
        tickets.emplace(applicationFrameId, ticket);
        tokenLeases[tokenAllocations++ % tokenLeases.size()] = ticket;
        lastBegun = applicationFrameId; haveBegun = true; ++accepted; ++frameCalls;
    }
    FrameCall call{this};
    // No frame/SDK mutex across sleep: rendering the preceding frame must remain
    // free to submit work and Present while the simulation thread is sleeping.
    slCheck(api.sleep(*ticket->token), "Reflex before-input sleep");
    std::lock_guard lock(frameMutex);
    ticket->begun = true;
    return true;
}
bool SlRuntimeState::mark(uint64_t applicationFrameId, SlMarker markerValue) {
    const auto sdkMarker = pclMarker(markerValue);
    std::shared_ptr<SlTicket> ticket;
    const auto bit = markerBit(markerValue);
    {
        std::lock_guard lock(frameMutex);
        if (!active || quarantined) return false;
        const auto found = tickets.find(applicationFrameId);
        if (found == tickets.end() || !found->second->begun) { report("No before-input ticket for this PCL marker"); return false; }
        ticket = found->second;
        if (ticket->markers & bit) return true; // Exactly once, not duplicate SDK timestamps.
        if (ticket->pendingMarkers & bit) return false;
        if ((markerValue == SlMarker::SimulationEnd && !(ticket->markers & markerBit(SlMarker::SimulationStart))) ||
            (markerValue == SlMarker::RenderSubmitEnd && !(ticket->markers & markerBit(SlMarker::RenderSubmitStart))) ||
            (markerValue == SlMarker::PresentEnd && (!(ticket->markers & markerBit(SlMarker::PresentStart)) || !ticket->presented))) {
            report("PCL phase end preceded its matching start or actual Present"); return false;
        }
        if (static_cast<uint32_t>(*ticket->token) != ticket->sdkFrameId)
            throw std::runtime_error("Streamline frame token changed while still leased");
        ticket->pendingMarkers |= bit; ++frameCalls;
    }
    FrameCall call{this};
    const auto result = api.marker(sdkMarker, *ticket->token);
    {
        std::lock_guard lock(frameMutex);
        ticket->pendingMarkers &= ~bit;
        if (result == sl::Result::eOk) ticket->markers |= bit;
    }
    slCheck(result, "Submit matching PCL marker");
    return true;
}
std::shared_ptr<SlTicket> SlRuntimeState::claim(uint64_t applicationFrameId) {
    std::lock_guard lock(frameMutex);
    // Present is externally ordered. Earlier unclaimed simulation frames were
    // skipped by the engine; they must not hold the SDK ring forever.
    for (auto it = tickets.begin(); it != tickets.end() && it->first < applicationFrameId;) {
        if (!it->second->claimed) it = tickets.erase(it);
        else ++it;
    }
    const auto found = tickets.find(applicationFrameId);
    if (!active || quarantined || found == tickets.end() || !found->second->begun || found->second->claimed) return {};
    const auto& ticket = found->second;
    // Copy/tag submission is still in progress at claim time. End/PresentStart
    // are validated at the real Present boundary, not required prematurely.
    constexpr uint32_t required = markerBit(SlMarker::SimulationStart) | markerBit(SlMarker::SimulationEnd) |
                                  markerBit(SlMarker::RenderSubmitStart);
    if (static_cast<uint32_t>(*ticket->token) != ticket->sdkFrameId) return {};
    ticket->generationReady = (ticket->markers & required) == required;
    ticket->claimed = true;
    return ticket;
}
bool SlRuntimeState::readyForPresent(const std::shared_ptr<SlTicket>& ticket) {
    if (!ticket) return false;
    std::lock_guard lock(frameMutex);
    constexpr auto required = markerBit(SlMarker::RenderSubmitEnd) | markerBit(SlMarker::PresentStart);
    return ticket->generationReady && (ticket->markers & required) == required;
}
void SlRuntimeState::presented(const std::shared_ptr<SlTicket>& ticket) {
    if (!ticket) return;
    std::lock_guard lock(frameMutex);
    ticket->presented = true;
}
void SlRuntimeState::clearTags(const SlTicket& ticket) {
    const sl::BufferType types[] = {sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors, sl::kBufferTypeHUDLessColor,
                                    sl::kBufferTypeUIAlpha, sl::kBufferTypeUIColorAndAlpha,
                                    sl::kBufferTypeBidirectionalDistortionField, sl::kBufferTypeBackbuffer};
    std::array<sl::ResourceTag, std::size(types)> tags;
    for (size_t i = 0; i < tags.size(); ++i) tags[i] = sl::ResourceTag(nullptr, types[i], sl::ResourceLifecycle::eValidUntilPresent);
    slCheck(api.tags(*ticket.token, sl::ViewportHandle(0u), tags.data(), static_cast<uint32_t>(tags.size()), nullptr),
            "Clear frame-owned Streamline resource tags");
}
bool SlRuntimeState::setReflex(SlReflexMode mode, uint32_t frameLimitMicroseconds) {
    std::lock_guard lock(sdkMutex);
    if (!attached || quarantined || !reflexSupported) return false;
    sl::ReflexOptions options{};
    switch (mode) {
    case SlReflexMode::Off: options.mode = sl::ReflexMode::eOff; break;
    case SlReflexMode::On: options.mode = sl::ReflexMode::eLowLatency; break;
    case SlReflexMode::OnWithBoost: options.mode = sl::ReflexMode::eLowLatencyWithBoost; break;
    default: return false;
    }
    options.frameLimitUs = frameLimitMicroseconds;
    slCheck(api.reflexOptions(options), "Configure Reflex latency/limiter");
    reflexMode.store(mode);
    return true;
}
SlRuntimeStatus SlRuntimeState::status() const {
    std::lock_guard lock(frameMutex);
    SlRuntimeStatus result;
    result.initialized = initialized; result.attached = attached; result.presentationActive = active;
    result.dlssSupported = dlssSupported; result.reflexSupported = reflexSupported; result.pclSupported = pclSupported;
    result.lowLatencyAvailable = lowLatencyAvailable; result.flashIndicatorDriverControlled = flashIndicatorDriverControlled;
    result.statsWindowMessage = statsWindowMessage; result.quarantined = quarantined;
    result.acceptedFrameTickets = accepted; result.rejectedFrameTickets = rejected;
    std::lock_guard errorLock(errorMutex); result.reason = reason;
    return result;
}
void SlRuntimeState::bounded(std::function<void()> operation) {
    if (control) throw std::runtime_error("A Streamline control operation remains quarantined");
    control = std::make_unique<SlControlWork>();
    auto* work = control.get();
    try {
        work->thread = std::thread([work, operation = std::move(operation)]() mutable {
            try { operation(); work->done.set_value(); }
            catch (...) { try { work->done.set_exception(std::current_exception()); } catch (...) {} }
        });
    } catch (...) { control.reset(); throw; }
    if (work->completion.wait_for(std::chrono::milliseconds(slTimeoutMilliseconds)) != std::future_status::ready) {
        quarantine("Streamline SDK retirement timed out; the complete owner remains retained");
        throw std::runtime_error("Streamline SDK retirement timed out");
    }
    work->thread.join();
    auto finished = std::move(control);
    finished->completion.get();
}
PresentRetirement SlRuntimeState::stop() noexcept {
    if (quarantined) return PresentRetirement::Quarantined;
    try {
        stopFrameCalls();
        if (presenterOwner) throw std::runtime_error("A presentation chain still owns this Streamline runtime");
        if (initialized) {
            bounded([this] {
                std::lock_guard sdkLock(sdkMutex);
                // Release SL proxy objects while their plugin manager still
                // exists. Native device/factory references remain alive across
                // slShutdown, as required by the SDK shutdown contract.
                proxyFactory.Reset(); proxyDevice.Reset();
                slCheck(api.shutdown(), "Shutdown Streamline");
            });
            initialized = attached = dlssLoaded = false;
        } else { proxyFactory.Reset(); proxyDevice.Reset(); }
        tickets.clear();
        if (ownsProcess) { processClaimed.store(false); ownsProcess = false; }
        return PresentRetirement::Drained;
    } catch (const std::exception& exception) { quarantine(exception.what()); }
    catch (...) { quarantine("Unknown Streamline runtime retirement failure"); }
    return PresentRetirement::Quarantined;
}
} // namespace dspaa::detail

namespace dspaa {
SlRuntime::SlRuntime(const SlRuntimeCreateInfo& info) : state_(new detail::SlRuntimeState(info), [](auto* state) {
    if (state->stop() == PresentRetirement::Drained) delete state;
}) { state_->initialize(); }
SlRuntime::~SlRuntime() = default;
void SlRuntime::attach(ID3D12Device* device, IDXGIFactory* factory) { state_->attach(device, factory); }
bool SlRuntime::beginFrame(uint64_t applicationFrameId) noexcept {
    const auto state = state_;
    try { return state->begin(applicationFrameId); }
    catch (const std::exception& exception) { state->report(exception.what()); }
    catch (...) { state->report("Unknown Streamline BeginFrame failure"); }
    return false;
}
bool SlRuntime::marker(uint64_t applicationFrameId, SlMarker marker) noexcept {
    const auto state = state_;
    try { return state->mark(applicationFrameId, marker); }
    catch (const std::exception& exception) { state->report(exception.what()); }
    catch (...) { state->report("Unknown Streamline PCL marker failure"); }
    return false;
}
bool SlRuntime::setReflex(SlReflexMode mode, uint32_t frameLimitMicroseconds) noexcept {
    const auto state = state_;
    try { return state->setReflex(mode, frameLimitMicroseconds); }
    catch (const std::exception& exception) { state->report(exception.what()); }
    catch (...) { state->report("Unknown Reflex configuration failure"); }
    return false;
}
SlRuntimeStatus SlRuntime::status() const { return state_->status(); }
PresentRetirement SlRuntime::shutdown() noexcept { return state_->stop(); }
} // namespace dspaa
