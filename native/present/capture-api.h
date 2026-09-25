#pragma once
#include "api.h"

struct DspAaCaptureTexture { void* resource; uint64_t epoch; };
struct DspAaCaptureDomain {
    float matrix[4], bias[2], offsetTexels[2];
    uint32_t radiusTexels[2];
};
struct DspAaCaptureInput { uint32_t resourceSlot, samplerSlot, firstDomain, domainCount; };
// Queueing copies all metadata and retains supplied resources. Only the ordered
// render event executes it; neither this call nor a main-thread flag opens a scope.
struct DspAaCaptureCommand {
    uint32_t size, version, operation, scope;
    uint64_t applicationFrameId, generation, passId;
    // source=currently bound RTV (1), whole-output overwrite (2), previous=last
    // explicitly remembered color (4). A null ordinary resource means the facade shadow.
    // Actual scope output (8): BeginScope declares the actual full-overwrite draw
    // target with source.epoch; Remember uses that draw's retained output value.
    // Camera handoff (16, with 1|4): retain the actual target's already tracked
    // value if available; otherwise bind from the last explicitly remembered color.
    uint32_t flags, inputCount, domainCount;
    int32_t occlusionSlot;
    DspAaCaptureTexture source, previous;
    DspAaCaptureDomain occlusionDomain;
};
// operation: 0 declare, 1 invalidate, 2 seed resolved root, 3 camera clean handoff,
// 4 begin scope, 5 end scope, 6 abort, 7 remember color, 8 bounded binding trace,
// 9 point-resample raw scene depth: source -> previous depth attachment, bias=UV jitter.
// 10 follow trusted whole-output camera-tail copies (flags=10, passId=verified Copy),
// 11 end that optional tail. Explicit scopes/root handoffs end it first; no other
// unknown shader, partial geometry or input is made valid by this annotation.
// scope uses CaptureScopeKind's 0..4 order. passId is a nonzero adapter identity.
struct DspAaCaptureStatus {
    uint32_t size, version, flags, reserved; // ready=1, active=2, complete=4, quarantine=8
    uint64_t applicationFrameId, generation, completedUnityFrame;
    uint64_t draws, colorReplays, coverageReplays, supportPasses, privateBytes;
    uint64_t proofInspected, proofAccepted, copiedConstantBytes;
    char reason[384], pixelShaderHash[65];
};
// Exact render-thread execution receipt for operation 9. Succeeded means its
// GPU commands were submitted in order, NOT that the GPU has completed them.
// A caller must match frame, generation AND its nonzero requestId (passId).
struct DspAaDepthCopyResult {
    uint32_t size, version, flags, reserved; // executed=1, submitted successfully=2
    uint64_t applicationFrameId, generation, requestId;
};
DSPAA_API void* __cdecl DspAaQueueCaptureCommand(const DspAaCaptureCommand* command,
    const DspAaCaptureInput* inputs, const DspAaCaptureDomain* domains, const char* basis);
DSPAA_API void __cdecl DspAaCancelCaptureCommand(void* token);
DSPAA_API int __cdecl DspAaGetCaptureStatus(DspAaCaptureStatus* status);
DSPAA_API int __cdecl DspAaGetDepthCopyResult(DspAaDepthCopyResult* result);
