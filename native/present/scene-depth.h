#pragma once
#include "graphics/dx11-dx12.h"
#include <memory>

namespace dspaa {
// Ordered private depth resampling for native-resolution world UI. The source
// is this frame's raw device depth (R32 float or a sampleable typeless D32 /
// D32S8 attachment), not linear eye distance or an assumed camera depth texture.
// It samples only the depth channel and never changes the source stencil.
// The caller checks active application queries before this state-restored draw.
class SceneDepth {
  public:
    explicit SceneDepth(std::shared_ptr<Dx11Dx12> graphics);
    ~SceneDepth();
    void copy(ID3D11Texture2D* source, ID3D11Texture2D* destination, float shiftX, float shiftY);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
