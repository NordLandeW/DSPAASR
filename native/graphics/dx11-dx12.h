#pragma once

#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>
#include <string>
#include <wrl/client.h>

namespace dspaa {
void graphicsCheck(HRESULT result, const char* operation);

struct SharedTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dx11;
    Microsoft::WRL::ComPtr<ID3D12Resource> dx12;
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
    SharedTexture texture(unsigned width, unsigned height, DXGI_FORMAT format, bool unorderedAccess);
    // Publish preceding 11 work and enqueue a GPU-side wait on 12; no CPU readback.
    void handoffTo12();
    // Publish preceding 12 work and enqueue a GPU-side wait on 11.
    uint64_t handoffTo11();
    void wait12(uint64_t completion);
    // Includes 11 copies submitted after handoffTo11, before resources are retired.
    void drain();
    static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  private:
    Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> produced11_, completed11_;
    Microsoft::WRL::ComPtr<ID3D12Fence> produced12_, completed12_;
    uint64_t next11_ = 0, next12_ = 0;
    HANDLE event_ = nullptr;
};
} // namespace dspaa
