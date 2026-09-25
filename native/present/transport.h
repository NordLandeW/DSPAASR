#pragma once
#include "channel.h"
#include "display-color.h"
#include "graphics/dx11-dx12.h"

namespace dspaa {
class PresentationTransport {
    struct Slot {
        std::array<SharedTexture, 6> textures;
        std::shared_ptr<FrameImages> images;
        DisplayColor color;
        uint64_t readyValue = 0;
    };
    std::shared_ptr<Dx11Dx12> bridge_;
    std::array<Slot, 4> slots_;
    RECT previousRect_{};
    unsigned previousLogicalWidth_ = 0, previousLogicalHeight_ = 0, previousRenderWidth_ = 0,
             previousRenderHeight_ = 0;
    uint64_t previousGeneration_ = 0;
    bool previousComplete_ = false, previousSrgb_ = false;
    void copy(Slot& slot, size_t index, ID3D11Resource* input, DXGI_FORMAT format, PresentImage& output);

  public:
    explicit PresentationTransport(std::shared_ptr<Dx11Dx12> bridge) : bridge_(std::move(bridge)) {}
    // Called by the unique render/presentation owner. Final is always genuine;
    // incomplete/stale temporal inputs never suppress the original application frame.
    FrameLease capture(ID3D11Texture2D* finalColor, uint64_t generation, DXGI_COLOR_SPACE_TYPE colorSpace,
                       std::unique_ptr<PresentationSubmission> submission, std::string& reason);
    // Caller must first prove both backend and shared-queue retirement.
    void reset() {
        for (auto& slot : slots_)
            slot = Slot{};
        previousComplete_ = false;
    }
};
} // namespace dspaa
