#include "api.h"
#include "core/preset-evidence.h"
#include "fsr/session.h"
#include "ngx/session.h"
#include "output-backup.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
static_assert(sizeof(DspAaFrame) == 112);
static_assert(offsetof(DspAaFrame, outputWidth) == 96);
static_assert(offsetof(DspAaFrame, outputHeight) == 100);
static_assert(offsetof(DspAaFrame, quality) == 104);
static_assert(offsetof(DspAaFrame, reserved2) == 108);
static_assert(sizeof(DspAaStatus) == 288);
static_assert(sizeof(DspAaOptimalSettings) == 300);
static_assert(sizeof(DspAaSupport) == 264);
static_assert(sizeof(DspAaFsrParameters) == 40);
static_assert(sizeof(DspAaFsrOptimalSettings) == 304);
constexpr uint32_t abiVersion = 2;
constexpr size_t maxPendingFrames = 16; // Shared frame/query backpressure for a stalled consumer.
enum class Kind { Frame, OptimalSettings, Support, Release, Shutdown };
struct Command {
    Kind kind = Kind::Frame;
    uintptr_t token{};
    DspAaFrame frame{};
    uint32_t backend = 0;
    DspAaFsrParameters fsr{};
    std::array<ComPtr<ID3D11Resource>, 5> resources;
};

struct CameraFeature {
    std::unique_ptr<dspaa::DlssFeature> feature;
    dspaa::PresetEvidence evidence;
    dspaa::OutputBackup backup;
};

struct OptimalRequest {
    uintptr_t token{};
    DspAaOptimalSettings settings{};
    uint32_t backend = 0, jitterPhases = 0;
};

struct Service {
    std::mutex queueMutex;
    std::mutex renderMutex;
    std::mutex logMutex;
    std::unordered_map<uintptr_t, std::unique_ptr<Command>> pending;
    std::unordered_map<uint64_t, DspAaStatus> statuses;
    std::unordered_map<uint64_t, OptimalRequest> optimalRequests;
    std::unordered_map<uintptr_t, DspAaSupport> supportRequests;
    uintptr_t nextToken = 1;
    bool ready = false;
    bool closing = false;
    fs::path runtimeDirectory;
    fs::path dataDirectory;
    std::ofstream log;
    dspaa::PresetEvidence evidence;
    std::unique_ptr<dspaa::NgxDevice> device;
    std::unordered_map<uint64_t, CameraFeature> features;
    std::shared_ptr<dspaa::Dx11Dx12> graphicsBridge;
    std::unique_ptr<dspaa::FsrDevice> fsrDevice;
    std::unordered_map<uint64_t, std::unique_ptr<dspaa::FsrFeature>> fsrFeatures;
};

Service& service() {
    // Unity retains the module for process lifetime. Never invoke graphics cleanup
    // under the DLL loader lock; explicit render-thread Shutdown owns that cleanup.
    static Service* instance = new Service;
    return *instance;
}

void NVSDK_CONV ngxLog(const char* text, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) noexcept {
    try {
        auto& state = service();
        std::lock_guard lock(state.logMutex);
        state.evidence.observe(text ? text : "");
        if (state.log)
            state.log << (text ? text : "") << '\n';
    } catch (...) {
        // No C++ exceptions may cross a vendor callback boundary.
    }
}

void statusFor(uint64_t camera, uint64_t frame, int result, uint32_t preset, const char* message,
               const dspaa::PresetEvidence* cameraEvidence = nullptr) {
    auto& state = service();
    DspAaStatus status{};
    status.size = sizeof(status);
    status.result = result;
    status.frame = frame;
    status.requestedPreset = preset;
    {
        std::lock_guard lock(state.logMutex);
        const auto& evidence = cameraEvidence ? *cameraEvidence : state.evidence;
        status.observedPreset = static_cast<unsigned char>(evidence.selected);
        const char letter = preset >= 1 && preset <= 26 ? static_cast<char>('A' + preset - 1) : '\0';
        status.verification = static_cast<uint32_t>(evidence.verify(letter));
        if (state.log && result < 0)
            state.log << "[DSPAAMod] " << message << '\n';
        state.log.flush();
    }
    if (message)
        strncpy_s(status.message, message, _TRUNCATE);
    std::lock_guard lock(state.queueMutex);
    state.statuses[camera] = status;
}

void* enqueue(std::unique_ptr<Command> command) {
    auto& state = service();
    std::lock_guard lock(state.queueMutex);
    if (!state.ready || state.closing)
        return nullptr;
    if ((command->kind == Kind::Frame || command->kind == Kind::OptimalSettings ||
         command->kind == Kind::Support) &&
        state.pending.size() >= maxPendingFrames)
        return nullptr;
    if (command->kind == Kind::Support && state.supportRequests.size() >= maxPendingFrames)
        return nullptr;
    if (command->kind == Kind::Shutdown)
        state.closing = true;
    uintptr_t token = state.nextToken++;
    while (token == 0 || state.pending.contains(token) || state.supportRequests.contains(token))
        token = state.nextToken++;
    command->token = token;
    if (command->kind == Kind::OptimalSettings) {
        DspAaOptimalSettings settings{};
        settings.size = sizeof(settings);
        settings.outputWidth = command->frame.outputWidth;
        settings.outputHeight = command->frame.outputHeight;
        settings.quality = command->frame.quality;
        state.optimalRequests[command->frame.camera] = {token, settings, command->backend, 0};
    }
    if (command->kind == Kind::Support) {
        DspAaSupport support{};
        support.size = sizeof(support);
        state.supportRequests.emplace(token, support);
    }
    state.pending.emplace(token, std::move(command));
    return reinterpret_cast<void*>(token);
}

std::string describeTexture(const D3D11_TEXTURE2D_DESC& desc) {
    std::ostringstream text;
    text << desc.Width << 'x' << desc.Height << " fmt=" << desc.Format << " samples=" << desc.SampleDesc.Count
         << " quality=" << desc.SampleDesc.Quality << " mips=" << desc.MipLevels
         << " layers=" << desc.ArraySize << " bind=" << desc.BindFlags;
    return text.str();
}

void logFrameTextures(const Command& command) {
    auto& state = service();
    std::lock_guard lock(state.logMutex);
    if (!state.log)
        return;
    state.log << "[DSPAAMod] Rejected camera=" << command.frame.camera << " frame=" << command.frame.frame
              << " input=" << command.frame.width << 'x' << command.frame.height
              << " output=" << command.frame.outputWidth << 'x' << command.frame.outputHeight << '\n';
    constexpr const char* names[] = {"color", "output", "depth", "motion", "opaque-color"};
    for (size_t i = 0; i < command.resources.size(); ++i) {
        if (i == 4 && !command.resources[i])
            continue;
        ComPtr<ID3D11Texture2D> texture;
        state.log << "[DSPAAMod] " << names[i] << ": ";
        if (SUCCEEDED(command.resources[i].As(&texture))) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            state.log << describeTexture(desc);
        } else {
            state.log << "not a Texture2D";
        }
        state.log << '\n';
    }
}

bool compatibleFormat(DXGI_FORMAT resource, DXGI_FORMAT view) {
    if (resource == view)
        return true;
    // Unity allocates typeless resources and binds floating-point views. Only the
    // exact backing family is compatible; UINT/UNORM siblings are not accepted.
    return (view == DXGI_FORMAT_R16G16B16A16_FLOAT && resource == DXGI_FORMAT_R16G16B16A16_TYPELESS) ||
           (view == DXGI_FORMAT_R32_FLOAT && resource == DXGI_FORMAT_R32_TYPELESS) ||
           (view == DXGI_FORMAT_R16G16_FLOAT && resource == DXGI_FORMAT_R16G16_TYPELESS);
}

D3D11_TEXTURE2D_DESC validateTexture(const char* role, ID3D11Resource* resource, ID3D11Device* expectedDevice,
                                     uint32_t width, uint32_t height, DXGI_FORMAT format, UINT requiredBind) {
    if (!resource)
        throw std::invalid_argument("Missing frame texture.");
    ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    if (device.Get() != expectedDevice)
        throw std::invalid_argument("Textures belong to different devices.");
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        throw std::invalid_argument("A frame resource is not a 2D texture.");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height || !compatibleFormat(desc.Format, format) ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 || desc.MipLevels != 1 ||
        desc.ArraySize != 1 || (desc.BindFlags & requiredBind) != requiredBind) {
        std::ostringstream message;
        message << role << " texture mismatch: expected " << width << 'x' << height << " fmt=" << format
                << " samples=1 mips=1 layers=1 bind&=" << requiredBind << "; got " << describeTexture(desc);
        throw std::invalid_argument(message.str());
    }
    return desc;
}

bool validDimensions(uint32_t width, uint32_t height) {
    return width && height && width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

NVSDK_NGX_PerfQuality_Value ngxQuality(uint32_t quality) {
    switch (quality) {
    case 0:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    case 1:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case 3:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case 4:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:
        throw std::invalid_argument("Unknown DLSS resolution mode.");
    }
}

void ensureDevice(ID3D11Device* device) {
    auto& state = service();
    if (state.device && state.device->device() != device) {
        state.features.clear();
        state.device.reset();
    }
    if (!state.device) {
        {
            std::lock_guard lock(state.logMutex);
            state.evidence = {};
        }
        state.device =
            std::make_unique<dspaa::NgxDevice>(device, state.runtimeDirectory, state.dataDirectory, ngxLog,
                                               NVSDK_NGX_ENGINE_TYPE_UNITY, "2022.3.62f3c1");
    }
}

void ensureFsrDevice(ID3D11Device* device) {
    auto& state = service();
    if (state.graphicsBridge && state.graphicsBridge->device11() != device) {
        state.graphicsBridge->drain();
        state.fsrFeatures.clear();
        state.fsrDevice.reset();
        state.graphicsBridge.reset();
    }
    if (!state.graphicsBridge)
        state.graphicsBridge = std::make_shared<dspaa::Dx11Dx12>(device);
    if (!state.fsrDevice) {
        auto candidate = std::make_unique<dspaa::FsrDevice>(state.graphicsBridge, state.runtimeDirectory);
        // Check actual cross-API sharing, not just vendor or feature-level claims.
        for (auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT})
            (void)state.graphicsBridge->texture(2, 2, format, format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        state.fsrDevice = std::move(candidate); // Publish only after all sharing checks succeed.
        std::lock_guard lock(state.logMutex);
        state.log << "[DSPAASR] Analytical FSR provider " << state.fsrDevice->version()
                  << " selected; ML excluded.\n";
    }
}
void releaseFsr(uint64_t camera) {
    auto& state = service();
    const auto it = state.fsrFeatures.find(camera);
    if (it == state.fsrFeatures.end())
        return;
    it->second->drain(); // A render-event acknowledgement alone is not GPU retirement.
    state.fsrFeatures.erase(it);
}

void renderSupport(Command& command) {
    auto& state = service();
    DspAaSupport support{};
    support.size = sizeof(support);
    try {
        ComPtr<ID3D11Device> device;
        command.resources[0]->GetDevice(&device);
        if (FAILED(device->GetDeviceRemovedReason()))
            throw std::runtime_error("The graphics device was removed; restart the game.");
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&desc)))
            throw std::runtime_error("Cannot identify the active graphics adapter.");
        if (command.backend == 1) {
            ensureFsrDevice(device.Get());
            strncpy_s(support.message, state.fsrDevice->version().c_str(), _TRUNCATE);
        } else {
            if (desc.VendorId != 0x10de)
                throw std::runtime_error("DLSS requires a supported NVIDIA RTX GPU.");
            // The same NGX device/capability path is used by subsequent rendering.
            ensureDevice(device.Get());
        }
        support.result = 1;
    } catch (const std::exception& error) {
        support.result = -1;
        strncpy_s(support.message, error.what(), _TRUNCATE);
    }
    std::lock_guard lock(state.queueMutex);
    const auto it = state.supportRequests.find(command.token);
    if (it != state.supportRequests.end())
        it->second = support;
}

void renderOptimalSettings(Command& command) {
    auto& state = service();
    const auto& frame = command.frame;
    DspAaOptimalSettings settings{};
    settings.size = sizeof(settings);
    settings.outputWidth = frame.outputWidth;
    settings.outputHeight = frame.outputHeight;
    settings.quality = frame.quality;
    uint32_t jitterPhases = 0;
    try {
        ComPtr<ID3D11Device> device;
        command.resources[0]->GetDevice(&device);
        if (FAILED(device->GetDeviceRemovedReason()))
            throw std::runtime_error("The D3D11 query device was removed.");
        dspaa::DlssOptimalSettings optimal;
        if (command.backend == 1) {
            ensureFsrDevice(device.Get());
            const auto fsr =
                state.fsrDevice->resolution(frame.outputWidth, frame.outputHeight, frame.quality);
            optimal = {fsr.width, fsr.height, fsr.width, fsr.height, fsr.width, fsr.height};
            jitterPhases = fsr.phases;
        } else if (frame.quality == 0) {
            optimal = {frame.outputWidth,  frame.outputHeight, frame.outputWidth,
                       frame.outputHeight, frame.outputWidth,  frame.outputHeight};
        } else {
            ensureDevice(device.Get());
            optimal = state.device->optimalSettings(frame.outputWidth, frame.outputHeight,
                                                    ngxQuality(frame.quality));
        }
        settings.optimalWidth = optimal.optimalWidth;
        settings.optimalHeight = optimal.optimalHeight;
        settings.minWidth = optimal.minWidth;
        settings.minHeight = optimal.minHeight;
        settings.maxWidth = optimal.maxWidth;
        settings.maxHeight = optimal.maxHeight;
        settings.result = 1;
    } catch (const std::exception& error) {
        settings.result = -1;
        strncpy_s(settings.message, error.what(), _TRUNCATE);
    }
    std::lock_guard lock(state.queueMutex);
    const auto it = state.optimalRequests.find(frame.camera);
    // An older in-flight query must not overwrite the newly accepted request.
    if (it != state.optimalRequests.end() && it->second.token == command.token) {
        it->second.settings = settings;
        it->second.jitterPhases = jitterPhases;
    }
}

void renderFrame(Command& command) {
    auto& state = service();
    const auto& frame = command.frame;
    ID3D11Resource* color = command.resources[0].Get();
    ID3D11Resource* output = command.resources[1].Get();
    ComPtr<ID3D11Device> device;
    color->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    bool canCopyFallback = false;
    dspaa::OutputBackup* capturedBackup = nullptr;
    try {
        if (FAILED(device->GetDeviceRemovedReason()))
            throw std::runtime_error("The D3D11 device was removed; reapply settings or restart the game.");
        validateTexture("color", color, device.Get(), frame.width, frame.height,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        validateTexture("output", output, device.Get(), frame.outputWidth, frame.outputHeight,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
        canCopyFallback = frame.width == frame.outputWidth && frame.height == frame.outputHeight;
        validateTexture("depth", command.resources[2].Get(), device.Get(), frame.width, frame.height,
                        DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        validateTexture("motion", command.resources[3].Get(), device.Get(), frame.width, frame.height,
                        DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        if (!std::isfinite(frame.jitterX) || !std::isfinite(frame.jitterY) ||
            !std::isfinite(frame.motionScaleX) || !std::isfinite(frame.motionScaleY) ||
            !std::isfinite(frame.frameTimeMilliseconds) || frame.frameTimeMilliseconds < 0.0f)
            throw std::invalid_argument("Invalid temporal frame parameters.");
        if (command.backend == 1) {
            if (command.resources[4])
                validateTexture("opaque-color", command.resources[4].Get(), device.Get(), frame.width,
                                frame.height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
            ensureFsrDevice(device.Get());
            state.features.erase(frame.camera);
            auto& feature = state.fsrFeatures[frame.camera];
            if (!feature)
                feature = std::make_unique<dspaa::FsrFeature>(*state.fsrDevice);
            feature->configure({frame.width, frame.height, frame.outputWidth, frame.outputHeight,
                                (frame.flags & 1) != 0, (frame.flags & 2) != 0,
                                command.resources[4] != nullptr});
            dspaa::FsrFrame input;
            input.color = color;
            input.output = output;
            input.opaqueColor = command.resources[4].Get();
            input.depth = command.resources[2].Get();
            input.motion = command.resources[3].Get();
            input.jitterX = frame.jitterX;
            input.jitterY = frame.jitterY;
            input.motionScaleX = frame.motionScaleX;
            input.motionScaleY = frame.motionScaleY;
            input.milliseconds = frame.frameTimeMilliseconds;
            input.reset = (frame.flags & 4) != 0;
            input.cameraNear = command.fsr.cameraNear;
            input.cameraFar = command.fsr.cameraFar;
            input.verticalFov = command.fsr.verticalFov;
            input.preExposure = command.fsr.preExposure;
            input.viewSpaceToMeters = command.fsr.viewSpaceToMeters;
            input.sharpness = command.fsr.sharpness;
            feature->evaluate(input);
            const dspaa::PresetEvidence noNgxPreset;
            const auto message =
                "Analytical FSR " + state.fsrDevice->version() + " executed through the shared D3D12 bridge.";
            statusFor(frame.camera, frame.frame, 1, 0, message.c_str(), &noNgxPreset);
            return;
        }
        releaseFsr(frame.camera);
        ensureDevice(device.Get());
        auto& cameraFeature = state.features[frame.camera];
        auto& feature = cameraFeature.feature;
        if (!feature)
            feature = std::make_unique<dspaa::DlssFeature>(*state.device);
        dspaa::DlssConfiguration config;
        config.inputWidth = frame.width;
        config.inputHeight = frame.height;
        config.outputWidth = frame.outputWidth;
        config.outputHeight = frame.outputHeight;
        config.quality = ngxQuality(frame.quality);
        config.preset = static_cast<NVSDK_NGX_DLSS_Hint_Render_Preset>(frame.preset);
        config.hdr = (frame.flags & 1) != 0;
        config.invertedDepth = (frame.flags & 2) != 0;
        config.autoExposure = true;
        if (!(feature->configuration() == config)) {
            std::lock_guard lock(state.logMutex);
            state.evidence.selected = '\0';
            state.evidence.origin = dspaa::PresetOrigin::Unknown;
        }
        const bool changed = feature->configure(config);
        if (changed) {
            std::lock_guard lock(state.logMutex);
            cameraFeature.evidence = state.evidence;
            const char letter =
                frame.preset >= 1 && frame.preset <= 26 ? static_cast<char>('A' + frame.preset - 1) : '\0';
            const auto verdict = cameraFeature.evidence.verify(letter);
            if (verdict != dspaa::PresetVerdict::Matched) {
                throw std::runtime_error("Requested preset is not verified as application-controlled; "
                                         "inspect ngx.log and driver overrides.");
            }
        }
        dspaa::DlssFrame input;
        input.color = color;
        input.output = output;
        input.depth = command.resources[2].Get();
        input.motionVectors = command.resources[3].Get();
        input.jitterX = frame.jitterX;
        input.jitterY = frame.jitterY;
        input.motionScaleX = frame.motionScaleX;
        input.motionScaleY = frame.motionScaleY;
        input.frameTimeMilliseconds = frame.frameTimeMilliseconds;
        input.reset = (frame.flags & 4) != 0;
        if (!canCopyFallback) {
            cameraFeature.backup.capture(context.Get(), output);
            capturedBackup = &cameraFeature.backup;
        }
        feature->evaluate(input);
        statusFor(frame.camera, frame.frame, 1, frame.preset,
                  "DLSS executed; input-buffer correctness still requires game validation.",
                  &cameraFeature.evidence);
    } catch (const std::exception& error) {
        // Never copy low-resolution color to an unequal-size output. Preserve the
        // caller's spatial prefill even when NGX partially writes before failing.
        if (capturedBackup)
            capturedBackup->restore(context.Get(), output);
        else if (canCopyFallback)
            context->CopyResource(output, color);
        state.features.erase(frame.camera);
        state.fsrFeatures.erase(frame.camera); // The FSR destructor drains or quarantines its GPU resources.
        logFrameTextures(command);
        statusFor(frame.camera, frame.frame, -1, frame.preset, error.what());
    }
}

void __stdcall renderEvent(int eventId, void* opaque) noexcept {
    if (eventId != 1 || !opaque)
        return;
    try {
        auto& state = service();
        std::unique_ptr<Command> command;
        {
            std::lock_guard lock(state.queueMutex);
            const auto it = state.pending.find(reinterpret_cast<uintptr_t>(opaque));
            if (it == state.pending.end())
                return; // Cancelled/duplicate events are harmless.
            command = std::move(it->second);
            state.pending.erase(it);
        }
        std::lock_guard renderLock(state.renderMutex);
        {
            std::lock_guard lock(state.queueMutex);
            if (!state.ready)
                return;
        }
        if (command->kind == Kind::Frame) {
            renderFrame(*command);
        } else if (command->kind == Kind::OptimalSettings) {
            renderOptimalSettings(*command);
        } else if (command->kind == Kind::Support) {
            renderSupport(*command);
        } else if (command->kind == Kind::Release) {
            releaseFsr(command->frame.camera);
            state.features.erase(command->frame.camera);
            {
                std::lock_guard lock(state.queueMutex);
                state.optimalRequests.erase(command->frame.camera);
            }
            statusFor(command->frame.camera, 0, 2, 0, "Camera released; queued texture users have drained.");
        } else {
            if (state.graphicsBridge)
                state.graphicsBridge->drain();
            state.fsrFeatures.clear();
            state.fsrDevice.reset();
            state.graphicsBridge.reset();
            state.features.clear();
            state.device.reset();
            {
                std::lock_guard lock(state.queueMutex);
                state.pending.clear();
                state.statuses.clear();
                state.optimalRequests.clear();
                state.supportRequests.clear();
                state.ready = false;
                state.closing = false;
            }
            std::lock_guard lock(state.logMutex);
            state.log.close();
        }
    } catch (...) {
        // Keep exceptions within the native plugin, including during shutdown.
    }
}
} // namespace

uint32_t __cdecl DspAaGetAbiVersion() {
    return abiVersion;
}
DspAaRenderEvent __cdecl DspAaGetRenderEvent() {
    return renderEvent;
}

int __cdecl DspAaInitialize(const wchar_t* runtimeDirectory, const wchar_t* dataDirectory) {
    try {
        if (!runtimeDirectory || !dataDirectory)
            return 0;
        auto& state = service();
        std::lock_guard renderLock(state.renderMutex);
        std::lock_guard queueLock(state.queueMutex);
        if (state.ready || state.closing || !state.pending.empty())
            return 0;
        state.runtimeDirectory = fs::absolute(runtimeDirectory);
        state.dataDirectory = fs::absolute(dataDirectory);
        if (!fs::is_directory(state.runtimeDirectory))
            return 0;
        fs::create_directories(state.dataDirectory);
        std::lock_guard logLock(state.logMutex);
        state.log.open(state.dataDirectory / "ngx.log", std::ios::app);
        if (!state.log)
            return 0;
        state.evidence = {};
        state.ready = true;
        return 1;
    } catch (...) {
        return 0;
    }
}

namespace {
void* queueFrame(const DspAaFrame* frame, const DspAaFsrParameters* fsr) {
    try {
        if (!frame || frame->size != sizeof(DspAaFrame) || frame->version != abiVersion || !frame->camera ||
            !frame->color || !frame->output || !frame->depth || !frame->motion ||
            !validDimensions(frame->width, frame->height) ||
            !validDimensions(frame->outputWidth, frame->outputHeight) || frame->quality > 4 ||
            frame->width > frame->outputWidth || frame->height > frame->outputHeight ||
            (frame->quality == 0 &&
             (frame->width != frame->outputWidth || frame->height != frame->outputHeight)) ||
            frame->color == frame->output || frame->reserved != 0 || frame->reserved2 != 0 ||
            (frame->flags & ~7u) != 0)
            return nullptr;
        auto command = std::make_unique<Command>();
        command->frame = *frame;
        if (fsr) {
            command->backend = 1;
            command->fsr = *fsr;
            command->resources[4] = static_cast<ID3D11Resource*>(fsr->opaqueColor);
        }
        command->resources[0] = static_cast<ID3D11Resource*>(frame->color);
        command->resources[1] = static_cast<ID3D11Resource*>(frame->output);
        command->resources[2] = static_cast<ID3D11Resource*>(frame->depth);
        command->resources[3] = static_cast<ID3D11Resource*>(frame->motion);
        return enqueue(std::move(command));
    } catch (...) {
        return nullptr;
    }
}
} // namespace
void* __cdecl DspAaQueueFrame(const DspAaFrame* frame) {
    return queueFrame(frame, nullptr);
}
void* __cdecl DspAaQueueFsrFrame(const DspAaFrame* frame, const DspAaFsrParameters* parameters) {
    if (!parameters || parameters->size != sizeof(DspAaFsrParameters) || parameters->reserved)
        return nullptr;
    return queueFrame(frame, parameters);
}

void* __cdecl DspAaQueueOptimalSettings(uint64_t camera, void* deviceResource, uint32_t outputWidth,
                                        uint32_t outputHeight, uint32_t quality) {
    return DspAaQueueOptimalSettingsForBackend(camera, deviceResource, outputWidth, outputHeight, quality, 0);
}
void* __cdecl DspAaQueueOptimalSettingsForBackend(uint64_t camera, void* deviceResource, uint32_t outputWidth,
                                                  uint32_t outputHeight, uint32_t quality, uint32_t backend) {
    try {
        if (!camera || !deviceResource || !validDimensions(outputWidth, outputHeight) || quality > 4 ||
            backend > 1)
            return nullptr;
        auto command = std::make_unique<Command>();
        command->kind = Kind::OptimalSettings;
        command->backend = backend;
        command->frame.camera = camera;
        command->frame.outputWidth = outputWidth;
        command->frame.outputHeight = outputHeight;
        command->frame.quality = quality;
        command->resources[0] = static_cast<ID3D11Resource*>(deviceResource);
        return enqueue(std::move(command));
    } catch (...) {
        return nullptr;
    }
}

int __cdecl DspAaGetOptimalSettings(uint64_t camera, DspAaOptimalSettings* output) {
    try {
        if (!output || output->size != sizeof(DspAaOptimalSettings))
            return 0;
        auto& state = service();
        std::lock_guard lock(state.queueMutex);
        const auto it = state.optimalRequests.find(camera);
        if (it == state.optimalRequests.end())
            return 0;
        *output = it->second.settings;
        return 1;
    } catch (...) {
        return 0;
    }
}

int __cdecl DspAaGetFsrOptimalSettings(uint64_t camera, DspAaFsrOptimalSettings* output) {
    try {
        if (!output || output->settings.size != sizeof(DspAaFsrOptimalSettings))
            return 0;
        auto& state = service();
        std::lock_guard lock(state.queueMutex);
        const auto it = state.optimalRequests.find(camera);
        if (it == state.optimalRequests.end() || it->second.backend != 1)
            return 0;
        output->settings = it->second.settings;
        output->settings.size = sizeof(DspAaFsrOptimalSettings);
        output->jitterPhases = it->second.jitterPhases;
        return 1;
    } catch (...) {
        return 0;
    }
}

void* __cdecl DspAaQueueSupport(void* deviceResource) {
    return DspAaQueueSupportForBackend(deviceResource, 0);
}
void* __cdecl DspAaQueueSupportForBackend(void* deviceResource, uint32_t backend) {
    try {
        if (!deviceResource || backend > 1)
            return nullptr;
        auto command = std::make_unique<Command>();
        command->kind = Kind::Support;
        command->backend = backend;
        command->resources[0] = static_cast<ID3D11Resource*>(deviceResource);
        return enqueue(std::move(command));
    } catch (...) {
        return nullptr;
    }
}

int __cdecl DspAaGetSupport(void* token, DspAaSupport* output) {
    try {
        if (!token || !output || output->size != sizeof(DspAaSupport))
            return 0;
        auto& state = service();
        std::lock_guard lock(state.queueMutex);
        const auto it = state.supportRequests.find(reinterpret_cast<uintptr_t>(token));
        if (it == state.supportRequests.end())
            return 0;
        *output = it->second;
        if (output->result != 0)
            state.supportRequests.erase(it);
        return 1;
    } catch (...) {
        return 0;
    }
}

void* __cdecl DspAaQueueRelease(uint64_t camera) {
    try {
        auto command = std::make_unique<Command>();
        command->kind = Kind::Release;
        command->frame.camera = camera;
        return enqueue(std::move(command));
    } catch (...) {
        return nullptr;
    }
}
void* __cdecl DspAaQueueShutdown() {
    try {
        auto command = std::make_unique<Command>();
        command->kind = Kind::Shutdown;
        return enqueue(std::move(command));
    } catch (...) {
        return nullptr;
    }
}
void __cdecl DspAaCancel(void* token) {
    try {
        auto& state = service();
        std::lock_guard lock(state.queueMutex);
        const auto it = state.pending.find(reinterpret_cast<uintptr_t>(token));
        if (it != state.pending.end()) {
            if (it->second->kind == Kind::Shutdown)
                state.closing = false;
            if (it->second->kind == Kind::OptimalSettings) {
                const auto request = state.optimalRequests.find(it->second->frame.camera);
                if (request != state.optimalRequests.end() && request->second.token == it->first)
                    state.optimalRequests.erase(request);
            }
            state.supportRequests.erase(it->first);
            state.pending.erase(it);
        }
    } catch (...) {
    }
}
int __cdecl DspAaGetStatus(uint64_t camera, DspAaStatus* output) {
    try {
        if (!output || output->size != sizeof(DspAaStatus))
            return 0;
        auto& state = service();
        std::lock_guard lock(state.queueMutex);
        const auto it = state.statuses.find(camera);
        if (it == state.statuses.end())
            return 0;
        *output = it->second;
        if (output->result == 2)
            state.statuses.erase(it); // Acknowledge retirement exactly once.
        return 1;
    } catch (...) {
        return 0;
    }
}
