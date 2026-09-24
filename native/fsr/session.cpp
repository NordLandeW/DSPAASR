#include "session.h"
#include <api/include/dx12/ffx_api_dx12.h>
#include <api/include/ffx_api_loader.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <upscalers/include/ffx_upscale.h>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace dspaa {
namespace {
void fsrCheck(ffxReturnCode_t code, const char* operation) {
    if (code != FFX_API_RETURN_OK)
        throw std::runtime_error(std::string(operation) + " failed (FSR API " + std::to_string(code) + ")");
}
struct Module {
    HMODULE value = nullptr;
    ~Module() {
        if (value)
            FreeLibrary(value);
    }
    void load(const std::filesystem::path& path) {
        value = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!value)
            graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Load official FSR runtime");
    }
};
} // namespace
struct FsrDevice::Impl {
    std::shared_ptr<Dx11Dx12> bridge;
    Module upscaler, loader;
    ffxFunctions api{};
    uint64_t versionId = 0;
    std::string version;
    ffxOverrideVersion overrideVersion() const {
        ffxOverrideVersion value{};
        value.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        value.versionId = versionId;
        return value;
    }
};
FsrDevice::FsrDevice(std::shared_ptr<Dx11Dx12> bridge, const std::filesystem::path& runtime)
    : impl_(std::make_shared<Impl>()) {
    if (!bridge)
        throw std::invalid_argument("Missing FSR graphics bridge");
    impl_->bridge = std::move(bridge);
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_2};
    graphicsCheck(impl_->bridge->device12()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel,
                                                                 sizeof(shaderModel)),
                  "FSR shader model query");
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_2)
        throw std::runtime_error("FSR 3.1 requires shader model 6.2 on the active adapter");
    // Loading the effect from the same absolute directory prevents an unrelated
    // process working directory from selecting a different upscaler binary.
    impl_->upscaler.load(std::filesystem::absolute(runtime / "amd_fidelityfx_upscaler_dx12.dll"));
    impl_->loader.load(std::filesystem::absolute(runtime / "amd_fidelityfx_loader_dx12.dll"));
    ffxLoadFunctions(&impl_->api, impl_->loader.value);
    if (!impl_->api.CreateContext || !impl_->api.DestroyContext || !impl_->api.Query ||
        !impl_->api.Configure || !impl_->api.Dispatch)
        throw std::runtime_error("Incomplete FSR loader API");
    uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = impl_->bridge->device12();
    query.outputCount = &count;
    fsrCheck(impl_->api.Query(nullptr, &query.header), "Enumerate FSR providers");
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    query.versionIds = ids.data();
    query.versionNames = names.data();
    const auto capacity = count;
    fsrCheck(impl_->api.Query(nullptr, &query.header), "Read FSR providers");
    if (count > capacity)
        throw std::runtime_error("FSR provider list changed during enumeration");
    unsigned bestPatch = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned major = 0, minor = 0, patch = 0;
        char tail = 0;
        if (names[i] && sscanf_s(names[i], "%u.%u.%u%c", &major, &minor, &patch, &tail, 1u) == 3 &&
            major == 3 && minor == 1 && patch >= 5 && (!impl_->versionId || patch > bestPatch)) {
            impl_->versionId = ids[i];
            impl_->version = names[i];
            bestPatch = patch;
        }
    }
    if (!impl_->versionId)
        throw std::runtime_error(
            "Official analytical FSR 3.1.5 or newer 3.1 provider is unavailable; ML is not substituted");
}
FsrDevice::~FsrDevice() = default;
const std::string& FsrDevice::version() const {
    return impl_->version;
}
std::shared_ptr<Dx11Dx12> FsrDevice::bridge() const {
    return impl_->bridge;
}
FsrResolution FsrDevice::resolution(unsigned width, unsigned height, unsigned quality) {
    if (!width || !height || quality > 4)
        throw std::invalid_argument("Invalid FSR resolution request");
    auto version = impl_->overrideVersion();
    FsrResolution result;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode size{};
    size.header = {FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE, &version.header};
    size.displayWidth = width;
    size.displayHeight = height;
    size.qualityMode = quality;
    size.pOutRenderWidth = &result.width;
    size.pOutRenderHeight = &result.height;
    fsrCheck(impl_->api.Query(nullptr, &size.header), "Query FSR render dimensions");
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount jitter{};
    jitter.header = {FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT, &version.header};
    jitter.renderWidth = result.width;
    jitter.displayWidth = width;
    jitter.pOutPhaseCount = &phases;
    fsrCheck(impl_->api.Query(nullptr, &jitter.header), "Query FSR jitter phases");
    if (!result.width || !result.height || result.width > width || result.height > height || phases <= 0)
        throw std::runtime_error("Invalid dimensions or jitter phase count returned by FSR");
    result.phases = static_cast<unsigned>(phases);
    return result;
}

struct FsrFeature::Impl {
    std::shared_ptr<FsrDevice::Impl> device;
    ffxContext context = nullptr;
    FsrConfiguration configuration{};
    ffxCreateBackendDX12Desc backend{};
    ffxCreateContextDescUpscaleVersion apiVersion{};
    ffxOverrideVersion algorithm{};
    ffxCreateContextDescUpscale description{};
    SharedTexture color, depth, motion, output, opaque;
    ComPtr<ID3D12Resource> reactive;
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t completion = 0;
    };
    std::array<Slot, 3> slots;
    size_t nextSlot = 0;
    bool reset = true;
    void destroy() {
        if (context)
            fsrCheck(device->api.DestroyContext(&context, nullptr), "Destroy FSR context");
        context = nullptr;
    }
};
FsrFeature::FsrFeature(FsrDevice& device) : impl_(std::make_unique<Impl>()) {
    impl_->device = device.impl_;
}
FsrFeature::~FsrFeature() {
    try {
        drain();
        impl_->destroy();
    } catch (...) {
        // Faulted GPU work may still own these allocations. Keep the SDK modules,
        // both devices and resources alive until process teardown instead of UAF.
        (void)impl_.release();
    }
}
void FsrFeature::drain() {
    if (impl_ && impl_->context)
        impl_->device->bridge->drain();
}
bool FsrFeature::configure(const FsrConfiguration& config) {
    auto& s = *impl_;
    if (s.context && s.configuration == config)
        return false;
    if (!config.inputWidth || !config.inputHeight || !config.outputWidth || !config.outputHeight ||
        config.inputWidth > config.outputWidth || config.inputHeight > config.outputHeight)
        throw std::invalid_argument("Invalid FSR frame dimensions");
    drain();
    s.destroy();
    auto& bridge = *s.device->bridge;
    s.color = bridge.texture(config.inputWidth, config.inputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    s.depth = bridge.texture(config.inputWidth, config.inputHeight, DXGI_FORMAT_R32_FLOAT, false);
    s.motion = bridge.texture(config.inputWidth, config.inputHeight, DXGI_FORMAT_R16G16_FLOAT, false);
    s.output = bridge.texture(config.outputWidth, config.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    s.opaque = {};
    s.reactive.Reset();
    if (config.reactiveMask) {
        s.opaque =
            bridge.texture(config.inputWidth, config.inputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC mask{};
        mask.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        mask.Width = config.inputWidth;
        mask.Height = config.inputHeight;
        mask.DepthOrArraySize = mask.MipLevels = mask.SampleDesc.Count = 1;
        mask.Format = DXGI_FORMAT_R8_UNORM;
        mask.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        graphicsCheck(bridge.device12()->CreateCommittedResource(
                          &heap, D3D12_HEAP_FLAG_NONE, &mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          nullptr, IID_PPV_ARGS(&s.reactive)),
                      "Create private FSR reactive mask");
    }
    s.algorithm = s.device->overrideVersion();
    s.backend.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, &s.algorithm.header};
    s.backend.device = bridge.device12();
    s.apiVersion.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &s.backend.header};
    s.apiVersion.version = FFX_UPSCALER_VERSION;
    s.description.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &s.apiVersion.header};
    s.description.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (config.hdr)
        s.description.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (config.invertedDepth)
        s.description.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    s.description.maxRenderSize = {config.inputWidth, config.inputHeight};
    s.description.maxUpscaleSize = {config.outputWidth, config.outputHeight};
    fsrCheck(s.device->api.CreateContext(&s.context, &s.description.header, nullptr),
             "Create analytical FSR context");
    ffxQueryGetProviderVersion actual{};
    actual.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    fsrCheck(s.device->api.Query(&s.context, &actual.header), "Verify selected FSR provider");
    if (actual.versionId != s.device->versionId)
        throw std::runtime_error("FSR ignored the analytical provider selection");
    for (auto& slot : s.slots) {
        if (slot.allocator)
            continue;
        graphicsCheck(bridge.device12()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                IID_PPV_ARGS(&slot.allocator)),
                      "Create FSR command allocator");
        graphicsCheck(bridge.device12()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           slot.allocator.Get(), nullptr,
                                                           IID_PPV_ARGS(&slot.list)),
                      "Create FSR command list");
        graphicsCheck(slot.list->Close(), "Close initial FSR command list");
    }
    s.configuration = config;
    s.reset = true;
    return true;
}
void FsrFeature::evaluate(const FsrFrame& frame) {
    auto& s = *impl_;
    if (!s.context)
        throw std::runtime_error("FSR context is not configured");
    if (!frame.color || !frame.depth || !frame.motion || !frame.output ||
        (s.configuration.reactiveMask && !frame.opaqueColor) || !std::isfinite(frame.cameraNear) ||
        !std::isfinite(frame.cameraFar) || frame.cameraNear <= 0 || frame.cameraFar <= frame.cameraNear ||
        !std::isfinite(frame.verticalFov) || frame.verticalFov <= 0 || frame.verticalFov >= 3.14159265f ||
        !std::isfinite(frame.preExposure) || frame.preExposure <= 0 ||
        !std::isfinite(frame.viewSpaceToMeters) || frame.viewSpaceToMeters <= 0 ||
        !std::isfinite(frame.sharpness) || frame.sharpness < 0 || frame.sharpness > 1)
        throw std::invalid_argument("Invalid FSR camera/exposure/sharpening parameters");
    auto& bridge = *s.device->bridge;
    auto& slot = s.slots[s.nextSlot++ % s.slots.size()];
    bridge.wait12(slot.completion); // Allocator reuse, not a full CPU wait each frame.
    graphicsCheck(slot.allocator->Reset(), "Reset FSR allocator");
    graphicsCheck(slot.list->Reset(slot.allocator.Get(), nullptr), "Reset FSR command list");
    bridge.context11()->CopyResource(s.color.dx11.Get(), frame.color);
    bridge.context11()->CopyResource(s.depth.dx11.Get(), frame.depth);
    bridge.context11()->CopyResource(s.motion.dx11.Get(), frame.motion);
    if (s.configuration.reactiveMask)
        bridge.context11()->CopyResource(s.opaque.dx11.Get(), frame.opaqueColor);
    bridge.handoffTo12();
    const std::array<ID3D12Resource*, 3> inputs{s.color.dx12.Get(), s.depth.dx12.Get(), s.motion.dx12.Get()};
    for (auto* input : inputs)
        Dx11Dx12::transition(slot.list.Get(), input, D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Dx11Dx12::transition(slot.list.Get(), s.output.dx12.Get(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (s.configuration.reactiveMask) {
        Dx11Dx12::transition(slot.list.Get(), s.opaque.dx12.Get(), D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ffxDispatchDescUpscaleGenerateReactiveMask generate{};
        generate.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
        generate.commandList = slot.list.Get();
        generate.colorOpaqueOnly = ffxApiGetResourceDX12(s.opaque.dx12.Get());
        generate.colorPreUpscale = ffxApiGetResourceDX12(s.color.dx12.Get());
        generate.outReactive = ffxApiGetResourceDX12(s.reactive.Get());
        generate.renderSize = {s.configuration.inputWidth, s.configuration.inputHeight};
        // Official FSR SDK 2.3 sample parameters: binary reactivity capped at 0.9.
        generate.scale = 1.f;
        generate.cutoffThreshold = 0.2f;
        generate.binaryValue = 0.9f;
        generate.flags = FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_TONEMAP |
                         FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD |
                         FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
        const auto generated = s.device->api.Dispatch(&s.context, &generate.header);
        if (generated != FFX_API_RETURN_OK) {
            (void)slot.list->Close();
            fsrCheck(generated, "Generate FSR reactive mask");
        }
        Dx11Dx12::transition(slot.list.Get(), s.opaque.dx12.Get(),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = slot.list.Get();
    dispatch.color = ffxApiGetResourceDX12(s.color.dx12.Get());
    dispatch.depth = ffxApiGetResourceDX12(s.depth.dx12.Get());
    dispatch.motionVectors = ffxApiGetResourceDX12(s.motion.dx12.Get());
    dispatch.output = ffxApiGetResourceDX12(s.output.dx12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    if (s.configuration.reactiveMask)
        dispatch.reactive = ffxApiGetResourceDX12(s.reactive.Get());
    dispatch.jitterOffset = {frame.jitterX, frame.jitterY};
    dispatch.motionVectorScale = {frame.motionScaleX, frame.motionScaleY};
    dispatch.renderSize = {s.configuration.inputWidth, s.configuration.inputHeight};
    dispatch.upscaleSize = {s.configuration.outputWidth, s.configuration.outputHeight};
    dispatch.enableSharpening = frame.sharpness > 0;
    dispatch.sharpness = frame.sharpness;
    dispatch.frameTimeDelta = frame.milliseconds;
    dispatch.preExposure = frame.preExposure;
    dispatch.reset = frame.reset || s.reset;
    dispatch.cameraNear = s.configuration.invertedDepth ? frame.cameraFar : frame.cameraNear;
    dispatch.cameraFar = s.configuration.invertedDepth ? frame.cameraNear : frame.cameraFar;
    dispatch.cameraFovAngleVertical = frame.verticalFov;
    dispatch.viewSpaceToMetersFactor = frame.viewSpaceToMeters;
    const auto result = s.device->api.Dispatch(&s.context, &dispatch.header);
    if (result != FFX_API_RETURN_OK) {
        (void)slot.list->Close(); // Nothing has been executed on 12; no partial output is copied back.
        fsrCheck(result, "Dispatch FSR upscaling");
    }
    for (auto* input : inputs)
        Dx11Dx12::transition(slot.list.Get(), input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_COMMON);
    Dx11Dx12::transition(slot.list.Get(), s.output.dx12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COMMON);
    graphicsCheck(slot.list->Close(), "Close FSR commands");
    ID3D12CommandList* lists[] = {slot.list.Get()};
    bridge.queue12()->ExecuteCommandLists(1, lists);
    slot.completion = bridge.handoffTo11();
    bridge.context11()->CopyResource(frame.output, s.output.dx11.Get());
    s.reset = false;
}
} // namespace dspaa
