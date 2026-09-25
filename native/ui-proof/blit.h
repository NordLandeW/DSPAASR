#pragma once
#include "capture/hooks.h"
#include "capture/owner.h"
#include <span>

namespace dspaa::proof {
class ConstantShadow;
struct ClipBlitVertex {
    std::array<float, 3> position{};
    std::array<float, 2> uv{};
};
// This is the CPU geometric part only. The caller also proves the exact shader
// pair, input bytes, full raster domain, overwrite blend and unconditional tests.
bool clipBlitDomain(std::span<const ClipBlitVertex> vertices, D3D11_PRIMITIVE_TOPOLOGY topology,
                    CaptureSampleDomain& domain, std::string& reason);
// Store only IA metadata; never retain descriptors or shader-signature pointers.
void observeBlitLayout(ID3D11InputLayout*, const D3D11_INPUT_ELEMENT_DESC*, unsigned count) noexcept;
bool inspectClipBlit(ID3D11DeviceContext4*, ConstantShadow&, const capture::DrawArguments&,
                     unsigned sourceWidth, unsigned sourceHeight, CaptureSupport&, std::string& reason);
} // namespace dspaa::proof
