#pragma once
#include "frame.h"

namespace dspaa {
// One per retired transport slot. All work executes on the shared bridge queue,
// between its 11->12 and 12->11 fences; no Unity graphics state is modified.
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
    // Caller proves both this slot's preceding ready fence and every consumer
    // retired before reuse (the FrameImages lease retains the converted output).
    // Source is a full logical image; rect is its exact display content extent.
    PresentImage convert(ID3D12Device* device, ID3D12CommandQueue* queue, const PresentImage& source,
                         unsigned width, unsigned height, DXGI_FORMAT format, RECT rect, bool srgbFilter);
};
} // namespace dspaa
