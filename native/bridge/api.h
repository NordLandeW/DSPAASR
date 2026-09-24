#pragma once

#include <stdint.h>

#ifdef DSPAA_BRIDGE_EXPORTS
#define DSPAA_API extern "C" __declspec(dllexport)
#else
#define DSPAA_API extern "C" __declspec(dllimport)
#endif

// All fields have explicit ABI sizes; Windows x64 only. Texture pointers are
// ID3D11Resource pointers obtained from stable, owned Unity RenderTextures.
struct DspAaFrame {
    uint32_t size;
    uint32_t version;
    uint64_t camera;
    uint64_t frame;
    void* color;
    void* output;
    void* depth;
    void* motion;
    uint32_t width;
    uint32_t height;
    uint32_t preset;
    uint32_t flags; // bit 0: HDR, bit 1: inverted depth, bit 2: reset history
    float jitterX;
    float jitterY;
    float motionScaleX;
    float motionScaleY;
    float frameTimeMilliseconds;
    uint32_t reserved;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t quality; // wire: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 UltraPerformance
    uint32_t reserved2;
};

struct DspAaStatus {
    uint32_t size;
    int32_t result; // 0: no completed frame, 1: DLSS/DLAA, -1: fallback/error, 2: released
    uint64_t frame;
    uint32_t requestedPreset;
    uint32_t observedPreset; // ASCII letter, 0 when unknown
    uint32_t verification;   // dspaa::PresetVerdict, diagnostics only
    uint32_t reserved;
    char message[256]; // UTF-8, NUL-terminated
};

struct DspAaOptimalSettings {
    uint32_t size;
    int32_t result; // 0: pending, 1: ready, -1: error
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t quality;
    uint32_t optimalWidth;
    uint32_t optimalHeight;
    uint32_t minWidth;
    uint32_t minHeight;
    uint32_t maxWidth;
    uint32_t maxHeight;
    char message[256]; // UTF-8, NUL-terminated
};

// Additive ABI v2 query. A completed result is consumed exactly once by GetSupport.
struct DspAaSupport {
    uint32_t size;
    int32_t result;    // 0: pending, 1: available, -1: unavailable/error
    char message[256]; // UTF-8, NUL-terminated
};

using DspAaRenderEvent = void(__stdcall*)(int eventId, void* token);

DSPAA_API uint32_t __cdecl DspAaGetAbiVersion();
DSPAA_API int __cdecl DspAaInitialize(const wchar_t* runtimeDirectory, const wchar_t* dataDirectory);
DSPAA_API DspAaRenderEvent __cdecl DspAaGetRenderEvent();
// Queue calls retain COM resources but do not evaluate NGX or touch D3D context state.
// Issue the returned token exactly once with IssuePluginEventAndData(callback, 1, token).
DSPAA_API void* __cdecl DspAaQueueFrame(const DspAaFrame* frame);
// The anchor is retained until consumption/cancellation. Query only on the render thread.
DSPAA_API void* __cdecl DspAaQueueOptimalSettings(uint64_t camera, void* deviceResource, uint32_t outputWidth,
                                                  uint32_t outputHeight, uint32_t quality);
DSPAA_API int __cdecl DspAaGetOptimalSettings(uint64_t camera, DspAaOptimalSettings* output);
// Checks the actual resource's device on Unity's render thread, without creating a DLSS feature.
DSPAA_API void* __cdecl DspAaQueueSupport(void* deviceResource);
DSPAA_API int __cdecl DspAaGetSupport(void* token, DspAaSupport* output);
DSPAA_API void* __cdecl DspAaQueueRelease(uint64_t camera);
DSPAA_API void* __cdecl DspAaQueueShutdown();
// Cancel only when the caller did NOT submit the command to Unity.
DSPAA_API void __cdecl DspAaCancel(void* token);
DSPAA_API int __cdecl DspAaGetStatus(uint64_t camera, DspAaStatus* status);
