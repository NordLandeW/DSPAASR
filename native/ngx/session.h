#pragma once

#include <d3d11.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>

#include <filesystem>

namespace dspaa {
struct DlssOptimalSettings {
    unsigned optimalWidth{};
    unsigned optimalHeight{};
    unsigned minWidth{};
    unsigned minHeight{};
    unsigned maxWidth{};
    unsigned maxHeight{};
};

struct DlssConfiguration {
    unsigned inputWidth{};
    unsigned inputHeight{};
    unsigned outputWidth{};
    unsigned outputHeight{};
    NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_DLAA;
    NVSDK_NGX_DLSS_Hint_Render_Preset preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    bool hdr = true;
    bool invertedDepth = false;
    bool autoExposure = true;
    bool operator==(const DlssConfiguration&) const = default;
};

struct DlssFrame {
    ID3D11Resource* color{};
    ID3D11Resource* output{};
    ID3D11Resource* depth{};
    ID3D11Resource* motionVectors{};
    ID3D11Resource* exposure{};
    float jitterX{};
    float jitterY{};
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    float preExposure = 1.0f;
    float frameTimeMilliseconds = 1000.0f / 60.0f;
    bool reset = false;
};

// All NGX operations, including destruction, must be serialized by the caller on
// the render thread. Exactly one device owner per native graphics device; destroy
// every DlssFeature before its NgxDevice. Never invoke these through main-thread P/Invoke.
class NgxDevice {
  public:
    NgxDevice(ID3D11Device* device, const std::filesystem::path& runtimeDirectory,
              const std::filesystem::path& dataDirectory, NVSDK_NGX_AppLogCallback logCallback,
              NVSDK_NGX_EngineType engine = NVSDK_NGX_ENGINE_TYPE_CUSTOM,
              const char* engineVersion = "DSPAAMod-native-0.1");
    ~NgxDevice();
    NgxDevice(const NgxDevice&) = delete;
    NgxDevice& operator=(const NgxDevice&) = delete;
    [[nodiscard]] ID3D11Device* device() const noexcept {
        return device_.Get();
    }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept {
        return context_.Get();
    }
    [[nodiscard]] DlssOptimalSettings optimalSettings(unsigned outputWidth, unsigned outputHeight,
                                                      NVSDK_NGX_PerfQuality_Value quality);

  private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    bool initialized_ = false;
};

class DlssFeature {
  public:
    explicit DlssFeature(NgxDevice& device);
    ~DlssFeature();
    DlssFeature(const DlssFeature&) = delete;
    DlssFeature& operator=(const DlssFeature&) = delete;
    // Returns true only when a new temporal feature/history was created.
    bool configure(const DlssConfiguration& configuration);
    void evaluate(const DlssFrame& frame);
    void resetHistory() noexcept {
        resetHistory_ = true;
    }
    [[nodiscard]] const DlssConfiguration& configuration() const noexcept {
        return configuration_;
    }

  private:
    NgxDevice& device_;
    NVSDK_NGX_Parameter* parameters_{};
    NVSDK_NGX_Handle* feature_{};
    DlssConfiguration configuration_{};
    bool resetHistory_ = true;
};
} // namespace dspaa
