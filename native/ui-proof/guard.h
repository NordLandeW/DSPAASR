#pragma once
#include "capture/owner.h"
#include <functional>
#include <memory>
#include <string>

namespace dspaa {
struct UiProofStatus {
    uint64_t inspected = 0, accepted = 0, copiedConstantBytes = 0, invalidations = 0;
    std::string reason, shaderHash;
};
// Game-specific DXBC identities plus actual IA/SRV/sampler/CB conditions. Shader
// names, serialized material defaults and the hash alone never grant a policy.
class UiShaderProof {
  public:
    UiShaderProof(std::shared_ptr<Dx11Dx12> graphics, std::function<void(const char*)> log = {});
    ~UiShaderProof();
    UiShaderProof(const UiShaderProof&) = delete;
    UiShaderProof& operator=(const UiShaderProof&) = delete;
    // Explicit shutdown result for the capture/presentation owner. Only this
    // closing path may drain the graphics bridge; frame-time inspection never waits.
    bool stop() noexcept;
    // Retain the first proof failure within one render frame, not the last
    // successful inspection. Lifetime counters and constant shadows persist.
    void beginFrame();
    // The capture owner holds the graphics lock while invoking either policy.
    CaptureUiShaderPolicy inspect(ID3D11PixelShader* shader) noexcept;
    bool inspectEffect(ID3D11PixelShader* shader, const CaptureScope& scope,
                       const capture::DrawArguments& arguments, CaptureSupport& support) noexcept;
    UiProofStatus status() const;
    // Diagnostic identity only; a fingerprint does not authorize any replay.
    static std::string shaderFingerprint(ID3D11DeviceChild* shader);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace dspaa
