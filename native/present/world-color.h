#pragma once
#include "graphics/dx11-dx12.h"
#include <array>
#include <memory>

namespace dspaa {
struct PresentationSubmission;
// One ordered copy at the end of the main world camera, before screen UI.
// This observes no draws, shaders, constants, materials or postprocess passes.
class WorldColor {
    struct Image {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        bool srgbView = false;
    };
    std::shared_ptr<Dx11Dx12> bridge_;
    // Display surface/format and producer liveness only, not the current
    // camera's pixels: Unity may rescale its logical output here after EOF.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
    std::array<std::shared_ptr<Image>, 4> slots_;
    std::shared_ptr<Image> pending_;
    uint64_t generation_ = 0, frame_ = 0;

  public:
    explicit WorldColor(std::shared_ptr<Dx11Dx12> bridge) : bridge_(std::move(bridge)) {}
    void surface(ID3D11Texture2D* source, uint64_t generation);
    // Temporarily release our internal backbuffer reference for DXGI's external
    // reference-count check. Rebinding the same generation preserves the snapshot.
    void releaseSurfaceReference();
    void capture(uint64_t frame);
    void attach(PresentationSubmission& submission);
};
} // namespace dspaa
