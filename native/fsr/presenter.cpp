#include "presenter.h"
#include "graphics/dx11-dx12.h"
#include <api/include/dx12/ffx_api_dx12.h>
#include <api/include/ffx_api_loader.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace dspaa {
namespace {
constexpr DWORD gpuTimeout = 30000;
constexpr size_t slotCount = 3;
constexpr D3D12_RESOURCE_STATES pixelRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES computeRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void checkFfx(ffxReturnCode_t result, const char* operation) {
    if (result != FFX_API_RETURN_OK)
        throw std::runtime_error(std::string(operation) + " (FSR API " + std::to_string(result) + ")");
}
struct Module {
    HMODULE handle = nullptr;
    ~Module() { if (handle) FreeLibrary(handle); }
    void load(const std::filesystem::path& path) {
        handle = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle) graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Load FSR FG runtime");
    }
};
struct Event {
    HANDLE handle = nullptr;
    ~Event() { if (handle) CloseHandle(handle); }
};
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before != after) Dx11Dx12::transition(list, resource, before, after);
}
DXGI_FORMAT readableFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    default: return format;
    }
}
bool colorFormat(DXGI_FORMAT format) {
    switch (readableFormat(format)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return true;
    default: return false;
    }
}
D3D12_RESOURCE_STATES nativeState(uint32_t state) {
    if (state == FFX_API_RESOURCE_STATE_COMMON || state == FFX_API_RESOURCE_STATE_PRESENT)
        return D3D12_RESOURCE_STATE_COMMON;
    switch (state) {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return computeRead;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return pixelRead;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return pixelRead | computeRead;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_COPY_SOURCE | computeRead;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    default: throw std::runtime_error("Unexpected FSR callback resource state");
    }
}
uint32_t transferFunction(DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    default: throw std::invalid_argument("Unsupported FSR presentation color space");
    }
}
FfxApiRect2D generationRect(const RECT& rect, unsigned width, unsigned height) {
    if (!rect.left && !rect.top && !rect.right && !rect.bottom)
        return {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
        static_cast<unsigned>(rect.right) > width || static_cast<unsigned>(rect.bottom) > height)
        throw std::invalid_argument("Invalid frame-generation rectangle");
    FfxApiRect2D result{};
    result.left = rect.left; result.top = rect.top;
    result.width = rect.right - rect.left; result.height = rect.bottom - rect.top;
    return result;
}

// Original shaders: all operands are postprocessed values in the declared
// presentation encoding. Floating point storage must not clamp the signed RGB
// residual. Alpha is independently rendered coverage, never inferred from RGB.
constexpr char shaders[] = R"hlsl(
Texture2D<float4> firstImage : register(t0);
Texture2D<float4> secondImage : register(t1);
Texture2D<float4> thirdImage : register(t2);
float4 fullscreen(uint id : SV_VertexID) : SV_Position {
    return float4((id & 1) * 4.0 - 1.0, (id & 2) * -2.0 + 1.0, 0, 1);
}
float4 copyColor(float4 position : SV_Position) : SV_Target {
    return firstImage.Load(int3(int2(position.xy), 0));
}
float4 makeResidual(float4 position : SV_Position) : SV_Target {
    int3 p = int3(int2(position.xy), 0);
    float a = saturate(thirdImage.Load(p).r);
    return float4(firstImage.Load(p).rgb - (1-a)*secondImage.Load(p).rgb, a);
}
float4 compose(float4 position : SV_Position) : SV_Target {
    int3 p = int3(int2(position.xy), 0);
    float4 residual = secondImage.Load(p);
    return float4(residual.rgb + (1-residual.a)*firstImage.Load(p).rgb, 1);
}
)hlsl";
ComPtr<ID3DBlob> compileShader(const char* entry, const char* target) {
    ComPtr<ID3DBlob> shader, errors;
    const auto result = D3DCompile(shaders, sizeof(shaders)-1, "DSPAASR FSR presentation", nullptr,
                                   nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                   &shader, &errors);
    if (FAILED(result))
        throw std::runtime_error(errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                    errors->GetBufferSize()) : "Compile presentation shader");
    return shader;
}
} // namespace

struct FsrPresenter::Impl {
    FsrPresenterCreateInfo creation;
    ComPtr<IDXGIFactory> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> chain;
    ComPtr<ID3D12Fence> fence;
    Event fenceEvent;
    uint64_t nextFence = 0, lastSubmitted = 0, nextFrame = 0, lastApplicationFrame = 0;
    uint64_t algorithmId = 0, chainId = 0;
    Module effectModule, loaderModule, callbackModule;
    ffxFunctions api{};
    ffxContext chainContext = nullptr, fgContext = nullptr;
    ffxOverrideVersion chainSelection{};
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 chainVersion{};
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 chainDescription{};
    struct Effect {
        ffxContext context = nullptr;
        ffxOverrideVersion selection{};
        ffxCreateBackendDX12Desc backend{};
        ffxCreateContextDescFrameGenerationVersion version{};
        ffxCreateContextDescFrameGeneration description{};
    };
    std::vector<std::unique_ptr<Effect>> effects;
    uint32_t contextFlags = 0;
    unsigned contextWidth = 0, contextHeight = 0;
    bool stopped = false, contextVerified = false;
    std::atomic<bool> quarantined{false};
    bool lastEnabled = false, fgFaulted = false, historyReset = true;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::atomic<int> callbackError{0};
    void dispatchFailed() noexcept {
        int healthy = 0;
        callbackError.compare_exchange_strong(healthy, 1); // Never downgrade a composition failure (2).
    }
    std::atomic<uint64_t> dispatches{0}, realCallbacks{0}, generatedCallbacks{0};
    uint64_t applicationPresents = 0, fgMemory = 0, chainMemory = 0;
    std::string reason;
    mutable std::mutex slotsMutex;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> copyPipeline, residualPipeline, composePipeline;
    UINT srvStride = 0, rtvStride = 0;

    struct Image {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    static void change(ID3D12GraphicsCommandList* commands, Image& image, D3D12_RESOURCE_STATES state) {
        barrier(commands, image.resource.Get(), image.state, state);
        image.state = state;
    }
    struct Slot {
        uint64_t id = 0, copied = 0, retired = 0;
        bool occupied = false, generating = false;
        FrameLease lease;
        PresentationFrame metadata;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        Image finalColor, hudlessRaw, hudless, alpha, residual, depth, motion, distortion;
        ComPtr<ID3D12DescriptorHeap> views, targets;
    };
    std::array<Slot, slotCount> slots;
    Slot* previous = nullptr;
    struct Snapshot {
        PresentationFrame metadata;
        bool generating = false;
        D3D12_RESOURCE_STATES finalState = pixelRead, residualState = pixelRead;
        ComPtr<ID3D12Resource> finalColor, residual;
        ComPtr<ID3D12DescriptorHeap> views, targets;
    };
    struct ControlWork {
        std::promise<void> done;
        std::future<void> completion = done.get_future();
        std::thread thread;
    };
    // Never destroy this member while its thread can still access the owner.
    // A timeout permanently quarantines the entire Impl, including this job.
    std::unique_ptr<ControlWork> control;

    explicit Impl(const FsrPresenterCreateInfo& info) : creation(info), factory(info.factory),
        device(info.device), queue(info.gameQueue) {}
    void initialize();
    uint64_t provider(uint64_t type, const char* wanted);
    void verifyProvider(ffxContext context, uint64_t expected);
    void pipelines();
    ComPtr<ID3D12PipelineState> pipeline(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT format);
    uint64_t signal();
    uint64_t completed();
    void wait(uint64_t value);
    void sdkControl(std::function<void()> operation);
    void waitSdk();
    void collect();
    void drain();
    void destroyContexts();
    void error(const char* text) noexcept { try { reason = text; } catch (...) {} }
    void quarantine(const char* text) noexcept { quarantined = true; error(text); }
    D3D12_RESOURCE_DESC validate(const PresentImage& image, unsigned width, unsigned height,
                                  const char* role) const;
    void validateTemporal(const PresentationFrame& frame) const;
    uint32_t flags(const PresentationFrame& frame) const;
    void ensureContext(const PresentationFrame& frame);
    void configure(ffxContext context, uint64_t id, bool enabled, const PresentationFrame& frame,
                   ID3D12Resource* distortion);
    Slot& acquire();
    ComPtr<ID3D12Resource> texture(unsigned width, unsigned height, DXGI_FORMAT format,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    void allocate(Slot& slot, const PresentationFrame& frame, bool generating);
    void view(ID3D12DescriptorHeap* heap, unsigned index, ID3D12Resource* resource);
    D3D12_CPU_DESCRIPTOR_HANDLE target(ID3D12DescriptorHeap* heap, unsigned index,
                                       ID3D12Resource* resource);
    void draw(ID3D12GraphicsCommandList* commands, ID3D12PipelineState* pipeline,
              ID3D12DescriptorHeap* views, unsigned first,
              D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void copyInput(ID3D12GraphicsCommandList* commands, const PresentImage& source,
                   Image& destination, D3D12_RESOURCE_STATES after);
    void upload(Slot& slot, bool generating, ID3D12Resource* backbuffer);
    void prepare(Slot& slot);
    bool snapshot(uint64_t id, Snapshot& output);
    static ffxReturnCode_t dispatchCallback(ffxDispatchDescFrameGeneration*, void*) noexcept;
    static ffxReturnCode_t presentCallback(ffxCallbackDescFrameGenerationPresent*, void*) noexcept;
    HRESULT present(FrameLease frame, const PresentArguments& args, bool generate);
    HRESULT resize(const DXGI_SWAP_CHAIN_DESC1& description, uint64_t generation);
    PresentRetirement stop() noexcept;
    template<class Operation> HRESULT controlChain(Operation operation, bool pause = false) noexcept {
        if (stopped || quarantined || !chain) return DXGI_ERROR_INVALID_CALL;
        try {
            if (!pause) return operation(chain.Get());
            drain();
            sdkControl([this] {
                if (fgContext) configure(fgContext, nextFrame, false, PresentationFrame{}, nullptr);
                graphicsCheck(device->GetDeviceRemovedReason(), "FSR control device health");
            });
            lastEnabled = false; historyReset = true;
            // Preserve the caller's DXGI/Win32 thread for the actual control.
            // Only the SDK's potentially unbounded retirement runs on a worker.
            return operation(chain.Get());
        } catch (const std::exception& exception) { quarantine(exception.what()); }
        catch (...) { quarantine("Unknown FSR swapchain control failure"); }
        return E_FAIL;
    }
};

void FsrPresenter::Impl::initialize() {
    if (!creation.window || !IsWindow(creation.window) || !factory || !device || !queue ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || creation.swapChain.SampleDesc.Count != 1 ||
        creation.swapChain.SampleDesc.Quality || creation.swapChain.BufferCount < 2 ||
        !colorFormat(creation.swapChain.Format))
        throw std::invalid_argument("Invalid FSR presenter device/window/swapchain contract");
    ComPtr<ID3D12Device> queueDevice;
    graphicsCheck(queue->GetDevice(IID_PPV_ARGS(&queueDevice)), "Get FSR game queue device");
    if (queueDevice.Get() != device.Get()) throw std::invalid_argument("FSR queue belongs to another device");
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_2};
    graphicsCheck(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)),
                  "FSR FG shader model query");
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_2)
        throw std::runtime_error("Analytical FSR FG requires shader model 6.2");
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&presentCallback), &callbackModule.handle))
        graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Retain FSR callback module");
    effectModule.load(creation.runtimeDirectory / "amd_fidelityfx_framegeneration_dx12.dll");
    loaderModule.load(creation.runtimeDirectory / "amd_fidelityfx_loader_dx12.dll");
    ffxLoadFunctions(&api, loaderModule.handle);
    if (!api.CreateContext || !api.DestroyContext || !api.Query || !api.Configure || !api.Dispatch)
        throw std::runtime_error("Incomplete FSR FG loader API");
    algorithmId = provider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, "3.1.6");
    chainId = provider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, "3.1.7");
    graphicsCheck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create FSR host fence");
    fenceEvent.handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent.handle) graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Create FSR host event");
    auto& selection = chainSelection;
    selection.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION; selection.versionId = chainId;
    auto& version = chainVersion;
    version.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12, &selection.header};
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    auto& desc = chainDescription;
    desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, &version.header};
    desc.swapchain = chain.GetAddressOf(); desc.hwnd = creation.window;
    desc.desc = &creation.swapChain;
    desc.fullscreenDesc = creation.fullscreen ? &*creation.fullscreen : nullptr;
    desc.dxgiFactory = factory.Get(); desc.gameQueue = queue.Get();
    checkFfx(api.CreateContext(&chainContext, &desc.header, nullptr), "Create FSR 3.1.7 swapchain");
    verifyProvider(chainContext, chainId);
    graphicsCheck(chain->GetDesc1(&creation.swapChain), "Read actual FSR swapchain dimensions");
    if (!creation.swapChain.Width || !creation.swapChain.Height)
        throw std::runtime_error("FSR created an empty swapchain");
    srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    pipelines();
    FfxApiEffectMemoryUsage memory{};
    ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12;
    query.gpuMemoryUsageFrameGenerationSwapchain = &memory;
    if (api.Query(&chainContext, &query.header) == FFX_API_RETURN_OK) chainMemory = memory.totalUsageInBytes;
}
uint64_t FsrPresenter::Impl::provider(uint64_t type, const char* wanted) {
    uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = type; query.device = device.Get(); query.outputCount = &count;
    checkFfx(api.Query(nullptr, &query.header), "Enumerate FSR FG providers");
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    const auto capacity = count;
    query.versionIds = ids.data(); query.versionNames = names.data();
    checkFfx(api.Query(nullptr, &query.header), "Read FSR FG providers");
    if (count > capacity) throw std::runtime_error("FSR provider list changed during enumeration");
    for (size_t i = 0; i < count; ++i)
        if (names[i] && std::strcmp(names[i], wanted) == 0) return ids[i];
    throw std::runtime_error(std::string("Required FSR provider ") + wanted + " is unavailable; ML is not substituted");
}
void FsrPresenter::Impl::verifyProvider(ffxContext context, uint64_t expected) {
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    checkFfx(api.Query(&context, &query.header), "Verify FSR FG provider");
    if (query.versionId != expected) throw std::runtime_error("FSR ignored the explicitly selected provider");
}
ComPtr<ID3D12PipelineState> FsrPresenter::Impl::pipeline(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    for (auto& blend : desc.BlendState.RenderTarget) {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace =
        {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = format; desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> result;
    graphicsCheck(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&result)), "Create FSR UI pipeline");
    return result;
}
void FsrPresenter::Impl::pipelines() {
    if (!root) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 3;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range}; parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 1; desc.pParameters = &parameter;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> bytes, error;
        graphicsCheck(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &error),
                      "Serialize FSR UI root signature");
        graphicsCheck(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                                   IID_PPV_ARGS(&root)), "Create FSR UI root signature");
    }
    const auto vs = compileShader("fullscreen", "vs_5_1");
    const auto copy = compileShader("copyColor", "ps_5_1");
    const auto residual = compileShader("makeResidual", "ps_5_1");
    const auto compose = compileShader("compose", "ps_5_1");
    copyPipeline = pipeline(vs.Get(), copy.Get(), creation.swapChain.Format);
    residualPipeline = pipeline(vs.Get(), residual.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    composePipeline = pipeline(vs.Get(), compose.Get(), creation.swapChain.Format);
}
uint64_t FsrPresenter::Impl::signal() {
    const auto value = ++nextFence;
    graphicsCheck(queue->Signal(fence.Get(), value), "Signal FSR presenter host fence");
    lastSubmitted = value;
    return value;
}
uint64_t FsrPresenter::Impl::completed() {
    graphicsCheck(device->GetDeviceRemovedReason(), "FSR presenter device health");
    const auto value = fence->GetCompletedValue();
    if (value == UINT64_MAX) throw std::runtime_error("FSR presenter fence reports device removal");
    return value;
}
void FsrPresenter::Impl::wait(uint64_t value) {
    if (!value || completed() >= value) return;
    graphicsCheck(fence->SetEventOnCompletion(value, fenceEvent.handle), "Register FSR completion event");
    if (WaitForSingleObject(fenceEvent.handle, gpuTimeout) != WAIT_OBJECT_0 || completed() < value)
        throw std::runtime_error("FSR presenter GPU retirement was not proved");
}
void FsrPresenter::Impl::sdkControl(std::function<void()> operation) {
    if (control) throw std::runtime_error("A quarantined FSR SDK control operation is still owned");
    control = std::make_unique<ControlWork>();
    auto* work = control.get();
    try {
        work->thread = std::thread([work, operation = std::move(operation)]() mutable {
            try { operation(); work->done.set_value(); }
            catch (...) { try { work->done.set_exception(std::current_exception()); } catch (...) {} }
        });
    } catch (...) { control.reset(); throw; }
    if (work->completion.wait_for(std::chrono::milliseconds(gpuTimeout)) != std::future_status::ready) {
        quarantined = true;
        throw std::runtime_error("FSR SDK drain timed out; its complete owner remains quarantined");
    }
    work->thread.join();
    auto finished = std::move(control);
    finished->completion.get();
}
void FsrPresenter::Impl::waitSdk() {
    if (!chainContext) return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 waitForPresents{};
    waitForPresents.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    checkFfx(api.Dispatch(&chainContext, &waitForPresents.header), "Drain FSR SDK presents");
    graphicsCheck(device->GetDeviceRemovedReason(), "FSR SDK drain device health");
}
void FsrPresenter::Impl::collect() {
    const auto value = completed();
    std::lock_guard lock(slotsMutex);
    for (auto& slot : slots) {
        if (slot.copied && slot.copied <= value) slot.lease.reset();
        if (slot.retired && slot.retired <= value) slot.occupied = false;
    }
}
void FsrPresenter::Impl::drain() {
    if (!fence) return;
    wait(lastSubmitted);
    sdkControl([this] { waitSdk(); });
    const auto value = signal();
    wait(value);
    std::lock_guard lock(slotsMutex);
    for (auto& slot : slots) { slot.lease.reset(); slot.retired = value; slot.occupied = false; }
    previous = nullptr; historyReset = true;
}

D3D12_RESOURCE_DESC FsrPresenter::Impl::validate(const PresentImage& image, unsigned width, unsigned height,
                                                const char* role) const {
    if (!image.resource) throw std::invalid_argument(std::string("Missing presentation ") + role);
    const auto desc = image.resource->GetDesc();
    ComPtr<ID3D12Device> owner;
    graphicsCheck(image.resource->GetDevice(IID_PPV_ARGS(&owner)), "Get presentation image device");
    if (owner.Get() != device.Get() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width != width || desc.Height != height || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality)
        throw std::invalid_argument(std::string("Invalid presentation image contract: ") + role);
    return desc;
}
void FsrPresenter::Impl::validateTemporal(const PresentationFrame& frame) const {
    const auto width = creation.swapChain.Width, height = creation.swapChain.Height;
    if (!frame.inputsComplete || !frame.cameraMotionIncluded || !frame.renderWidth || !frame.renderHeight ||
        frame.renderWidth > width || frame.renderHeight > height)
        throw std::invalid_argument("Incomplete or incompatible FSR temporal inputs");
    const auto& images = *frame.images;
    if (!colorFormat(validate(images.hudless, width, height, "Hudless").Format))
        throw std::invalid_argument("Unsupported Hudless format");
    const auto alpha = readableFormat(validate(images.occlusionAlpha, width, height, "occlusion alpha").Format);
    if (alpha != DXGI_FORMAT_R8_UNORM && alpha != DXGI_FORMAT_R16_FLOAT && alpha != DXGI_FORMAT_R32_FLOAT)
        throw std::invalid_argument("Occlusion alpha must be independently rendered single-channel coverage");
    const auto depth = readableFormat(validate(images.depth, frame.renderWidth, frame.renderHeight, "depth").Format);
    if (depth != DXGI_FORMAT_R32_FLOAT && depth != DXGI_FORMAT_R16_FLOAT)
        throw std::invalid_argument("FSR depth must contain device-depth values in R16F or R32F");
    if (!images.motion.resource) throw std::invalid_argument("Missing motion vectors");
    const auto motion = images.motion.resource->GetDesc();
    const bool displayMotion = motion.Width == width && motion.Height == height;
    const auto motionFormat = readableFormat(validate(images.motion, displayMotion ? width : frame.renderWidth,
                                                       displayMotion ? height : frame.renderHeight, "motion vectors").Format);
    if (motionFormat != DXGI_FORMAT_R16G16_FLOAT && motionFormat != DXGI_FORMAT_R32G32_FLOAT)
        throw std::invalid_argument("FSR motion vectors must be RG16F or RG32F");
    if (images.fsrDistortion.resource) {
        const auto distortion = readableFormat(validate(images.fsrDistortion, width, height, "FSR distortion").Format);
        if (distortion != DXGI_FORMAT_R16G16_FLOAT && distortion != DXGI_FORMAT_R32G32_FLOAT)
            throw std::invalid_argument("FSR distortion must be RG normalized forward UV displacement");
    }
    for (float value : {frame.jitterX, frame.jitterY, frame.motionScaleX, frame.motionScaleY,
                        frame.deltaMilliseconds, frame.cameraNear, frame.cameraFar, frame.verticalFov,
                        frame.viewSpaceToMeters, frame.minLuminance, frame.maxLuminance})
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite FSR camera metadata");
    if (frame.deltaMilliseconds <= 0 || frame.cameraNear <= 0 || frame.cameraFar <= frame.cameraNear ||
        frame.verticalFov <= 0 || frame.verticalFov >= 3.14159265f || frame.viewSpaceToMeters <= 0 ||
        frame.motionScaleX == 0 || frame.motionScaleY == 0 || frame.minLuminance < 0 ||
        frame.maxLuminance <= frame.minLuminance)
        throw std::invalid_argument("Invalid FSR camera/luminance metadata");
    for (const auto* vector : {&frame.cameraPosition, &frame.cameraUp, &frame.cameraRight, &frame.cameraForward}) {
        float length = 0;
        for (const auto value : *vector) {
            if (!std::isfinite(value)) throw std::invalid_argument("Non-finite FSR camera basis");
            length += value * value;
        }
        if (vector != &frame.cameraPosition && (length < .99f || length > 1.01f))
            throw std::invalid_argument("FSR camera basis is not normalized");
    }
    (void)transferFunction(frame.colorSpace);
    (void)generationRect(frame.generationRect, width, height);
}
uint32_t FsrPresenter::Impl::flags(const PresentationFrame& frame) const {
    uint32_t result = 0;
    if (frame.depthInverted) result |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (frame.depthInfinite) result |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (frame.motionVectorsJittered) result |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (frame.colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) result |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    if (creation.debugChecking) result |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    const auto mv = frame.images->motion.resource->GetDesc();
    if (mv.Width == creation.swapChain.Width && mv.Height == creation.swapChain.Height)
        result |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    return result;
}
void FsrPresenter::Impl::configure(ffxContext context, uint64_t id, bool enabled,
                                   const PresentationFrame& frame, ID3D12Resource* distortion) {
    ffxConfigureDescFrameGenerationRegisterDistortionFieldResource lens{};
    lens.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE;
    lens.distortionField = ffxApiGetResourceDX12(distortion);
    ffxConfigureDescFrameGeneration configuration{};
    configuration.header = {FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION, &lens.header};
    configuration.swapChain = chain.Get();
    configuration.presentCallback = presentCallback;
    configuration.presentCallbackUserContext = this;
    configuration.frameGenerationCallback = dispatchCallback;
    configuration.frameGenerationCallbackUserContext = this;
    configuration.frameGenerationEnabled = enabled;
    configuration.allowAsyncWorkloads = false;
    configuration.frameID = id;
    configuration.generationRect = enabled ? generationRect(frame.generationRect, creation.swapChain.Width,
                                                           creation.swapChain.Height) : FfxApiRect2D{};
    // The replacement backbuffer is already clean H. Supplying HUDLessColor
    // would invoke the SDK's UI extraction a second time, so it stays empty.
    checkFfx(api.Configure(&context, &configuration.header), "Configure analytical FSR FG");
}
void FsrPresenter::Impl::ensureContext(const PresentationFrame& frame) {
    const auto wantedFlags = flags(frame);
    if (fgContext && contextWidth == creation.swapChain.Width && contextHeight == creation.swapChain.Height &&
        contextFlags == wantedFlags) return;
    try { drain(); } catch (...) { quarantined = true; throw; }
    auto candidate = std::make_unique<Effect>();
    auto* next = candidate.get();
    next->selection.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    next->selection.versionId = algorithmId;
    next->backend.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, &next->selection.header};
    next->backend.device = device.Get();
    next->version.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &next->backend.header};
    next->version.version = FFX_FRAMEGENERATION_VERSION;
    next->description.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &next->version.header};
    next->description.flags = wantedFlags;
    next->description.displaySize = {creation.swapChain.Width, creation.swapChain.Height};
    next->description.maxRenderSize = next->description.displaySize;
    next->description.backBufferFormat = ffxApiGetSurfaceFormatDX12(creation.swapChain.Format);
    effects.push_back(std::move(candidate));
    checkFfx(api.CreateContext(&next->context, &next->description.header, nullptr), "Create analytical FSR 3.1.6 FG");
    verifyProvider(next->context, algorithmId);
    // A swapchain callback contains the SDK's InternalFgContext, not just our
    // userdata. Keep old contexts alive until the replacement is installed.
    PresentationFrame metadata = frame;
    metadata.images.reset(); metadata.readyFence.Reset();
    try {
        sdkControl([this, next, metadata] {
            configure(next->context, nextFrame + 1, false, metadata, nullptr);
            fgContext = next->context;
            for (auto& old : effects)
                if (old.get() != next && old->context) {
                    checkFfx(api.DestroyContext(&old->context, nullptr), "Retire replaced FSR FG context");
                    // DestroyContext does not promise to clear the caller's handle.
                    // Retirement must not retain a stale pointer for the next resize.
                    old->context = nullptr;
                }
        });
    } catch (...) { quarantined = true; throw; }
    effects.erase(std::remove_if(effects.begin(), effects.end(), [](const auto& effect) { return !effect->context; }), effects.end());
    contextFlags = wantedFlags; contextWidth = creation.swapChain.Width; contextHeight = creation.swapChain.Height;
    lastEnabled = false; contextVerified = true;
    FfxApiEffectMemoryUsage memory{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    query.gpuMemoryUsageFrameGeneration = &memory;
    if (api.Query(&fgContext, &query.header) == FFX_API_RETURN_OK) fgMemory = memory.totalUsageInBytes;
}
FsrPresenter::Impl::Slot& FsrPresenter::Impl::acquire() {
    collect();
    for (auto& slot : slots) if (!slot.occupied) return slot;
    uint64_t first = UINT64_MAX;
    for (const auto& slot : slots) if (slot.retired) first = std::min(first, slot.retired);
    if (first == UINT64_MAX) throw std::runtime_error("FSR private slot retirement has no queue proof");
    wait(first); collect();
    for (auto& slot : slots) if (!slot.occupied) return slot;
    throw std::runtime_error("FSR private slot remained in use after retirement");
}
ComPtr<ID3D12Resource> FsrPresenter::Impl::texture(unsigned width, unsigned height, DXGI_FORMAT format,
                                                 D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format;
    desc.SampleDesc.Count = 1; desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    graphicsCheck(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)), "Create private FSR presentation image");
    return result;
}
void FsrPresenter::Impl::allocate(Slot& slot, const PresentationFrame& frame, bool generating) {
    if (!slot.allocator) {
        graphicsCheck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)),
                      "Create FSR presentation allocator");
        graphicsCheck(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&slot.commands)), "Create FSR presentation commands");
        graphicsCheck(slot.commands->Close(), "Close initial FSR presentation commands");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 9;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        graphicsCheck(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.views)), "Create FSR private SRVs");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 3; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        graphicsCheck(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.targets)), "Create FSR private RTVs");
    }
    const auto width = creation.swapChain.Width, height = creation.swapChain.Height;
    auto ensure = [this](Image& image, unsigned w, unsigned h, DXGI_FORMAT format,
                         D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        if (image.resource) {
            const auto desc = image.resource->GetDesc();
            if (desc.Width == w && desc.Height == h && desc.Format == format && desc.Flags == flags) return;
        }
        image.resource = texture(w, h, format, flags); image.state = D3D12_RESOURCE_STATE_COMMON;
    };
    ensure(slot.finalColor, width, height, creation.swapChain.Format);
    if (!generating) return;
    const auto& images = *frame.images;
    ensure(slot.hudlessRaw, width, height, readableFormat(images.hudless.resource->GetDesc().Format));
    ensure(slot.hudless, width, height, creation.swapChain.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ensure(slot.alpha, width, height, readableFormat(images.occlusionAlpha.resource->GetDesc().Format));
    ensure(slot.residual, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ensure(slot.depth, frame.renderWidth, frame.renderHeight, readableFormat(images.depth.resource->GetDesc().Format));
    const auto mv = images.motion.resource->GetDesc();
    ensure(slot.motion, static_cast<unsigned>(mv.Width), mv.Height, readableFormat(mv.Format));
    if (images.fsrDistortion.resource)
        ensure(slot.distortion, width, height, readableFormat(images.fsrDistortion.resource->GetDesc().Format));
    for (unsigned i = 0; i < 9; ++i) view(slot.views.Get(), i, nullptr);
    view(slot.views.Get(), 0, slot.hudlessRaw.resource.Get());
    view(slot.views.Get(), 3, slot.finalColor.resource.Get());
    view(slot.views.Get(), 4, slot.hudless.resource.Get());
    view(slot.views.Get(), 5, slot.alpha.resource.Get());
    view(slot.views.Get(), 7, slot.residual.resource.Get());
}
void FsrPresenter::Impl::view(ID3D12DescriptorHeap* heap, unsigned index, ID3D12Resource* resource) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = resource ? readableFormat(resource->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture2D.MipLevels = 1;
    auto handle = heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += static_cast<SIZE_T>(index) * srvStride;
    device->CreateShaderResourceView(resource, &desc, handle);
}
D3D12_CPU_DESCRIPTOR_HANDLE FsrPresenter::Impl::target(ID3D12DescriptorHeap* heap, unsigned index,
                                                        ID3D12Resource* resource) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += static_cast<SIZE_T>(index) * rtvStride;
    D3D12_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = resource->GetDesc().Format; desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(resource, &desc, handle);
    return handle;
}
void FsrPresenter::Impl::draw(ID3D12GraphicsCommandList* commands, ID3D12PipelineState* pipeline,
                              ID3D12DescriptorHeap* views, unsigned first, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    commands->SetGraphicsRootSignature(root.Get()); commands->SetPipelineState(pipeline);
    commands->SetDescriptorHeaps(1, &views);
    auto table = views->GetGPUDescriptorHandleForHeapStart(); table.ptr += static_cast<UINT64>(first) * srvStride;
    commands->SetGraphicsRootDescriptorTable(0, table);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(creation.swapChain.Width),
                               static_cast<float>(creation.swapChain.Height), 0, 1};
    const RECT scissor{0, 0, static_cast<LONG>(creation.swapChain.Width), static_cast<LONG>(creation.swapChain.Height)};
    commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &destination, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
}
void FsrPresenter::Impl::copyInput(ID3D12GraphicsCommandList* commands, const PresentImage& source,
                                   Image& destination, D3D12_RESOURCE_STATES after) {
    barrier(commands, source.resource.Get(), source.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    change(commands, destination, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(destination.resource.Get(), source.resource.Get());
    barrier(commands, source.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source.state);
    change(commands, destination, after);
}

void FsrPresenter::Impl::prepare(Slot& slot) {
    const auto& frame = slot.metadata;
    ffxDispatchDescFrameGenerationPrepareV2 desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    desc.frameID = slot.id; desc.commandList = slot.commands.Get();
    desc.renderSize = {frame.renderWidth, frame.renderHeight};
    desc.jitterOffset = {frame.jitterX, frame.jitterY};
    desc.motionVectorScale = {frame.motionScaleX, frame.motionScaleY};
    desc.frameTimeDelta = frame.deltaMilliseconds; desc.reset = frame.reset;
    desc.cameraNear = frame.depthInverted ? frame.cameraFar : frame.cameraNear;
    desc.cameraFar = frame.depthInverted ? frame.cameraNear : frame.cameraFar;
    desc.cameraFovAngleVertical = frame.verticalFov;
    desc.viewSpaceToMetersFactor = frame.viewSpaceToMeters;
    desc.depth = ffxApiGetResourceDX12(slot.depth.resource.Get());
    desc.motionVectors = ffxApiGetResourceDX12(slot.motion.resource.Get());
    std::copy(frame.cameraPosition.begin(), frame.cameraPosition.end(), desc.cameraPosition);
    std::copy(frame.cameraUp.begin(), frame.cameraUp.end(), desc.cameraUp);
    std::copy(frame.cameraRight.begin(), frame.cameraRight.end(), desc.cameraRight);
    std::copy(frame.cameraForward.begin(), frame.cameraForward.end(), desc.cameraForward);
    checkFfx(api.Dispatch(&fgContext, &desc.header), "Prepare analytical FSR FG");
}
void FsrPresenter::Impl::upload(Slot& slot, bool generating, ID3D12Resource* backbuffer) {
    graphicsCheck(slot.allocator->Reset(), "Reset retired FSR presentation allocator");
    graphicsCheck(slot.commands->Reset(slot.allocator.Get(), nullptr), "Reset retired FSR presentation commands");
    auto* commands = slot.commands.Get();
    const auto& inputs = *slot.lease->images;
    copyInput(commands, inputs.finalColor, slot.finalColor, pixelRead);
    if (generating) {
        copyInput(commands, inputs.hudless, slot.hudlessRaw, pixelRead);
        copyInput(commands, inputs.occlusionAlpha, slot.alpha, pixelRead);
        copyInput(commands, inputs.depth, slot.depth, computeRead);
        copyInput(commands, inputs.motion, slot.motion, computeRead);
        if (inputs.fsrDistortion.resource) copyInput(commands, inputs.fsrDistortion, slot.distortion, computeRead);
        change(commands, slot.hudless, D3D12_RESOURCE_STATE_RENDER_TARGET);
        draw(commands, copyPipeline.Get(), slot.views.Get(), 0, target(slot.targets.Get(), 0, slot.hudless.resource.Get()));
        change(commands, slot.hudless, pixelRead);
        change(commands, slot.residual, D3D12_RESOURCE_STATE_RENDER_TARGET);
        draw(commands, residualPipeline.Get(), slot.views.Get(), 3, target(slot.targets.Get(), 1, slot.residual.resource.Get()));
        change(commands, slot.residual, pixelRead);
        prepare(slot);
    }
    auto& presentation = generating ? slot.hudless : slot.finalColor;
    change(commands, presentation, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(commands, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(backbuffer, presentation.resource.Get());
    barrier(commands, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    change(commands, presentation, pixelRead);
    graphicsCheck(commands->Close(), "Close FSR presentation inputs");
    ID3D12CommandList* lists[] = {commands};
    queue->ExecuteCommandLists(1, lists);
    // Only this proof releases the producer lease; SDK consumption is tracked
    // separately by Slot::retired and can outlive this copy completion.
    slot.copied = signal();
}
bool FsrPresenter::Impl::snapshot(uint64_t id, Snapshot& output) {
    std::lock_guard lock(slotsMutex);
    for (const auto& slot : slots) {
        if (!slot.occupied || slot.id != id) continue;
        output.metadata = slot.metadata; output.generating = slot.generating;
        output.finalColor = slot.finalColor.resource; output.residual = slot.residual.resource;
        output.finalState = slot.finalColor.state; output.residualState = slot.residual.state;
        output.views = slot.views; output.targets = slot.targets;
        return true;
    }
    return false;
}
ffxReturnCode_t FsrPresenter::Impl::dispatchCallback(ffxDispatchDescFrameGeneration* desc, void* user) noexcept {
    auto* self = static_cast<Impl*>(user);
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    try {
        Snapshot slot;
        if (!self || !self->fgContext || !self->snapshot(desc->frameID, slot) || !slot.generating ||
            !desc->commandList || desc->numGeneratedFrames != 1)
            throw std::runtime_error("FSR dispatch did not match its prepared private slot");
        desc->reset = desc->reset || slot.metadata.reset;
        desc->backbufferTransferFunction = transferFunction(slot.metadata.colorSpace);
        desc->minMaxLuminance[0] = slot.metadata.minLuminance;
        desc->minMaxLuminance[1] = slot.metadata.maxLuminance;
        desc->generationRect = generationRect(slot.metadata.generationRect, self->creation.swapChain.Width,
                                               self->creation.swapChain.Height);
        const auto result = self->api.Dispatch(&self->fgContext, &desc->header);
        if (result != FFX_API_RETURN_OK) {
            // The pinned swapchain still examines this count even when the
            // callback failed and it dropped the interpolation command list.
            desc->numGeneratedFrames = 0;
            self->dispatchFailed();
            return result;
        }
        self->dispatches.fetch_add(1);
        return FFX_API_RETURN_OK;
    } catch (...) {
        desc->numGeneratedFrames = 0;
        if (self) self->dispatchFailed();
        return FFX_API_RETURN_ERROR;
    }
}
ffxReturnCode_t FsrPresenter::Impl::presentCallback(ffxCallbackDescFrameGenerationPresent* desc, void* user) noexcept {
    auto* self = static_cast<Impl*>(user);
    try {
        Snapshot slot;
        if (!self || !desc || !desc->commandList || desc->device != self->device.Get() ||
            !desc->outputSwapChainBuffer.resource || !self->snapshot(desc->frameID, slot))
            throw std::runtime_error("FSR present callback did not match its private slot");
        auto* commands = static_cast<ID3D12GraphicsCommandList*>(desc->commandList);
        auto* output = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
        const auto outputState = nativeState(desc->outputSwapChainBuffer.state);
        if (!desc->isGeneratedFrame) {
            // Real frames bypass residual storage entirely: bit-preserving copy
            // of Final, not a reconstruction through signed FP16 intermediates.
            barrier(commands, slot.finalColor.Get(), slot.finalState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(commands, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->CopyResource(output, slot.finalColor.Get());
            barrier(commands, output, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
            barrier(commands, slot.finalColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, slot.finalState);
            self->realCallbacks.fetch_add(1);
        } else {
            if (!slot.generating || !slot.residual || !desc->currentBackBuffer.resource)
                throw std::runtime_error("Missing generated-frame residual or background");
            auto* background = static_cast<ID3D12Resource*>(desc->currentBackBuffer.resource);
            const auto state = nativeState(desc->currentBackBuffer.state);
            self->view(slot.views.Get(), 6, background);
            const auto destination = self->target(slot.targets.Get(), 2, output);
            barrier(commands, background, state, pixelRead);
            barrier(commands, slot.residual.Get(), slot.residualState, pixelRead);
            barrier(commands, output, outputState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            self->draw(commands, self->composePipeline.Get(), slot.views.Get(), 6, destination);
            barrier(commands, output, D3D12_RESOURCE_STATE_RENDER_TARGET, outputState);
            barrier(commands, slot.residual.Get(), pixelRead, slot.residualState);
            barrier(commands, background, pixelRead, state);
            self->generatedCallbacks.fetch_add(1);
        }
        return FFX_API_RETURN_OK;
    } catch (...) {
        if (self) { self->callbackError.store(2); self->quarantined.store(true); }
        return FFX_API_RETURN_ERROR;
    }
}
HRESULT FsrPresenter::Impl::present(FrameLease frame, const PresentArguments& args, bool generate) {
    if (stopped || quarantined || !chain) return DXGI_ERROR_INVALID_CALL;
    if (args.flags & DXGI_PRESENT_TEST) return chain->Present(args.syncInterval, args.flags);
    if (!frame || !frame->images || frame->generation != creation.generation || !frame->applicationFrameId ||
        frame->applicationFrameId <= lastApplicationFrame || (frame->readyValue && !frame->readyFence)) return E_INVALIDARG;
    const auto final = validate(frame->images->finalColor, creation.swapChain.Width, creation.swapChain.Height, "Final");
    if (readableFormat(final.Format) != readableFormat(creation.swapChain.Format)) return E_INVALIDARG;
    if (callbackError.load() == 2) throw std::runtime_error("FSR composition callback failed; presentation is quarantined");
    if (callbackError.load() == 1) {
        fgFaulted = true;
        error("FSR interpolation dispatch failed; original Final presentation remains active");
    }
    const bool requested = generate;
    generate = generate && !fgFaulted;
    if (generate) {
        try { validateTemporal(*frame); }
        catch (const std::exception& exception) { generate = false; error(exception.what()); }
    }
    if (frame->colorSpace != colorSpace) {
        drain();
        graphicsCheck(chain->SetColorSpace1(frame->colorSpace), "Set FSR presentation color space");
        colorSpace = frame->colorSpace;
    }
    if (generate) {
        try { ensureContext(*frame); }
        catch (const std::exception& exception) {
            if (quarantined) throw;
            fgFaulted = true; generate = false; error(exception.what());
        }
    }
    auto& slot = acquire();
    allocate(slot, *frame, generate);
    {
        std::lock_guard lock(slotsMutex);
        slot.occupied = true; slot.copied = slot.retired = 0;
        nextFrame = frame->applicationFrameId;
        slot.id = nextFrame;
        slot.generating = generate; slot.lease = frame; slot.metadata = *frame;
        slot.metadata.reset = frame->reset || historyReset || !lastEnabled ||
                              frame->applicationFrameId != lastApplicationFrame + 1;
        slot.metadata.images.reset(); slot.metadata.readyFence.Reset(); slot.metadata.readyValue = 0;
    }
    if (fgContext) {
        auto* distortion = generate && frame->images->fsrDistortion.resource ? slot.distortion.resource.Get() : nullptr;
        if (generate != lastEnabled) {
            // The SDK reconfiguration contains its own unbounded drain. All
            // captures live in this owned job, never in the caller's stack.
            const auto metadata = slot.metadata;
            const auto id = slot.id;
            const ComPtr<ID3D12Resource> retainedDistortion = distortion;
            sdkControl([this, id, generate, metadata, retainedDistortion] {
                configure(fgContext, id, generate, metadata, retainedDistortion.Get());
                graphicsCheck(device->GetDeviceRemovedReason(), "FSR mode-change device health");
            });
        } else configure(fgContext, slot.id, generate, slot.metadata, distortion);
    }
    if (frame->readyFence && frame->readyValue) {
        if (frame->readyFence->GetCompletedValue() == UINT64_MAX)
            throw std::runtime_error("Presentation producer fence reports device removal");
        graphicsCheck(queue->Wait(frame->readyFence.Get(), frame->readyValue), "Wait for FSR presentation producer");
    }
    ComPtr<ID3D12Resource> backbuffer;
    graphicsCheck(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "Get FSR replacement backbuffer");
    upload(slot, generate, backbuffer.Get());
    // FSR 3.1.7 Present1 ignores dirty rectangles; all our paths produce complete
    // frames. Do not promise partial-update semantics or retain this borrowed ptr.
    if (args.boundary && args.boundary->before) args.boundary->before(args.boundary->context, frame->applicationFrameId);
    const auto result = args.parameters ? chain->Present1(args.syncInterval, args.flags, args.parameters) :
                                          chain->Present(args.syncInterval, args.flags);
    if (args.boundary && args.boundary->after) args.boundary->after(args.boundary->context, frame->applicationFrameId);
    const auto retirement = signal();
    {
        std::lock_guard lock(slotsMutex);
        // Present(k) inserts a game-queue wait for composition(k-1). Its CPU
        // return alone is not retirement of private inputs for frame k.
        if (previous) previous->retired = retirement;
        if (generate) previous = &slot;
        else { slot.retired = retirement; previous = nullptr; }
    }
    lastEnabled = generate; lastApplicationFrame = frame->applicationFrameId; historyReset = false;
    ++applicationPresents;
    graphicsCheck(device->GetDeviceRemovedReason(), "FSR post-present device health");
    if (callbackError.load() == 2) throw std::runtime_error("FSR composition failed; final-image preservation was not proved");
    if (callbackError.load() == 1) {
        fgFaulted = true;
        error("FSR interpolation dispatch failed; original Final presentation remains active");
    }
    if (generate && !callbackError.load()) reason.clear();
    else if (!requested && !fgFaulted) reason.clear();
    // This result reports accepted submission only: the pinned SDK can return
    // S_OK even when its underlying real swapchain Present failed.
    return result;
}
void FsrPresenter::Impl::destroyContexts() {
    // The swapchain owns wrappers that dereference the effect's InternalFgContext.
    // It must stop and disappear BEFORE the last such context can be destroyed.
    if (chainContext) {
        checkFfx(api.DestroyContext(&chainContext, nullptr), "Destroy FSR swapchain context");
        chainContext = nullptr;
    }
    chain.Reset();
    for (auto& effect : effects)
        if (effect->context) {
            checkFfx(api.DestroyContext(&effect->context, nullptr), "Destroy FSR FG context");
            effect->context = nullptr;
        }
    fgContext = nullptr;
}
PresentRetirement FsrPresenter::Impl::stop() noexcept {
    if (quarantined) return PresentRetirement::Quarantined;
    if (stopped) return PresentRetirement::Drained;
    try {
        if (fence) wait(lastSubmitted);
        sdkControl([this] { waitSdk(); destroyContexts(); });
        if (fence && fenceEvent.handle) wait(signal());
        for (auto& slot : slots) slot = Slot{};
        previous = nullptr; lastEnabled = false; stopped = true;
        return PresentRetirement::Drained;
    } catch (const std::exception& exception) { quarantine(exception.what()); }
    catch (...) { quarantine("Unknown FSR retirement failure"); }
    return PresentRetirement::Quarantined;
}
HRESULT FsrPresenter::Impl::resize(const DXGI_SWAP_CHAIN_DESC1& desc, uint64_t generation) {
    if (stopped || quarantined || !chain) return DXGI_ERROR_INVALID_CALL;
    if (desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || !colorFormat(desc.Format) || desc.BufferCount < 2 ||
        desc.SwapEffect != creation.swapChain.SwapEffect || desc.AlphaMode != creation.swapChain.AlphaMode ||
        desc.Scaling != creation.swapChain.Scaling || desc.BufferUsage != creation.swapChain.BufferUsage)
        return E_INVALIDARG;
    drain();
    // Keep the old effect alive during ResizeBuffers: the SDK may retain its
    // present callback for Off frames until the new effect replaces it safely.
    sdkControl([this, desc] {
        graphicsCheck(chain->ResizeBuffers(desc.BufferCount, desc.Width, desc.Height, desc.Format, desc.Flags),
                      "Resize FSR presentation swapchain");
    });
    graphicsCheck(chain->GetDesc1(&creation.swapChain), "Read resized FSR swapchain");
    for (auto& slot : slots) slot = Slot{};
    creation.generation = generation; contextWidth = contextHeight = 0;
    previous = nullptr; lastApplicationFrame = 0; fgFaulted = false; callbackError.store(0);
    pipelines();
    return S_OK;
}

FsrPresenter::FsrPresenter(const FsrPresenterCreateInfo& info) : impl_(std::make_unique<Impl>(info)) {
    try { impl_->initialize(); }
    catch (...) {
        const auto failure = std::current_exception();
        const auto retirement = impl_->stop();
        if (retirement == PresentRetirement::Quarantined) (void)impl_.release();
        try { std::rethrow_exception(failure); }
        catch (const std::exception& exception) { throw FsrPresenterCreationError(exception.what(), retirement); }
        catch (...) { throw FsrPresenterCreationError("Unknown FSR presenter initialization failure", retirement); }
    }
}
FsrPresenter::~FsrPresenter() {
    if (impl_ && impl_->stop() == PresentRetirement::Quarantined) {
        // Intentional fail-closed quarantine: leaking the complete owner is safer
        // than freeing live GPU resources, callback code or a blocked SDK worker.
        // The caller must not reuse this HWND after a Quarantined result.
        (void)impl_.release();
    }
}
HRESULT FsrPresenter::present(FrameLease frame, const PresentArguments& arguments, bool generate) noexcept {
    try { return impl_->present(std::move(frame), arguments, generate); }
    catch (const std::invalid_argument& exception) { impl_->error(exception.what()); return E_INVALIDARG; }
    catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown FSR presentation failure"); }
    return E_FAIL;
}
HRESULT FsrPresenter::resize(const DXGI_SWAP_CHAIN_DESC1& description, uint64_t generation) noexcept {
    try { return impl_->resize(description, generation); }
    catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown FSR resize failure"); }
    return E_FAIL;
}
PresentRetirement FsrPresenter::stop() noexcept { return impl_->stop(); }
HRESULT FsrPresenter::setFullscreenState(BOOL fullscreen, IDXGIOutput* output) noexcept {
    const ComPtr<IDXGIOutput> retainedOutput = output;
    return impl_->controlChain([fullscreen, retainedOutput](IDXGISwapChain4* chain) {
        return chain->SetFullscreenState(fullscreen, retainedOutput.Get());
    }, true);
}
HRESULT FsrPresenter::resizeTarget(const DXGI_MODE_DESC& description) noexcept {
    return impl_->controlChain([description](IDXGISwapChain4* chain) { return chain->ResizeTarget(&description); }, true);
}
HRESULT FsrPresenter::setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept {
    const auto result = impl_->controlChain([colorSpace](IDXGISwapChain4* chain) { return chain->SetColorSpace1(colorSpace); }, true);
    if (SUCCEEDED(result)) { impl_->colorSpace = colorSpace; impl_->historyReset = true; }
    return result;
}
HRESULT FsrPresenter::setHdrMetadata(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) noexcept {
    return impl_->controlChain([type, size, data](IDXGISwapChain4* chain) { return chain->SetHDRMetaData(type, size, data); });
}
HRESULT FsrPresenter::setMaximumFrameLatency(UINT latency) noexcept {
    return impl_->controlChain([latency](IDXGISwapChain4* chain) { return chain->SetMaximumFrameLatency(latency); });
}
HRESULT FsrPresenter::setSourceSize(UINT width, UINT height) noexcept {
    return impl_->controlChain([width, height](IDXGISwapChain4* chain) { return chain->SetSourceSize(width, height); });
}
HRESULT FsrPresenter::setRotation(DXGI_MODE_ROTATION rotation) noexcept {
    return impl_->controlChain([rotation](IDXGISwapChain4* chain) { return chain->SetRotation(rotation); });
}
HRESULT FsrPresenter::setMatrixTransform(const DXGI_MATRIX_3X2_F& matrix) noexcept {
    return impl_->controlChain([matrix](IDXGISwapChain4* chain) { return chain->SetMatrixTransform(&matrix); });
}
HRESULT FsrPresenter::setBackgroundColor(const DXGI_RGBA& color) noexcept {
    return impl_->controlChain([color](IDXGISwapChain4* chain) { return chain->SetBackgroundColor(&color); });
}
IDXGISwapChain4* FsrPresenter::swapChainForQueries() const noexcept {
    // A timed-out owner may still have a control worker mutating its chain.
    return impl_->quarantined || impl_->stopped ? nullptr : impl_->chain.Get();
}
FsrPresenterStatus FsrPresenter::status() const {
    FsrPresenterStatus result;
    result.algorithm = "3.1.6 analytical"; result.swapChainVersion = "3.1.7";
    result.reason = impl_->reason;
    if (impl_->callbackError.load() == 2)
        result.reason = "FSR composition callback failed; the complete owner is quarantined";
    else if (impl_->callbackError.load() == 1 && result.reason.empty())
        result.reason = "FSR interpolation dispatch failed; original Final presentation remains active";
    result.applicationPresents = impl_->applicationPresents;
    result.successfulDispatches = impl_->dispatches.load();
    result.realCallbacks = impl_->realCallbacks.load(); result.generatedCallbacks = impl_->generatedCallbacks.load();
    result.frameGenerationMemoryBytes = impl_->fgMemory; result.swapChainMemoryBytes = impl_->chainMemory;
    result.generationActive = impl_->lastEnabled && !impl_->callbackError.load() && !impl_->quarantined && !impl_->stopped;
    result.providerVerified = impl_->contextVerified; result.quarantined = impl_->quarantined;
    return result;
}
} // namespace dspaa
