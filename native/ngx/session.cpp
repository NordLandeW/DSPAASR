#include "session.h"

#include <nvsdk_ngx_helpers.h>

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dspaa {
namespace {
constexpr char projectId[] = "cd751b03-626f-4384-a83e-30279c066322";

void requireNgx(NVSDK_NGX_Result result, const char* operation) {
    if (NVSDK_NGX_FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << static_cast<unsigned>(result);
        throw std::runtime_error(message.str());
    }
}

void validateConfiguration(const DlssConfiguration& config) {
    if (!config.inputWidth || !config.inputHeight || !config.outputWidth || !config.outputHeight) {
        throw std::invalid_argument("DLSS dimensions must be nonzero.");
    }
    if (config.quality == NVSDK_NGX_PerfQuality_Value_DLAA &&
        (config.inputWidth != config.outputWidth || config.inputHeight != config.outputHeight)) {
        throw std::invalid_argument("DLAA requires native 1:1 input/output dimensions.");
    }
    switch (config.preset) {
    case NVSDK_NGX_DLSS_Hint_Render_Preset_E:
    case NVSDK_NGX_DLSS_Hint_Render_Preset_F:
    case NVSDK_NGX_DLSS_Hint_Render_Preset_J:
    case NVSDK_NGX_DLSS_Hint_Render_Preset_K:
    case NVSDK_NGX_DLSS_Hint_Render_Preset_L:
    case NVSDK_NGX_DLSS_Hint_Render_Preset_M:
        break;
    default:
        throw std::invalid_argument("An explicit non-reserved DLSS preset is required.");
    }
}
} // namespace

NgxDevice::NgxDevice(ID3D11Device* device, const std::filesystem::path& runtimeDirectory,
                     const std::filesystem::path& dataDirectory, NVSDK_NGX_AppLogCallback logCallback,
                     NVSDK_NGX_EngineType engine, const char* engineVersion)
    : device_(device) {
    if (!device)
        throw std::invalid_argument("NGX requires a D3D11 device.");
    const auto runtime = std::filesystem::absolute(runtimeDirectory);
    if (!std::filesystem::is_regular_file(runtime / "nvngx_dlss.dll")) {
        throw std::invalid_argument("Missing nvngx_dlss.dll in the requested runtime directory.");
    }
    std::filesystem::create_directories(dataDirectory);
    device->GetImmediateContext(&context_);
    const auto runtimeString = runtime.wstring();
    const wchar_t* paths[] = {runtimeString.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = logCallback;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    info.LoggingInfo.DisableOtherLoggingSinks = logCallback != nullptr;
    requireNgx(NVSDK_NGX_D3D11_Init_with_ProjectID(projectId, engine, engineVersion,
                                                   std::filesystem::absolute(dataDirectory).c_str(), device,
                                                   &info),
               "NGX init");
    initialized_ = true;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    try {
        requireNgx(NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilities), "NGX capabilities");
        int needsDriver = 0;
        if (NVSDK_NGX_SUCCEED(
                capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver)) &&
            needsDriver) {
            std::ostringstream message;
            message << "Update the NVIDIA graphics driver";
            unsigned major = 0, minor = 0;
            if (NVSDK_NGX_SUCCEED(
                    capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major)) &&
                NVSDK_NGX_SUCCEED(
                    capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor)))
                message << " to version " << major << '.' << std::setfill('0') << std::setw(2) << minor
                        << " or newer";
            message << "; DLSS is unavailable with the current driver.";
            throw std::runtime_error(message.str());
        }
        int available = 0;
        requireNgx(capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available),
                   "DLSS availability");
        if (!available)
            throw std::runtime_error("NGX reports DLSS unavailable on this GPU/driver. A supported NVIDIA "
                                     "RTX GPU and compatible driver are required.");
        const auto destroyed = NVSDK_NGX_D3D11_DestroyParameters(capabilities);
        capabilities = nullptr;
        requireNgx(destroyed, "Destroy capabilities");
    } catch (...) {
        if (capabilities)
            NVSDK_NGX_D3D11_DestroyParameters(capabilities);
        NVSDK_NGX_D3D11_Shutdown1(device);
        initialized_ = false;
        throw;
    }
}

NgxDevice::~NgxDevice() {
    if (initialized_)
        NVSDK_NGX_D3D11_Shutdown1(device_.Get());
}

DlssOptimalSettings NgxDevice::optimalSettings(unsigned outputWidth, unsigned outputHeight,
                                               NVSDK_NGX_PerfQuality_Value quality) {
    if (!outputWidth || !outputHeight || outputWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        outputHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("Invalid DLSS output dimensions.");
    NVSDK_NGX_Parameter* capabilities = nullptr;
    DlssOptimalSettings settings;
    try {
        requireNgx(NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilities), "NGX optimal capabilities");
        float sharpness = 0.0f;
        requireNgx(NGX_DLSS_GET_OPTIMAL_SETTINGS(capabilities, outputWidth, outputHeight, quality,
                                                 &settings.optimalWidth, &settings.optimalHeight,
                                                 &settings.maxWidth, &settings.maxHeight, &settings.minWidth,
                                                 &settings.minHeight, &sharpness),
                   "Query DLSS optimal settings");
        const auto destroyed = NVSDK_NGX_D3D11_DestroyParameters(capabilities);
        capabilities = nullptr;
        requireNgx(destroyed, "Destroy optimal capabilities");
    } catch (...) {
        if (capabilities)
            NVSDK_NGX_D3D11_DestroyParameters(capabilities);
        throw;
    }
    if (!settings.minWidth || !settings.minHeight || settings.minWidth > settings.optimalWidth ||
        settings.minHeight > settings.optimalHeight || settings.optimalWidth > settings.maxWidth ||
        settings.optimalHeight > settings.maxHeight || settings.maxWidth > outputWidth ||
        settings.maxHeight > outputHeight)
        throw std::runtime_error("NGX returned invalid or unsupported optimal dimensions.");
    return settings;
}

DlssFeature::DlssFeature(NgxDevice& device) : device_(device) {
    requireNgx(NVSDK_NGX_D3D11_AllocateParameters(&parameters_), "Allocate feature parameters");
}

DlssFeature::~DlssFeature() {
    if (feature_)
        NVSDK_NGX_D3D11_ReleaseFeature(feature_);
    if (parameters_)
        NVSDK_NGX_D3D11_DestroyParameters(parameters_);
}

bool DlssFeature::configure(const DlssConfiguration& config) {
    validateConfiguration(config);
    if (feature_ && configuration_ == config)
        return false;
    if (feature_) {
        requireNgx(NVSDK_NGX_D3D11_ReleaseFeature(feature_), "Release previous DLSS feature");
        feature_ = nullptr;
    }
    parameters_->Reset();
    const char* presetKeys[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
    for (const char* key : presetKeys)
        parameters_->Set(key, static_cast<unsigned>(config.preset));
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = config.inputWidth;
    create.Feature.InHeight = config.inputHeight;
    create.Feature.InTargetWidth = config.outputWidth;
    create.Feature.InTargetHeight = config.outputHeight;
    create.Feature.InPerfQualityValue = config.quality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (config.hdr)
        create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (config.invertedDepth)
        create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (config.autoExposure)
        create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    requireNgx(NGX_D3D11_CREATE_DLSS_EXT(device_.context(), &feature_, parameters_, &create),
               "Create DLSS feature");
    configuration_ = config;
    resetHistory_ = true;
    return true;
}

void DlssFeature::evaluate(const DlssFrame& frame) {
    if (!feature_)
        throw std::logic_error("DLSS feature has not been configured.");
    if (!frame.color || !frame.output || !frame.depth || !frame.motionVectors) {
        throw std::invalid_argument("DLSS needs color, output, depth and motion vectors.");
    }
    if (!std::isfinite(frame.jitterX) || !std::isfinite(frame.jitterY) ||
        !std::isfinite(frame.motionScaleX) || !std::isfinite(frame.motionScaleY) ||
        !std::isfinite(frame.preExposure) || frame.preExposure <= 0.0f ||
        !std::isfinite(frame.frameTimeMilliseconds) || frame.frameTimeMilliseconds < 0.0f) {
        throw std::invalid_argument("Invalid temporal frame parameters.");
    }
    NVSDK_NGX_D3D11_DLSS_Eval_Params evaluate{};
    evaluate.Feature.pInColor = frame.color;
    evaluate.Feature.pInOutput = frame.output;
    evaluate.pInDepth = frame.depth;
    evaluate.pInMotionVectors = frame.motionVectors;
    evaluate.pInExposureTexture = frame.exposure;
    evaluate.InJitterOffsetX = frame.jitterX;
    evaluate.InJitterOffsetY = frame.jitterY;
    evaluate.InRenderSubrectDimensions = {configuration_.inputWidth, configuration_.inputHeight};
    evaluate.InReset = frame.reset || resetHistory_ ? 1 : 0;
    evaluate.InMVScaleX = frame.motionScaleX;
    evaluate.InMVScaleY = frame.motionScaleY;
    evaluate.InPreExposure = frame.preExposure;
    evaluate.InExposureScale = 1.0f;
    evaluate.InFrameTimeDeltaInMsec = frame.frameTimeMilliseconds;
    // The D3D11 NGX API preserves immediate-context state (SDK guide section 5.2.5).
    const auto result = NGX_D3D11_EVALUATE_DLSS_EXT(device_.context(), feature_, parameters_, &evaluate);
    resetHistory_ = NVSDK_NGX_FAILED(result);
    requireNgx(result, "Evaluate DLSS");
}
} // namespace dspaa
