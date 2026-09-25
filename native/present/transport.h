#pragma once
#include "channel.h"
#include "graphics/dx11-dx12.h"

namespace dspaa {
class PresentationTransport {
    struct Slot {
        std::array<SharedTexture, 8> textures;
        std::shared_ptr<FrameImages> images;
    };
    std::shared_ptr<Dx11Dx12> bridge_;
    std::array<Slot, 4> slots_;
    void copy(Slot& slot, size_t index, ID3D11Resource* input, DXGI_FORMAT format, PresentImage& output);
  public:
    explicit PresentationTransport(std::shared_ptr<Dx11Dx12> bridge) : bridge_(std::move(bridge)) {}
    // Called by the unique render/presentation owner. Final is always genuine;
    // incomplete/stale temporal inputs never suppress the original application frame.
    FrameLease capture(ID3D11Texture2D* finalColor, uint64_t generation, DXGI_COLOR_SPACE_TYPE colorSpace,
                       std::unique_ptr<PresentationSubmission> submission, std::string& reason);
    // Caller must first prove both backend and shared-queue retirement.
    void reset() { for (auto& slot : slots_) slot = Slot{}; }
};
} // namespace dspaa
