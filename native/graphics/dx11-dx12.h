#pragma once

#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>
#include <string>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace dspaa {
void graphicsCheck(HRESULT result, const char* operation);

struct SharedTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dx11;
    Microsoft::WRL::ComPtr<ID3D12Resource> dx12;
};

// An owned fence/value pair: values from different API timelines are not comparable.
struct FencePoint {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    void arm() {
        fence.Reset();
        value = UINT64_MAX;
    }
    bool unpublished() const {
        return value == UINT64_MAX || (value && !fence);
    }
    bool complete() const;
};

// One bridge per actual Unity device, not one per reconstruction algorithm.
// The render-thread owner serializes this object with all immediate-context users.
// COMMON is the ownership boundary for every shared texture.
class Dx11Dx12 {
  public:
    explicit Dx11Dx12(ID3D11Device* device);
    ~Dx11Dx12();
    Dx11Dx12(const Dx11Dx12&) = delete;
    Dx11Dx12& operator=(const Dx11Dx12&) = delete;
    ID3D11Device* device11() const {
        return device11_.Get();
    }
    ID3D11DeviceContext4* context11() const {
        return context11_.Get();
    }
    ID3D12Device* device12() const {
        return device12_.Get();
    }
    ID3D12CommandQueue* queue12() const {
        return queue12_.Get();
    }
    // SR and the presentation adapter share the device and this queue timeline.
    // Hold a batch lock while submitting immediate-context copies and handoffs.
    std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock(access_); }
    ID3D12Fence* completionFence12() const { return completed12_.Get(); }
    ID3D12Fence* producerFence12() const {
        return produced12_.Get();
    }
    SharedTexture texture(unsigned width, unsigned height, DXGI_FORMAT format, bool unorderedAccess);
    // Publish preceding immediate-context work; any D3D12 consumer can wait on
    // producerFence12() without an intermediate bridge-queue submission.
    uint64_t signal11();
    // Publish preceding 11 work and enqueue a GPU-side wait on 12; no CPU readback.
    void handoffTo12();
    // Publish preceding 12 work without making unrelated D3D11 rendering wait.
    // Consumers and pool reuse must still honor this completion fence/value.
    uint64_t signal12();
    // Publish preceding 12 work and enqueue a GPU-side wait on 11.
    uint64_t handoffTo11();
    void wait12(uint64_t completion);
    void wait(const FencePoint& completion);
    // Includes 11 copies submitted after handoffTo11, before resources are retired.
    void drain();
    static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  private:
    std::recursive_mutex access_;
    Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> produced11_, completed11_;
    Microsoft::WRL::ComPtr<ID3D12Fence> produced12_, completed12_;
    uint64_t next11_ = 0, next12_ = 0;
    HANDLE event_ = nullptr;
};
std::shared_ptr<Dx11Dx12> acquireDx11Dx12(ID3D11Device* device);
} // namespace dspaa
