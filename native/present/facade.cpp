#include "facade.h"
#include "backend.h"
#include "fsr/presenter.h"
#include "graphics/dx11-dx12.h"
#include "latency.h"
#include "sl/presenter.h"
#include "transport.h"
#include "world-color.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const noexcept {
        return std::memcmp(&a, &b, sizeof(a)) < 0;
    }
};
struct PrivateData {
    std::vector<unsigned char> bytes;
    ComPtr<IUnknown> object;
};
// This adapter exposes Unity's D3D11 logical buffer zero, not the rotating physical
// D3D12 buffer index. Cached ID3D11Resource/RTV identities survive every Present.
// Other logical indices are deliberately rejected rather than exposing an unused
// image or leaking a D3D12 resource into an engine expecting D3D11.
class Facade final : public IDXGISwapChain4 {
    struct State {
        ComPtr<IDXGIFactory2> factory;
        ComPtr<ID3D11Device> device;
        std::shared_ptr<Dx11Dx12> bridge;
        std::unique_ptr<PresentBackend> backend;
        PresentationLatency latency;
        DXGI_SWAP_CHAIN_DESC1 description{};
        SharedTexture shadow;
        std::unique_ptr<PresentationTransport> transport;
        std::shared_ptr<PresentationChannel> channel;
        std::shared_ptr<WorldColor> world;
        std::filesystem::path runtimeDirectory;
        ComPtr<IDXGIOutput> restrictOutput;
        uint32_t activeBackend = 0;
        uint64_t appliedRevision = 0;
        bool resumeWhenEnvelope = false;
        std::string selectionFailure;
        std::optional<UINT> maximumLatency;
        std::optional<std::pair<UINT, UINT>> sourceSize;
        std::optional<DXGI_MODE_ROTATION> rotation;
        std::optional<DXGI_MATRIX_3X2_F> matrix;
        std::optional<DXGI_RGBA> background;
        DXGI_HDR_METADATA_TYPE hdrType = DXGI_HDR_METADATA_TYPE_NONE;
        std::vector<unsigned char> hdrMetadata;
        HWND window = nullptr;
        uint64_t generation = 1, presents = 0;
        DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        PresentationLog log = nullptr;
        bool faulted = false;
        std::map<GUID, PrivateData, GuidLess> privateData;
        bool waitable() const {
            return (description.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
        }
        bool bindLatency() {
            return !waitable() || latency.bind(backend->queries()->GetFrameLatencyWaitableObject());
        }
        PresentRetirement stop() noexcept {
            if (channel)
                channel->detach();
            // An event may already own WorldColor. Close its producer under the
            // graphics lock before proving the last GPU work drained.
            if (world)
                world->releaseSurfaceReference();
            // A relay that still owns the old waitable cannot outlive destruction
            // of the corresponding real chain. Keep the entire State if uncertain.
            if (!latency.pause())
                return PresentRetirement::Quarantined;
            if (backend && backend->stop() != PresentRetirement::Drained)
                return PresentRetirement::Quarantined;
            try {
                if (bridge)
                    bridge->drain();
            } catch (...) {
                return PresentRetirement::Quarantined;
            }
            if (world)
                world->surface(nullptr, 0);
            return PresentRetirement::Drained;
        }
    };
    std::atomic<ULONG> references_{1};
    mutable std::recursive_mutex mutex_;
    std::unique_ptr<State> s_ = std::make_unique<State>();
    template <class Operation> HRESULT query(Operation operation) noexcept {
        std::lock_guard guard(mutex_);
        if (!s_ || s_->faulted || !s_->backend || !s_->backend->queries())
            return DXGI_ERROR_DEVICE_REMOVED;
        return operation(s_->backend->queries());
    }
    template <class Operation> HRESULT control(Operation operation) noexcept {
        std::lock_guard guard(mutex_);
        if (!s_ || s_->faulted || !s_->backend)
            return DXGI_ERROR_DEVICE_REMOVED;
        return operation(*s_->backend);
    }
    static void dimensions(HWND window, DXGI_SWAP_CHAIN_DESC1& desc) {
        if (!desc.Width || !desc.Height) {
            RECT client{};
            if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
                throw std::invalid_argument("Cannot resolve presentation client dimensions");
            if (!desc.Width)
                desc.Width = static_cast<UINT>(client.right);
            if (!desc.Height)
                desc.Height = static_cast<UINT>(client.bottom);
        }
    }
    void restoreControls() {
        auto& s = *s_;
        graphicsCheck(s.backend->setColorSpace(s.colorSpace), "Restore presentation color space");
        if (s.maximumLatency)
            graphicsCheck(s.backend->setMaximumFrameLatency(*s.maximumLatency),
                          "Restore presentation latency");
        if (s.sourceSize)
            graphicsCheck(s.backend->setSourceSize(s.sourceSize->first, s.sourceSize->second),
                          "Restore presentation source size");
        if (s.rotation)
            graphicsCheck(s.backend->setRotation(*s.rotation), "Restore presentation rotation");
        if (s.matrix)
            graphicsCheck(s.backend->setMatrixTransform(&*s.matrix), "Restore presentation matrix");
        if (s.background)
            graphicsCheck(s.backend->setBackgroundColor(&*s.background), "Restore presentation background");
        if (s.hdrType != DXGI_HDR_METADATA_TYPE_NONE)
            graphicsCheck(s.backend->setHdrMetadata(s.hdrType, static_cast<UINT>(s.hdrMetadata.size()),
                                                    s.hdrMetadata.data()),
                          "Restore HDR metadata");
    }
    void changeBackend(uint32_t requested, const PresentationConfiguration& configuration) {
        auto& s = *s_;
        if (requested == s.activeBackend) {
            s.backend->configure(configuration);
            return;
        }
        // Serialize world-copy callbacks across the old owner's final drain and
        // surface generation change, not merely while updating its source pointer.
        auto graphics = s.bridge->lock();
        BOOL fullscreen = FALSE;
        ComPtr<IDXGIOutput> fullscreenOutput;
        graphicsCheck(s.backend->queries()->GetFullscreenState(&fullscreen, &fullscreenOutput),
                      "Read previous fullscreen owner");
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDescription{};
        graphicsCheck(s.backend->queries()->GetFullscreenDesc(&fullscreenDescription),
                      "Read previous fullscreen description");
        if (fullscreen)
            graphicsCheck(s.backend->setFullscreen(FALSE, nullptr), "Release previous fullscreen ownership");
        if (!s.latency.pause())
            throw std::runtime_error("Old presentation latency relay did not retire");
        if (s.backend->stop() != PresentRetirement::Drained)
            throw std::runtime_error(
                "Old presentation backend is quarantined; refusing a second chain on its HWND");
        s.backend.reset();
        s.bridge->drain();
        s.transport->reset();
        ++s.generation;
        s.channel->surface(s.generation, s.description.Width, s.description.Height);
        s.world->surface(s.shadow.dx11.Get(), s.generation);
        PresenterCreateInfo info;
        info.window = s.window;
        info.factory = s.factory.Get();
        info.device = s.bridge->device12();
        info.queue = s.bridge->queue12();
        info.description = s.description;
        info.fullscreen = fullscreenDescription;
        info.fullscreen->Windowed = TRUE;
        info.restrictOutput = s.restrictOutput.Get();
        info.runtimeDirectory = s.runtimeDirectory;
        info.generation = s.generation;
        s.selectionFailure.clear();
        bool mayRecreate = true;
        try {
            if (requested && s.restrictOutput)
                throw std::runtime_error("The SDK backend cannot preserve this restricted-output chain");
            if (requested == 1)
                s.backend = createFsrBackend(info);
            else if (requested == 2) {
                auto runtime = s.channel->runtime();
                if (!runtime)
                    throw std::runtime_error("Early Streamline initialization unavailable: " +
                                             presentationRuntimeFailure());
                runtime->attach(s.bridge->device12(), s.factory.Get());
                s.backend = createSlBackend(info, std::move(runtime));
            } else
                s.backend = createNativePresenter(info);
            s.backend->configure(configuration);
        } catch (const FsrPresenterCreationError& e) {
            mayRecreate = e.retirement == PresentRetirement::Drained;
            s.selectionFailure = e.what();
        } catch (const SlPresenterCreationError& e) {
            mayRecreate = e.retirement == PresentRetirement::Drained;
            s.selectionFailure = e.what();
        } catch (const std::exception& e) {
            s.selectionFailure = e.what();
        }
        if (!s.selectionFailure.empty()) {
            if (s.backend && s.backend->stop() != PresentRetirement::Drained)
                mayRecreate = false;
            if (!mayRecreate)
                throw std::runtime_error("New SDK owner is quarantined: " + s.selectionFailure);
            s.backend.reset();
            s.backend = createNativePresenter(info);
            requested = 0;
            if (s.log)
                s.log(s.selectionFailure.c_str());
        }
        s.activeBackend = requested;
        restoreControls();
        if (fullscreen)
            graphicsCheck(s.backend->setFullscreen(TRUE, fullscreenOutput.Get()),
                          "Restore fullscreen ownership");
        if (!s.bindLatency())
            throw std::runtime_error("Could not rebind stable host latency after backend replacement");
    }
    static bool beforePresent(void* opaque, uint64_t id) noexcept {
        auto& s = *static_cast<State*>(opaque);
        if (s.activeBackend != 2)
            return true;
        const bool end = s.channel->marker(id, SlMarker::RenderSubmitEnd);
        const bool start = s.channel->marker(id, SlMarker::PresentStart);
        return end && start;
    }
    static void afterPresent(void* opaque, uint64_t id) noexcept {
        auto& s = *static_cast<State*>(opaque);
        if (s.activeBackend == 2)
            s.channel->marker(id, SlMarker::PresentEnd);
    }
    void unbindBackbuffer() {
        auto& s = *s_;
        auto access = s.bridge->lock();
        std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views;
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw{};
        ComPtr<ID3D11DepthStencilView> depth;
        s.bridge->context11()->OMGetRenderTargets(static_cast<UINT>(raw.size()), raw.data(), &depth);
        bool changed = false;
        for (size_t i = 0; i < raw.size(); ++i) {
            views[i].Attach(raw[i]);
            ComPtr<ID3D11Resource> resource;
            if (raw[i])
                raw[i]->GetResource(&resource);
            if (resource.Get() == s.shadow.dx11.Get()) {
                raw[i] = nullptr;
                changed = true;
            }
        }
        if (changed)
            s.bridge->context11()->OMSetRenderTargets(static_cast<UINT>(raw.size()), raw.data(), depth.Get());
    }
    HRESULT present(const PresentArguments& args) noexcept {
        std::lock_guard guard(mutex_);
        if (!s_ || s_->faulted || (s_->waitable() && !s_->latency.active()))
            return DXGI_ERROR_DEVICE_REMOVED;
        try {
            if (args.flags & DXGI_PRESENT_TEST)
                return s_->backend->present({}, args, false);
            auto submission = s_->channel->consume();
            const auto configuration = s_->channel->configuration();
            if (!submission && s_->activeBackend) {
                changeBackend(0, configuration);
                s_->resumeWhenEnvelope = true;
            } else if ((configuration.revision != s_->appliedRevision ||
                        (s_->resumeWhenEnvelope && submission)) &&
                       (!configuration.backend || submission)) {
                changeBackend(configuration.backend, configuration);
                s_->appliedRevision = configuration.revision;
                s_->resumeWhenEnvelope = false;
            }
            std::string inputReason;
            auto frame = s_->transport->capture(s_->shadow.dx11.Get(), s_->generation, s_->colorSpace,
                                                std::move(submission), inputReason);
            const auto id = frame->applicationFrameId;
            PresentBoundary boundary{s_.get(), beforePresent, afterPresent};
            auto arguments = args;
            arguments.boundary = &boundary;
            const auto result =
                s_->backend->present(frame, arguments, s_->activeBackend != 0 && frame->inputsComplete);
            auto observed = s_->backend->observation();
            if (!s_->selectionFailure.empty())
                observed.reason = s_->selectionFailure;
            else if (s_->activeBackend && !inputReason.empty())
                observed.reason = inputReason;
            s_->channel->observe(s_->activeBackend, id, observed);
            if (SUCCEEDED(result)) {
                if (!(args.flags & DXGI_PRESENT_DO_NOT_SEQUENCE))
                    unbindBackbuffer();
                s_->latency.presented();
                ++s_->presents;
            } else if (result != DXGI_ERROR_WAS_STILL_DRAWING)
                s_->faulted = true;
            if (s_->log && (s_->presents <= 6 || s_->presents % 300 == 0 || FAILED(result))) {
                char message[224]{};
                std::snprintf(message, sizeof(message),
                              "native.present count=%llu hr=%08X width=%u height=%u generation=%llu "
                              "application=%llu backend=%u inputs=%u",
                              s_->presents, static_cast<unsigned>(result), s_->description.Width,
                              s_->description.Height, s_->generation, id, s_->activeBackend,
                              frame->inputsComplete ? 1u : 0u);
                s_->log(message);
            }
            return result;
        } catch (const std::exception& e) {
            if (s_->log)
                s_->log(e.what());
        } catch (...) {
            if (s_->log)
                s_->log("Unknown presentation bridge failure");
        }
        s_->faulted = true;
        if (s_->channel)
            s_->channel->reason("Presentation owner faulted; no unsafe HWND re-creation will be attempted",
                                true);
        return DXGI_ERROR_DEVICE_REMOVED;
    }

  public:
    Facade() = default;
    ~Facade() {
        if (s_ && s_->stop() != PresentRetirement::Drained) {
            if (s_->log)
                s_->log("Presentation owner quarantined; HWND must not be reused");
            (void)s_.release();
        }
    }
    void initialize(IDXGIFactory2* factory, ID3D11Device* device, HWND window,
                    const DXGI_SWAP_CHAIN_DESC1& description, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
                    IDXGIOutput* output, const std::filesystem::path& runtime, PresentationLog log) {
        if (!factory || !device || !IsWindow(window) || description.Stereo ||
            description.SampleDesc.Count != 1 || description.SampleDesc.Quality ||
            description.BufferCount < 2 ||
            (description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL &&
             description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD))
            throw std::invalid_argument(
                "Presentation bridge requires a non-stereo, single-sample D3D11 flip chain");
        auto& s = *s_;
        s.factory = factory;
        s.device = device;
        s.window = window;
        s.description = description;
        s.log = log;
        dimensions(window, s.description);
        s.bridge = acquireDx11Dx12(device);
        s.runtimeDirectory = runtime;
        s.restrictOutput = output;
        s.channel = registerPresentationChannel(runtime, log);
        s.channel->surface(s.generation, s.description.Width, s.description.Height);
        if (auto runtimeService = s.channel->runtime()) {
            try {
                runtimeService->attach(s.bridge->device12(), s.factory.Get());
            } catch (const std::exception& error) {
                if (s.log)
                    s.log(error.what());
            }
        }
        s.transport = std::make_unique<PresentationTransport>(s.bridge);
        s.shadow = s.bridge->texture(s.description.Width, s.description.Height, s.description.Format, false);
        s.world = std::make_shared<WorldColor>(s.bridge);
        s.world->surface(s.shadow.dx11.Get(), s.generation);
        s.channel->bindWorldColor(s.world);
        PresenterCreateInfo info;
        info.window = window;
        info.factory = factory;
        info.device = s.bridge->device12();
        info.queue = s.bridge->queue12();
        info.description = s.description;
        info.restrictOutput = output;
        info.runtimeDirectory = runtime;
        info.generation = s.generation;
        if (full)
            info.fullscreen = *full;
        s.backend = createNativePresenter(info);
        if (!s.bindLatency())
            throw std::runtime_error("Cannot bind stable presentation latency handle");
        if (s.log)
            s.log("Native D3D11-facing/D3D12 presentation bridge created");
    }
    PresentRetirement stop() noexcept {
        return s_ ? s_->stop() : PresentRetirement::Drained;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDeviceSubObject) ||
            iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IDXGISwapChain1) ||
            iid == __uuidof(IDXGISwapChain2) || iid == __uuidof(IDXGISwapChain3) ||
            iid == __uuidof(IDXGISwapChain4) || iid == presentationFacadeId) {
            *output = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --references_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key, UINT size, const void* data) override {
        if (size && !data)
            return E_INVALIDARG;
        try {
            std::lock_guard guard(mutex_);
            if (!size) {
                s_->privateData.erase(key);
                return S_OK;
            }
            PrivateData value;
            const auto* first = static_cast<const unsigned char*>(data);
            value.bytes.assign(first, first + size);
            s_->privateData[key] = std::move(value);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID key, const IUnknown* data) override {
        try {
            std::lock_guard guard(mutex_);
            if (!data) {
                s_->privateData.erase(key);
                return S_OK;
            }
            PrivateData value;
            value.object = const_cast<IUnknown*>(data);
            s_->privateData[key] = std::move(value);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key, UINT* size, void* data) override {
        if (!size)
            return E_INVALIDARG;
        std::lock_guard guard(mutex_);
        const auto it = s_->privateData.find(key);
        if (it == s_->privateData.end())
            return DXGI_ERROR_NOT_FOUND;
        const auto& value = it->second;
        const auto bytes = value.object ? sizeof(IUnknown*) : value.bytes.size();
        const auto capacity = *size;
        *size = static_cast<UINT>(bytes);
        if (!data)
            return S_OK;
        if (capacity < bytes)
            return DXGI_ERROR_MORE_DATA;
        if (value.object) {
            auto* object = value.object.Get();
            object->AddRef();
            std::memcpy(data, &object, bytes);
        } else
            std::memcpy(data, value.bytes.data(), bytes);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** output) override {
        return s_->factory->QueryInterface(iid, output);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override {
        return s_->device->QueryInterface(iid, output);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID iid, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        std::lock_guard guard(mutex_);
        if (index)
            return DXGI_ERROR_INVALID_CALL;
        if (s_->faulted)
            return DXGI_ERROR_DEVICE_REMOVED;
        return s_->shadow.dx11->QueryInterface(iid, output);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override {
        return present({interval, flags, nullptr});
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT interval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) override {
        if (!params)
            return E_INVALIDARG;
        return present({interval, flags, params});
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
        std::lock_guard guard(mutex_);
        if (s_->faulted)
            return DXGI_ERROR_DEVICE_REMOVED;
        // Keep world-copy callbacks out of the entire drain/surface transition.
        auto graphics = s_->bridge->lock();
        // D3D11 engines must release their cached views before resize. The COM
        // resource reference count includes such views as well as GetBuffer refs.
        s_->world->releaseSurfaceReference();
        const auto references = s_->shadow.dx11->AddRef();
        s_->shadow.dx11->Release();
        s_->world->surface(s_->shadow.dx11.Get(), s_->generation);
        if (references != 2)
            return DXGI_ERROR_INVALID_CALL;
        try {
            auto next = s_->description;
            if (count)
                next.BufferCount = count;
            next.Width = width;
            next.Height = height;
            if (format != DXGI_FORMAT_UNKNOWN)
                next.Format = format;
            next.Flags = flags;
            if (next.BufferCount < 2 ||
                ((flags ^ s_->description.Flags) & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
                return DXGI_ERROR_INVALID_CALL;
            dimensions(s_->window, next);
            auto replacement = s_->bridge->texture(next.Width, next.Height, next.Format, false);
            s_->bridge->drain();
            if (!s_->latency.pause()) {
                s_->faulted = true;
                return DXGI_ERROR_DEVICE_REMOVED;
            }
            const auto result = s_->backend->resize(next, s_->generation + 1);
            if (SUCCEEDED(result)) {
                s_->shadow = std::move(replacement);
                s_->description = next;
                ++s_->generation;
                s_->transport->reset();
                s_->sourceSize.reset(); // DXGI resets the source region to the new buffers.
                s_->channel->surface(s_->generation, next.Width, next.Height);
                s_->world->surface(s_->shadow.dx11.Get(), s_->generation);
            }
            if (!s_->bindLatency()) {
                s_->faulted = true;
                return DXGI_ERROR_DEVICE_REMOVED;
            }
            return result;
        } catch (const std::invalid_argument&) {
            return DXGI_ERROR_INVALID_CALL;
        } catch (const std::exception& e) {
            if (s_->log)
                s_->log(e.what());
        } catch (...) {
        }
        s_->faulted = true;
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                             const UINT* nodeMask, IUnknown* const* queues) override {
        if (nodeMask || queues)
            return DXGI_ERROR_INVALID_CALL; // D3D12 node/queue overrides are not D3D11 identities.
        return ResizeBuffers(count, w, h, f, flags);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
        return 0;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        if (!desc)
            return E_INVALIDARG;
        const auto result = query([&](auto* chain) { return chain->GetDesc(desc); });
        if (SUCCEEDED(result)) {
            desc->OutputWindow = s_->window;
            desc->BufferCount = s_->description.BufferCount;
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        if (!desc)
            return E_INVALIDARG;
        std::lock_guard guard(mutex_);
        *desc = s_->description;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* window) override {
        if (!window)
            return E_INVALIDARG;
        *window = s_->window;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        return E_NOINTERFACE;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {
        return FALSE;
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        std::lock_guard guard(mutex_);
        return s_->waitable() && !s_->faulted ? s_->latency.duplicateHostHandle() : nullptr;
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL v, IDXGIOutput* o) override {
        return control([&](auto& b) { return b.setFullscreen(v, o); });
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* v) override {
        if (!v)
            return E_INVALIDARG;
        return control([&](auto& b) { return b.resizeTarget(v); });
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE v) override {
        std::lock_guard guard(mutex_);
        const auto result = control([&](auto& b) { return b.setColorSpace(v); });
        if (SUCCEEDED(result))
            s_->colorSpace = v;
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT n, void* p) override {
        if (n && !p)
            return E_INVALIDARG;
        try {
            std::vector<unsigned char> bytes;
            if (n)
                bytes.assign(static_cast<unsigned char*>(p), static_cast<unsigned char*>(p) + n);
            std::lock_guard guard(mutex_);
            const auto hr = control([&](auto& b) { return b.setHdrMetadata(t, n, p); });
            if (SUCCEEDED(hr)) {
                s_->hdrType = t;
                s_->hdrMetadata = std::move(bytes);
            }
            return hr;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT v) override {
        std::lock_guard guard(mutex_);
        const auto hr = control([&](auto& b) { return b.setMaximumFrameLatency(v); });
        if (SUCCEEDED(hr))
            s_->maximumLatency = v;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override {
        std::lock_guard guard(mutex_);
        const auto hr = control([&](auto& b) { return b.setSourceSize(w, h); });
        if (SUCCEEDED(hr))
            s_->sourceSize = {w, h};
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION v) override {
        std::lock_guard guard(mutex_);
        const auto hr = control([&](auto& b) { return b.setRotation(v); });
        if (SUCCEEDED(hr))
            s_->rotation = v;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* v) override {
        if (!v)
            return E_INVALIDARG;
        std::lock_guard guard(mutex_);
        const auto hr = control([&](auto& b) { return b.setMatrixTransform(v); });
        if (SUCCEEDED(hr))
            s_->matrix = *v;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* v) override {
        if (!v)
            return E_INVALIDARG;
        std::lock_guard guard(mutex_);
        const auto hr = control([&](auto& b) { return b.setBackgroundColor(v); });
        if (SUCCEEDED(hr))
            s_->background = *v;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override {
        return query([&](auto* b) { return b->GetFullscreenState(f, o); });
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* v) override {
        return query([&](auto* b) { return b->GetFullscreenDesc(v); });
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** v) override {
        return query([&](auto* b) { return b->GetContainingOutput(v); });
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** v) override {
        return query([&](auto* b) { return b->GetRestrictToOutput(v); });
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* v) override {
        return query([&](auto* b) { return b->GetFrameStatistics(v); });
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* v) override {
        return query([&](auto* b) { return b->GetLastPresentCount(v); });
    }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* v) override {
        return query([&](auto* b) { return b->GetBackgroundColor(v); });
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* v) override {
        return query([&](auto* b) { return b->GetRotation(v); });
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override {
        return query([&](auto* b) { return b->GetSourceSize(w, h); });
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* v) override {
        return query([&](auto* b) { return b->GetMaximumFrameLatency(v); });
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* v) override {
        return query([&](auto* b) { return b->GetMatrixTransform(v); });
    }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE v, UINT* support) override {
        return query([&](auto* b) { return b->CheckColorSpaceSupport(v, support); });
    }
};
} // namespace
HRESULT createPresentationFacade(IDXGIFactory2* factory, ID3D11Device* device, HWND window,
                                 const DXGI_SWAP_CHAIN_DESC1& description,
                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                 IDXGIOutput* restrictOutput, const std::filesystem::path& runtime,
                                 PresentationLog log, IDXGISwapChain1** output,
                                 PresentRetirement* retirement) noexcept {
    if (retirement)
        *retirement = PresentRetirement::Drained;
    if (!output)
        return E_POINTER;
    *output = nullptr;
    std::unique_ptr<Facade> facade;
    try {
        facade = std::make_unique<Facade>();
        facade->initialize(factory, device, window, description, fullscreen, restrictOutput, runtime, log);
        *output = facade.release();
        return S_OK;
    } catch (const std::exception& e) {
        if (log)
            log(e.what());
    } catch (...) {
        if (log)
            log("Unknown presentation facade creation failure");
    }
    if (facade && facade->stop() != PresentRetirement::Drained) {
        if (retirement)
            *retirement = PresentRetirement::Quarantined;
        (void)facade.release();
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    return E_FAIL;
}
} // namespace dspaa
