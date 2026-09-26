#pragma once
#include "frame.h"

namespace dspaa {
// One per retired transport slot. All work executes on the shared bridge queue,
// after the 11->12 handoff and before its ready fence; no Unity state is modified.
class DisplayColor {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_, targets_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> output_;
    DXGI_FORMAT pipelineFormat_ = DXGI_FORMAT_UNKNOWN;
    void initialize(ID3D12Device* device, DXGI_FORMAT format);

  public:
    // Caller proves this allocator's last conversion fence/value and every consumer
    // retired before reuse, even across intervening pure-D3D11 publications.
    // The FrameImages lease retains the converted output.
    // Source is a full logical image; rect is its exact display content extent.
    PresentImage convert(ID3D12Device* device, ID3D12CommandQueue* queue, const PresentImage& source,
                         unsigned width, unsigned height, DXGI_FORMAT format, RECT rect, bool srgbFilter);
};
} // namespace dspaa
