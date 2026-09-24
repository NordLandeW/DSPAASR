#include "dx11-dx12.h"
#include <dxgi1_4.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace dspaa {
void graphicsCheck(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (0x" << std::hex << static_cast<uint32_t>(result) << ")";
        throw std::runtime_error(message.str());
    }
}
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
} // namespace
Dx11Dx12::Dx11Dx12(ID3D11Device* device) {
    if (!device)
        throw std::invalid_argument("Missing D3D11 device");
    graphicsCheck(device->QueryInterface(IID_PPV_ARGS(&device11_)), "D3D11 shared-fence support");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    graphicsCheck(context.As(&context11_), "D3D11 immediate-context fence support");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    graphicsCheck(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "D3D11 DXGI device");
    graphicsCheck(dxgi->GetAdapter(&adapter), "Actual rendering adapter");
    graphicsCheck(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_)),
                  "D3D12 device on the rendering adapter");
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    graphicsCheck(device12_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue12_)), "D3D12 bridge queue");
    graphicsCheck(device11_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&produced11_)),
                  "D3D11 producer fence");
    Handle producer;
    graphicsCheck(produced11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &producer.value),
                  "Share D3D11 producer fence");
    graphicsCheck(device12_->OpenSharedHandle(producer.value, IID_PPV_ARGS(&produced12_)),
                  "Open D3D11 producer fence in D3D12");
    graphicsCheck(device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&completed12_)),
                  "D3D12 completion fence");
    Handle completion;
    graphicsCheck(
        device12_->CreateSharedHandle(completed12_.Get(), nullptr, GENERIC_ALL, nullptr, &completion.value),
        "Share D3D12 completion fence");
    graphicsCheck(device11_->OpenSharedFence(completion.value, IID_PPV_ARGS(&completed11_)),
                  "Open D3D12 completion fence in D3D11");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_)
        graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Create GPU completion event");
}
Dx11Dx12::~Dx11Dx12() {
    // Owners drain before freeing any resources. No work is launched by destruction.
    if (event_)
        CloseHandle(event_);
}
SharedTexture Dx11Dx12::texture(unsigned width, unsigned height, DXGI_FORMAT format, bool unorderedAccess) {
    SharedTexture result;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // D3D11 opening a D3D12-created texture additionally requires RT capability
    // (the Microsoft VideoTexture interop contract), even for copy-only inputs.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (unorderedAccess)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    graphicsCheck(device12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&result.dx12)),
                  "Create shared bridge texture");
    Handle shared;
    graphicsCheck(
        device12_->CreateSharedHandle(result.dx12.Get(), nullptr, GENERIC_ALL, nullptr, &shared.value),
        "Export shared bridge texture");
    graphicsCheck(device11_->OpenSharedResource1(shared.value, IID_PPV_ARGS(&result.dx11)),
                  "Open bridge texture in D3D11");
    return result;
}
void Dx11Dx12::handoffTo12() {
    const auto value = ++next11_;
    graphicsCheck(context11_->Signal(produced11_.Get(), value), "Signal D3D11 producer");
    context11_->Flush(); // Submit the signal before the other API waits for it.
    graphicsCheck(queue12_->Wait(produced12_.Get(), value), "D3D12 waits for D3D11 producer");
}
uint64_t Dx11Dx12::handoffTo11() {
    const auto value = ++next12_;
    graphicsCheck(queue12_->Signal(completed12_.Get(), value), "Signal D3D12 completion");
    graphicsCheck(context11_->Wait(completed11_.Get(), value), "D3D11 waits for D3D12 completion");
    return value;
}
void Dx11Dx12::wait12(uint64_t completion) {
    if (!completion)
        return;
    graphicsCheck(device12_->GetDeviceRemovedReason(), "D3D12 device health");
    const auto completed = completed12_->GetCompletedValue();
    if (completed == UINT64_MAX)
        throw std::runtime_error("D3D12 completion fence reports device removal");
    if (completed >= completion)
        return;
    graphicsCheck(completed12_->SetEventOnCompletion(completion, event_), "Wait for bridge completion");
    const auto result = WaitForSingleObject(event_, 30000); // Fault isolation, never a frame-rate assertion.
    if (result != WAIT_OBJECT_0)
        throw std::runtime_error("GPU bridge failed to drain; resources must remain retained");
    graphicsCheck(device12_->GetDeviceRemovedReason(), "D3D12 completion health");
    // An earlier timed-out registration can signal this reusable event late.
    // Notification is not proof that THIS fence value has retired; fail closed.
    const auto observed = completed12_->GetCompletedValue();
    if (observed == UINT64_MAX || observed < completion)
        throw std::runtime_error(
            "GPU fence notification did not prove requested retirement; resources remain retained");
}
void Dx11Dx12::drain() {
    handoffTo12();
    const auto value = ++next12_;
    graphicsCheck(queue12_->Signal(completed12_.Get(), value), "Signal bridge retirement");
    wait12(value);
}
void Dx11Dx12::transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                          D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}
} // namespace dspaa
