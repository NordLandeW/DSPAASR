#pragma once
#include "graphics/dx11-dx12.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dspaa {
// All calls below execute in the ORIGINAL Unity immediate-context command order
// (normally ordered render events), never by setting a main-thread global flag.
// The caller owns camera/pass identification and the validity of its shader
// sampling contracts. This module never invokes Unity or advances CPU history.
enum class CaptureScopeKind {
    SharedPreparation,      // Original only; invalidate stale twins of its outputs.
    DualColor,              // Original draw, then the same raster work on clean inputs.
    FullOnlyUiCoverage,     // Original UI; independent private stencil/T/contribution.
    PartialWrite,           // DualColor, preserving unwritten destination pixels.
    ExternalBlurPublication // Original only; its published background remains shared.
};
struct CaptureTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    uint64_t epoch = 0; // Nonzero logical allocation/checkout generation, not a COM refcount.
};
struct CaptureFrameInfo {
    uint64_t applicationFrameId = 0, generation = 0;
    unsigned width = 0, height = 0;
};
// uvSource = matrix * uvDestination + bias. Offsets/radius are SOURCE texels.
// Radial SunShaft taps are affine maps too; a DoF box can conservatively bound
// the actual shared-CoC kernel. These describe support, NOT original RGB filters.
struct CaptureSampleDomain {
    std::array<float, 4> matrix{1, 0, 0, 1};
    std::array<float, 2> bias{0, 0};
    std::array<float, 2> offsetTexels{0, 0};
    std::array<unsigned, 2> radiusTexels{0, 0};
};
struct CaptureSamplingInput {
    unsigned pixelResourceSlot = 0, pixelSamplerSlot = 0;
    std::vector<CaptureSampleDomain> domains;
};
struct CaptureSupport {
    // Nonempty, specific provenance, e.g. pinned shader/pass + actual kernel
    // parameters. The adapter must verify it; a string is not shader validation.
    std::string basis;
    // Complete set of color-dependent PS inputs read by this particular pass.
    // Shared exposure/CoC/LUT/noise are NOT color-dependent support inputs.
    std::vector<CaptureSamplingInput> inputs;
    // Carry actual geometric occlusion from the primary color input, rather
    // than treating Bloom support as physical opacity. -1 means no occlusion.
    int occlusionResourceSlot = -1;
    CaptureSampleDomain occlusionDomain;
};
struct CaptureScope {
    CaptureScopeKind kind = CaptureScopeKind::SharedPreparation;
    uint64_t passId = 0; // Nonzero, verified game-adapter pass identity.
    CaptureSupport support;
    // Whole-subresource writes may start fresh; partial/scissored/clip draws must
    // preserve a valid existing clean destination. PartialWrite always preserves.
    bool fullOverwrite = false;
    // A trusted whole-output Graphics.Blit may name CameraTarget rather than a
    // Texture object. Declare its actual draw RTV with this explicit value epoch,
    // not the RTV which happened to be bound when the scope command executed.
    uint64_t implicitOutputEpoch = 0;
};
struct CaptureUiShaderPolicy {
    // Verify actual shader + bound texture/sampler/vertex/material conditions.
    // Ordinary SrcAlpha/InvSrcAlpha and replace do not need RGB nonnegativity.
    bool unitAlphaVerified = false;
    bool nonnegativeRgbVerified = false;
    // Explicit expensive fallback, ONLY with proof that this actual draw has no
    // overlapping fragments per pixel (including instancing/geometry expansion).
    // Dest=ZERO followed by per-draw support union cannot handle self-overlap.
    bool signedContributionWithoutSelfOverlapVerified = false;
    // Alpha-magnitude support cannot detect zero-weight NaN/Inf RGB propagation.
    // This is independent of whether finite signed RGB is permitted.
    bool finiteRgbVerified = false;
    // Explicit opt-in ONLY for ordinary SrcAlpha/InvSrcAlpha with verified unit
    // alpha but unknown RGB finiteness. M may cover every surviving fragment,
    // including alpha-zero geometry; this is not geometric opacity or exact RGB influence.
    bool conservativeFragmentSupportAllowed = false;
};
struct CaptureOwnerCreateInfo {
    std::shared_ptr<Dx11Dx12> graphics; // Existing same-device bridge; no second 12 device.
    // Called only for an actual captured UI PS, on the render thread. Must be
    // bounded/read-only and return false for an unverified shader family.
    std::function<CaptureUiShaderPolicy(ID3D11PixelShader*)> uiShaderPolicy;
    // Resolve the complete effect dependency/UV contract from the ACTUAL draw,
    // before any private raster work. A declaration/basis alone is not a proof.
    std::function<bool(ID3D11PixelShader*, const CaptureScope&, CaptureSupport&)> effectShaderPolicy;
    // Optional read-only diagnostic before the first unexplained color write.
    // Failures in this callback never suppress or replay the original draw.
    std::function<void(ID3D11RenderTargetView*)> unannotatedDraw;
    // These bound owned capture memory/leases, not frame-time correctness.
    unsigned maximumInFlightFrames = 3;
    unsigned maximumTrackedTextures = 128;
    uint64_t maximumFrameBytes = 512ull * 1024 * 1024; // Private planes/depth, conservatively estimated.
};
struct CaptureStatus {
    bool hooksReady = false, active = false, cleanComplete = false;
    bool occlusionComplete = false, influenceComplete = false, quarantined = false;
    uint64_t applicationFrameId = 0, generation = 0;
    uint64_t observedDraws = 0, colorReplays = 0, coverageReplays = 0, supportPasses = 0;
    uint64_t mirroredTransfers = 0, invalidations = 0, signedContributionDraws = 0;
    uint64_t alphaSupportReplays = 0, alphaDepthCopies = 0;
    // Nonzero counters expose deliberately widened raster/clip support in M.
    uint64_t conservativeFragmentPromotions = 0, conservativeFragmentReplays = 0;
    uint64_t allocatedFrameBytes = 0;
    std::string reason;
};
struct CapturedUiFrame {
    CaptureFrameInfo frame;
    CaptureStatus status;
    // Only a complete plane is exposed. Native color keeps its source encoding;
    // occlusion/influence are single-channel linear [0,1]. Neither is Final-H.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> clean, occlusion, influence;
    Microsoft::WRL::ComPtr<ID3D11Fence> readyFence;
    uint64_t readyValue = 0;
    // Holds every private intermediate/resource needed by the submitted GPU work.
    // The consumer retains the ENTIRE frame lease through its source-copy fence.
    std::shared_ptr<const void> storage;
};
using CapturedUiLease = std::shared_ptr<const CapturedUiFrame>;
class CaptureOwner {
  public:
    explicit CaptureOwner(const CaptureOwnerCreateInfo& info);
    ~CaptureOwner();
    CaptureOwner(const CaptureOwner&) = delete;
    CaptureOwner& operator=(const CaptureOwner&) = delete;
    bool beginFrame(const CaptureFrameInfo& frame) noexcept;
    // Declare a new allocation epoch before use. Re-declaring the same epoch is
    // harmless; a changed epoch invalidates old dependencies even for the same COM object.
    bool declareTexture(const CaptureTexture& texture) noexcept;
    void invalidateTexture(const CaptureTexture& texture) noexcept;
    // Seed from the once-resolved, already HUD-excluded world. Copies the full
    // native subresource into clean and initializes geometric T=1, influence=0.
    bool seedCleanRoot(const CaptureTexture& source) noexcept;
    // Seed a later camera's actual image-effect source from a known previous
    // color branch (normal UI/Top camera handoff), without copying its UI into clean.
    bool bindCleanInput(const CaptureTexture& fullSource, const CaptureTexture& previousColor) noexcept;
    bool beginScope(const CaptureScope& scope) noexcept;
    bool endScope() noexcept;
    // Exact output of the latest completed color draw in this scope. Empty on
    // no draw/failure or retirement; never aliases a guessed global backbuffer.
    CaptureTexture lastScopeOutput() const;
    // Query an already tracked value without declaring or invalidating it. Used
    // at camera handoffs and final sealing; never manufactures a clean input.
    CaptureTexture currentColor(ID3D11Texture2D* texture) const;
    // Records queued GPU completion, not a CPU/GPU wait. No SDK invocation.
    // A missing/unsupported plane has null texture and an explicit status reason.
    CapturedUiLease seal(const CaptureTexture& finalColor) noexcept;
    void abort(const char* reason) noexcept;
    CaptureStatus status() const;
    // Query gate for external private raster helpers; caller also checks pureRaster
    // and retains the shared graphics lock through state save/draw/restore.
    bool privateRasterAllowed() const;
    // After abort/seal and caller-provided GPU drain, release application texture
    // references from retired arenas without releasing their private outputs.
    // Refuses an active arena, an incomplete fence, or device removal. Existing
    // CapturedUiLease objects remain valid through their consumer-copy fence.
    bool releaseApplicationReferences() noexcept;
    // Caller must provide graphics quiescence. Hooks remain installed and their
    // code remains resident; no global MinHook Uninitialize/hot-unload occurs.
    // False means the complete owner remains quarantined rather than freeing in-flight state.
    bool stop() noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace dspaa
