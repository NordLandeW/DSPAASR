#pragma once
#include "bridge/api.h"

inline constexpr uint32_t DspAaPresentationAbiVersion = 3;

// Independent, additive presentation ABI. The SR ABI stays at version 2.
// All calls are Windows x64. Resource pointers are owned D3D11 resources;
// queueing retains COM references, but only the render event consumes pixels.
struct DspAaPresentationConfiguration {
    uint32_t size, version;
    uint32_t backend;             // 0 Native, 1 analytical FSR, 2 DLSS FG
    uint32_t mode;                // 0 fixed, 1 Dynamic (DLSS only)
    uint32_t generatedFrames;     // Additional frames, NOT the display multiplier.
    uint32_t reflex;              // 0 Off, 1 On, 2 On+Boost
    float dynamicTargetFrameRate; // 0 selects the monitor.
    uint32_t frameLimitMicroseconds;
};
struct DspAaPresentationBegin {
    uint32_t size, version;
    uint64_t applicationFrameId, generation;
};
struct DspAaPresentationInputs {
    uint32_t size, version;
    uint64_t applicationFrameId, generation;
    void* hudless;
    void* depth;
    void* motion;
    void* fsrDistortion;
    void* slDistortion;
    uint32_t renderWidth, renderHeight;
    // complete, reset, reversed depth, infinite depth, camera motion included,
    // jittered motion vectors. Incomplete frames still carry their real ID.
    uint32_t flags, reserved;
    float jitterX, jitterY, motionScaleX, motionScaleY;
    float milliseconds, cameraNear, cameraFar, verticalFov;
    float preExposure, viewSpaceToMeters, minLuminance, maxLuminance;
    float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3];
    float cameraViewToClip[16], clipToCameraView[16], clipToPrevClip[16], prevClipToClip[16];
    int32_t generationRect[4]; // left, top, right, bottom; zero = full canonical output / observed logical
                               // layout.
    // HUDless domain: both zero means already canonical, full display storage.
    // Otherwise H has this logical output size. An explicit generationRect maps
    // it into the display; zero uses Unity's observed final-blit viewport when L!=D.
    // Depth/motion and all camera metadata remain in the local render domain.
    uint32_t logicalOutputWidth, logicalOutputHeight;
};
struct DspAaPresentationStatus {
    uint32_t size, version;
    // facade=1, FSR runtime present=2, DLSS supported=4, Reflex supported=8,
    // Dynamic=16, VSync=32, generation enabled=64, quarantined=128.
    // Enabled describes accepted configuration/inputs, not a generated/displayed frame count.
    uint32_t flags, requestedBackend, activeBackend, activeMode;
    uint32_t maximumGeneratedFrames, width, height, sdkStatus;
    uint64_t generation, lastApplicationFrameId, lastPresentedFrameId;
    uint64_t applicationPresents, generatedSubmissions, sdkReportedPresents;
    char message[384];
};
DSPAA_API uint32_t __cdecl DspAaGetPresentationAbiVersion();
DSPAA_API int __cdecl DspAaConfigurePresentation(const DspAaPresentationConfiguration* configuration);
// Main thread, before ANY input sampling. Never call from Present or a camera.
DSPAA_API int __cdecl DspAaBeginPresentationFrame(DspAaPresentationBegin* frame);
// Main-thread SimulationEnd only (phase 0). Rendering start is issued using the
// render event (eventId=1, token=the explicit application ID, not a pointer).
DSPAA_API int __cdecl DspAaPresentationSimulationEnd(uint64_t applicationFrameId);
// Queue the final immutable metadata/input envelope; issue its token with
// eventId=2 at end-of-frame. Cancel only if not submitted to Unity.
// World camera completion: eventId=3, token=application ID. Copies the currently
// bound world color attachment before screen UI; EOF consumes only a matching snapshot.
DSPAA_API void* __cdecl DspAaQueuePresentationInputs(const DspAaPresentationInputs* frame);
DSPAA_API void __cdecl DspAaCancelPresentationInputs(void* token);
DSPAA_API DspAaRenderEvent __cdecl DspAaGetPresentationRenderEvent();
DSPAA_API int __cdecl DspAaGetPresentationStatus(DspAaPresentationStatus* status);
