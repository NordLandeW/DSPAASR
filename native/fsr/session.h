#pragma once
#include "graphics/dx11-dx12.h"
#include <filesystem>
#include <memory>
#include <string>

namespace dspaa {
struct FsrConfiguration {
    unsigned inputWidth{}, inputHeight{}, outputWidth{}, outputHeight{};
    bool hdr = true, invertedDepth = false;
    bool reactiveMask = false;
    bool operator==(const FsrConfiguration&) const = default;
};
struct FsrFrame {
    ID3D11Resource* color{};
    ID3D11Resource* depth{};
    ID3D11Resource* motion{};
    ID3D11Resource* output{};
    ID3D11Resource* opaqueColor{};
    float jitterX{}, jitterY{}, motionScaleX = 1, motionScaleY = 1;
    float milliseconds = 16.666667f;
    float cameraNear = 0.1f, cameraFar = 1000, verticalFov = 1.04719755f;
    float preExposure = 1, viewSpaceToMeters = 1, sharpness = 0;
    bool reset = false;
};
struct FsrResolution {
    unsigned width{}, height{}, phases{};
};

class FsrDevice {
  public:
    FsrDevice(std::shared_ptr<Dx11Dx12> bridge, const std::filesystem::path& runtime);
    ~FsrDevice();
    FsrDevice(const FsrDevice&) = delete;
    FsrDevice& operator=(const FsrDevice&) = delete;
    FsrResolution resolution(unsigned width, unsigned height, unsigned quality);
    const std::string& version() const;
    std::shared_ptr<Dx11Dx12> bridge() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class FsrFeature;
};
class FsrFeature {
  public:
    explicit FsrFeature(FsrDevice& device);
    ~FsrFeature();
    FsrFeature(const FsrFeature&) = delete;
    FsrFeature& operator=(const FsrFeature&) = delete;
    bool configure(const FsrConfiguration& configuration);
    void evaluate(const FsrFrame& frame);
    void drain();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dspaa
