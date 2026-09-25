#pragma once
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <wrl/client.h>

namespace dspaa {
struct PresentImage {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
// The producer pool must not overwrite any image while a consumer holds this
// shared owner. Backend copies restore the delivered state and retain the lease
// until their copy fence actually completes, not merely until Present returns.
struct FrameImages {
    PresentImage finalColor, hudless, depth, motion;
    PresentImage occlusionAlpha, uiInfluence;
    PresentImage fsrDistortion; // Optional RG normalized UV_after - UV_before.
    PresentImage slDistortion; // Optional RGBA bidirectional mapping in the SL contract.
};
struct PresentationFrame {
    uint64_t generation = 0, applicationFrameId = 0;
    std::shared_ptr<const FrameImages> images;
    std::shared_ptr<const void> producerLifetime; // Retain original 11 inputs through the ready/copy fences.
    Microsoft::WRL::ComPtr<ID3D12Fence> readyFence;
    uint64_t readyValue = 0;
    bool inputsComplete = false, reset = true, depthInverted = false, depthInfinite = false;
    unsigned renderWidth = 0, renderHeight = 0;
    float jitterX = 0, jitterY = 0, motionScaleX = 1, motionScaleY = 1;
    float deltaMilliseconds = 0, cameraNear = 0, cameraFar = 0, verticalFov = 0;
    float preExposure = 1, viewSpaceToMeters = 1;
    std::array<float, 3> cameraPosition{}, cameraUp{}, cameraRight{}, cameraForward{};
    // Row-major matrices in the SDK's documented coordinate convention. These
    // are derived from the exact same camera token, never a later Unity frame.
    std::array<float, 16> cameraViewToClip{}, clipToCameraView{}, clipToPrevClip{}, prevClipToClip{};
    bool cameraMotionIncluded = true, motionVectorsJittered = false;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    float minLuminance = 0, maxLuminance = 100;
    RECT generationRect{}; // All zero selects the full output; otherwise exact output pixel bounds.
};
using FrameLease = std::shared_ptr<const PresentationFrame>;
// Borrowed synchronous host callbacks around the actual application Present,
// after input-copy submission. SDK-generated Presents never call these.
struct PresentBoundary {
    void* context = nullptr;
    bool (*before)(void* context, uint64_t applicationFrameId) noexcept = nullptr;
    void (*after)(void* context, uint64_t applicationFrameId) noexcept = nullptr;
};
struct PresentArguments {
    unsigned syncInterval = 0, flags = 0;
    const DXGI_PRESENT_PARAMETERS* parameters = nullptr; // Borrowed for this synchronous call only.
    const PresentBoundary* boundary = nullptr;
};
enum class PresentRetirement { Drained, Quarantined };
} // namespace dspaa
