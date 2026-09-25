#include "backend.h"
#include "graphics/dx11-dx12.h"
#include <array>
#include <stdexcept>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
class NativePresenter final : public PresentBackend {
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        FrameLease frame;
        uint64_t completion = 0;
    };
    struct State {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGISwapChain4> chain;
        ComPtr<ID3D12Fence> fence;
        std::array<Slot, 3> slots;
        uint64_t next = 0;
        unsigned current = 0;
        bool faulted = false;
        HANDLE event = nullptr;
        ~State() { if (event) CloseHandle(event); }
        void wait(uint64_t value) {
            if (!value) return;
            graphicsCheck(device->GetDeviceRemovedReason(), "Native presenter device");
            auto completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX) throw std::runtime_error("Native presenter device removed");
            if (completed < value) {
                graphicsCheck(fence->SetEventOnCompletion(value, event), "Native presenter retirement event");
                if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("Native presenter retirement timeout");
                completed = fence->GetCompletedValue();
                if (completed == UINT64_MAX || completed < value)
                    throw std::runtime_error("Native presenter retirement was not proven");
            }
        }
        void drain() {
            const auto value = ++next;
            graphicsCheck(queue->Signal(fence.Get(), value), "Native presenter drain signal");
            wait(value);
            for (auto& slot : slots) slot.frame.reset();
        }
    };
    std::unique_ptr<State> s_ = std::make_unique<State>();
    bool quarantined_ = false;
  public:
    explicit NativePresenter(const PresenterCreateInfo& info) {
        if (!info.factory || !info.device || !info.queue || !IsWindow(info.window) ||
            info.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("Invalid native presentation owner");
        ComPtr<ID3D12Device> owner;
        graphicsCheck(info.queue->GetDevice(IID_PPV_ARGS(&owner)), "Native queue device");
        if (owner.Get() != info.device) throw std::invalid_argument("Native queue belongs to another device");
        auto& s = *s_;
        s.device = info.device;
        s.queue = info.queue;
        graphicsCheck(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)), "Native presenter fence");
        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s.event) graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Native presenter event");
        for (auto& slot : s.slots) {
            graphicsCheck(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)), "Native presenter allocator");
            graphicsCheck(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(&slot.list)), "Native presenter command list");
            graphicsCheck(slot.list->Close(), "Initial native presenter command list");
        }
        // No GPU work is submitted in construction. A failed construction releases
        // an unpresented chain; SDK backends separately report ambiguous ownership.
        ComPtr<IDXGISwapChain1> chain;
        graphicsCheck(info.factory->CreateSwapChainForHwnd(s.queue.Get(), info.window, &info.description,
            info.fullscreen ? &*info.fullscreen : nullptr, info.restrictOutput, &chain), "Native D3D12 swap chain");
        graphicsCheck(chain.As(&s.chain), "Native swap-chain interface");
    }
    ~NativePresenter() override { (void)stop(); }
    IDXGISwapChain4* queries() const noexcept override { return s_ ? s_->chain.Get() : nullptr; }
    HRESULT present(FrameLease frame, const PresentArguments& args, bool) noexcept override {
        if (!s_ || s_->faulted) return DXGI_ERROR_DEVICE_REMOVED;
        auto& s = *s_;
        if (args.flags & DXGI_PRESENT_TEST)
            return args.parameters ? s.chain->Present1(args.syncInterval, args.flags, args.parameters) : s.chain->Present(args.syncInterval, args.flags);
        try {
            if (!frame || !frame->images || !frame->images->finalColor.resource ||
                (frame->readyValue && !frame->readyFence)) return E_INVALIDARG;
            ComPtr<ID3D12Device> owner;
            graphicsCheck(frame->images->finalColor.resource->GetDevice(IID_PPV_ARGS(&owner)), "Native input device");
            if (owner.Get() != s.device.Get()) return E_INVALIDARG;
            auto& slot = s.slots[s.current];
            s.wait(slot.completion);
            slot.frame.reset();
            if (frame->readyFence && frame->readyValue)
                graphicsCheck(s.queue->Wait(frame->readyFence.Get(), frame->readyValue), "Native input lease readiness");
            ComPtr<ID3D12Resource> back;
            graphicsCheck(s.chain->GetBuffer(s.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)), "Native current back buffer");
            const auto sourceDesc = frame->images->finalColor.resource->GetDesc(), targetDesc = back->GetDesc();
            if (sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height ||
                sourceDesc.Format != targetDesc.Format || sourceDesc.SampleDesc.Count != 1) return E_INVALIDARG;
            graphicsCheck(slot.allocator->Reset(), "Reset native allocator");
            graphicsCheck(slot.list->Reset(slot.allocator.Get(), nullptr), "Reset native list");
            const auto& image = frame->images->finalColor;
            if (image.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
                Dx11Dx12::transition(slot.list.Get(), image.resource.Get(), image.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Dx11Dx12::transition(slot.list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            slot.list->CopyResource(back.Get(), image.resource.Get());
            Dx11Dx12::transition(slot.list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            if (image.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
                Dx11Dx12::transition(slot.list.Get(), image.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, image.state);
            graphicsCheck(slot.list->Close(), "Close native present copy");
            slot.frame = std::move(frame);
            ID3D12CommandList* lists[] = {slot.list.Get()};
            s.queue->ExecuteCommandLists(1, lists);
            slot.completion = ++s.next;
            graphicsCheck(s.queue->Signal(s.fence.Get(), slot.completion), "Native input consumption signal");
            if (args.boundary && args.boundary->before) args.boundary->before(args.boundary->context, slot.frame->applicationFrameId);
            const auto result = args.parameters ? s.chain->Present1(args.syncInterval, args.flags, args.parameters) : s.chain->Present(args.syncInterval, args.flags);
            if (args.boundary && args.boundary->after) args.boundary->after(args.boundary->context, slot.frame->applicationFrameId);
            s.current = (s.current + 1) % static_cast<unsigned>(s.slots.size());
            return result;
        } catch (...) { s.faulted = true; return DXGI_ERROR_DEVICE_REMOVED; }
    }
    HRESULT resize(const DXGI_SWAP_CHAIN_DESC1& desc, uint64_t) noexcept override {
        if (!s_ || s_->faulted) return DXGI_ERROR_DEVICE_REMOVED;
        try { s_->drain(); return s_->chain->ResizeBuffers(desc.BufferCount, desc.Width, desc.Height, desc.Format, desc.Flags); }
        catch (...) { s_->faulted = true; return DXGI_ERROR_DEVICE_REMOVED; }
    }
    PresentRetirement stop() noexcept override {
        if (quarantined_) return PresentRetirement::Quarantined;
        if (!s_) return PresentRetirement::Drained;
        try { s_->drain(); s_.reset(); return PresentRetirement::Drained; }
        catch (...) { quarantined_ = true; (void)s_.release(); return PresentRetirement::Quarantined; }
    }
    HRESULT setFullscreen(BOOL value, IDXGIOutput* output) noexcept override {
        if (!s_) return DXGI_ERROR_DEVICE_REMOVED;
        try { s_->drain(); return s_->chain->SetFullscreenState(value, output); } catch (...) { s_->faulted = true; return DXGI_ERROR_DEVICE_REMOVED; }
    }
    HRESULT resizeTarget(const DXGI_MODE_DESC* value) noexcept override {
        if (!s_) return DXGI_ERROR_DEVICE_REMOVED;
        try { s_->drain(); return s_->chain->ResizeTarget(value); } catch (...) { s_->faulted = true; return DXGI_ERROR_DEVICE_REMOVED; }
    }
    HRESULT setColorSpace(DXGI_COLOR_SPACE_TYPE v) noexcept override { return s_ ? s_->chain->SetColorSpace1(v) : DXGI_ERROR_DEVICE_REMOVED; }
    HRESULT setHdrMetadata(DXGI_HDR_METADATA_TYPE t, UINT n, void* p) noexcept override { return s_ ? s_->chain->SetHDRMetaData(t,n,p) : DXGI_ERROR_DEVICE_REMOVED; }
    HRESULT setMaximumFrameLatency(UINT v) noexcept override { return s_ ? s_->chain->SetMaximumFrameLatency(v) : DXGI_ERROR_DEVICE_REMOVED; }
    HRESULT setSourceSize(UINT w, UINT h) noexcept override { return s_ ? s_->chain->SetSourceSize(w,h) : DXGI_ERROR_DEVICE_REMOVED; }
    HRESULT setRotation(DXGI_MODE_ROTATION v) noexcept override { return s_ ? s_->chain->SetRotation(v) : DXGI_ERROR_DEVICE_REMOVED; }
    HRESULT setMatrixTransform(const DXGI_MATRIX_3X2_F* v) noexcept override { return s_ ? s_->chain->SetMatrixTransform(v) : DXGI_ERROR_DEVICE_REMOVED; }
    HRESULT setBackgroundColor(const DXGI_RGBA* v) noexcept override { return s_ ? s_->chain->SetBackgroundColor(v) : DXGI_ERROR_DEVICE_REMOVED; }
};
} // namespace
std::unique_ptr<PresentBackend> createNativePresenter(const PresenterCreateInfo& info) {
    return std::make_unique<NativePresenter>(info);
}
} // namespace dspaa
