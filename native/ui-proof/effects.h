#pragma once
#include "capture/owner.h"
#include <array>
#include <functional>
#include <string>

namespace dspaa {
struct EffectLayout;
struct EffectUniformPin;
struct EffectTexturePin;
// The caller verifies the actual VS/PS pairing, bound CB slice and SRV/sampler,
// and establishes the original fullscreen Blit's raw UV q in [0,1]^2. Individual
// stage hashes with the same pass ID do not by themselves verify a pairing.
// Components not declared by the uniform pin are zero. Texture dimensions are
// those of the validated SRV subresource, not the output or allocation base mip.
using ReadEffectUniform = std::function<bool(bool vertex, const EffectUniformPin&, std::array<float, 4>&)>;
using ReadEffectTexture = std::function<bool(const EffectTexturePin&, unsigned& width, unsigned& height)>;
// Pure dependency/geometry resolver: no context access or GPU work. Failure
// clears output, supplies a reason, and never promotes the caller's old support.
bool resolveEffectSupport(const CaptureScope&, const EffectLayout& vertex, const EffectLayout& pixel,
                          const ReadEffectUniform&, const ReadEffectTexture&, CaptureSupport&,
                          std::string& reason);
} // namespace dspaa
