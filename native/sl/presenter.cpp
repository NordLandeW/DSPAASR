#include "presenter.h"
#include "internal.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<std::atomic<HRESULT>*> errorTarget{nullptr};
void onApiError(const sl::APIError& error) {
    if (auto* target = errorTarget.load()) target->store(error.hres);
}
DXGI_FORMAT sampleFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    default: return format;
    }
}
struct Image {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    void transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES next) {
        Dx11Dx12::transition(list, resource.Get(), state, next); state = next;
    }
};
struct Slot {
    Image finalColor, rawHudless, hudless, depth, motion, alpha, distortion;
    ComPtr<ID3D12DescriptorHeap> srv, rtv;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> consumedFence;
    uint64_t copied = 0, consumed = 0;
    FrameLease producer;
    std::shared_ptr<detail::SlTicket> ticket;
    bool tagged = false;
};
bool texture(const PresentImage& image) {
    if (!image.resource) return false;
    const auto description = image.resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width && description.Height &&
           description.DepthOrArraySize == 1 && description.MipLevels == 1 && description.SampleDesc.Count == 1;
}
bool sizeIs(const PresentImage& image, unsigned width, unsigned height) {
    if (!texture(image)) return false;
    const auto description = image.resource->GetDesc();
    return description.Width == width && description.Height == height;
}
sl::Boolean boolean(bool value) { return value ? sl::eTrue : sl::eFalse; }
sl::float3 vector(const std::array<float, 3>& value) { return {value[0], value[1], value[2]}; }
sl::float4x4 matrix(const std::array<float, 16>& value) {
    sl::float4x4 result;
    for (unsigned row = 0; row < 4; ++row) result[row] = {value[row * 4], value[row * 4 + 1], value[row * 4 + 2], value[row * 4 + 3]};
    return result;
}
constexpr char blitShader[] = R"(
Texture2D<float4> sourceImage : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    return sourceImage.Load(int3(uint2(position.xy), 0));
}
)";
ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(blitShader, sizeof(blitShader) - 1, "DSPAASR SL HUDless conversion", nullptr, nullptr,
                                  entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    graphicsCheck(result, "Compile Streamline HUDless format conversion");
    return code;
}
} // namespace
struct SlPresenter::Impl {
    SlPresenterCreateInfo creation;
    std::shared_ptr<detail::SlRuntimeState> runtime;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> chain, nativeChain;
    ComPtr<ID3D12Fence> copiedFence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<Slot, 3> slots;
    size_t nextSlot = 0;
    uint64_t nextFence = 0;
    HANDLE event = nullptr;
    std::atomic<HRESULT> asynchronousError{S_OK};
    SlPresenterStatus observation;
    DXGI_SWAP_CHAIN_DESC1 description{};
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool stopped = false, owned = false, forceReset = true;
    uint64_t lastApplicationFrame = 0;
    bool haveFrame = false, optionsConfigured = false;
    unsigned configuredGeneratedFrames = 1;
    float configuredDynamicTarget = 0;

    Impl(const SlPresenterCreateInfo& info, std::shared_ptr<detail::SlRuntimeState> state)
        : creation(info), runtime(std::move(state)), description(info.swapChain) {}
    ~Impl() { if (event) CloseHandle(event); }
    void initialize();
    void createPipeline();
    bool completed(ID3D12Fence* fence, uint64_t value);
    void wait(ID3D12Fence* fence, uint64_t value);
    void retire(Slot& slot);
    void drain();
    void pause();
    void ensure(Image& destination, const D3D12_RESOURCE_DESC& description);
    void prepare(Slot& slot, const PresentationFrame& frame, bool generate);
    void copy(Slot& slot, Image& destination, const PresentImage& source);
    void copyFrame(Slot& slot, const PresentationFrame& frame, bool generate);
    void tag(Slot& slot, const PresentationFrame& frame);
    void options(SlGenerationMode mode, const SlGenerationRequest& request = {});
    sl::DLSSGState query();
    bool eligible(const PresentationFrame& frame, const PresentArguments& arguments,
                  const SlGenerationRequest& request, const std::shared_ptr<detail::SlTicket>& ticket);
    HRESULT present(FrameLease frame, const PresentArguments& arguments, const SlGenerationRequest& request);
    PresentRetirement stop() noexcept;
    void quarantine(const char* reason) noexcept {
        runtime->quarantine(reason); observation.quarantined = true;
        try { observation.reason = reason; } catch (...) {}
    }
    bool usable() const { return !stopped && !runtime->quarantined && chain; }
};
void SlPresenter::Impl::initialize() {
    if (!runtime || !creation.window || !IsWindow(creation.window) || description.SampleDesc.Count != 1 ||
        description.Stereo || description.BufferCount < 2 ||
        (description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
        throw std::invalid_argument("Invalid single-window Streamline flip-chain description");
    runtime->activate(this); owned = true;
    std::atomic<HRESULT>* empty = nullptr;
    if (!errorTarget.compare_exchange_strong(empty, &asynchronousError))
        throw std::runtime_error("Another Streamline asynchronous presenter still owns the error callback");
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    graphicsCheck(runtime->proxyDevice->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "Create Streamline proxy presentation queue");
    // The plugin defaults to Off. Configure at the first actual Present rather
    // than overwriting frame 1 options once here and again on the presenting thread.
    ComPtr<IDXGISwapChain1> created;
    graphicsCheck(runtime->proxyFactory->CreateSwapChainForHwnd(queue.Get(), creation.window, &description,
                   creation.fullscreen ? &*creation.fullscreen : nullptr, nullptr, &created), "Create Streamline proxy swapchain");
    graphicsCheck(created.As(&chain), "Query Streamline swapchain4 proxy");
    void* native = nullptr;
    detail::slCheck(runtime->api.native(chain.Get(), &native), "Retrieve native Streamline swapchain");
    ComPtr<IUnknown> nativeUnknown;
    nativeUnknown.Attach(static_cast<IUnknown*>(native)); // slGetNativeInterface AddRefs the result.
    if (!native || native == chain.Get()) throw std::runtime_error("Streamline did not proxy the presentation chain");
    graphicsCheck(nativeUnknown.As(&nativeChain), "Query native Streamline swapchain4");
    DXGI_SWAP_CHAIN_DESC1 actual{};
    graphicsCheck(nativeChain->GetDesc1(&actual), "Query Streamline output size");
    description.Width = actual.Width; description.Height = actual.Height; description.Format = actual.Format;
    graphicsCheck(runtime->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copiedFence)), "Create Streamline copy fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Create Streamline fence event");
    createPipeline();
    query();
    runtime->active.store(true);
}
void SlPresenter::Impl::createPipeline() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1; parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1; rootDescription.pParameters = &parameter;
    ComPtr<ID3DBlob> blob, errors;
    graphicsCheck(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "Serialize SL blit root signature");
    graphicsCheck(runtime->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "Create SL blit root signature");
    const auto vertex = compile("vs", "vs_5_0"), pixel = compile("ps", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root.Get(); state.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    state.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto& blend = state.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    state.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    state.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    state.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    state.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    state.DepthStencilState.BackFace = state.DepthStencilState.FrontFace;
    state.SampleMask = UINT_MAX; state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; state.RasterizerState.DepthClipEnable = TRUE;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1; state.RTVFormats[0] = sampleFormat(description.Format); state.SampleDesc.Count = 1;
    graphicsCheck(runtime->device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&pipeline)), "Create SL HUDless conversion pipeline");
}
bool SlPresenter::Impl::completed(ID3D12Fence* fence, uint64_t value) {
    if (!value) return true;
    if (!fence) throw std::runtime_error("A Streamline retirement value has no fence");
    graphicsCheck(runtime->device->GetDeviceRemovedReason(), "Streamline device health");
    const auto completedValue = fence->GetCompletedValue();
    if (completedValue == UINT64_MAX) throw std::runtime_error("Streamline fence reports device removal");
    return completedValue >= value;
}
void SlPresenter::Impl::wait(ID3D12Fence* fence, uint64_t value) {
    if (completed(fence, value)) return;
    graphicsCheck(fence->SetEventOnCompletion(value, event), "Arm Streamline retirement fence");
    const auto start = GetTickCount64();
    while (!completed(fence, value)) {
        const auto elapsed = GetTickCount64() - start;
        if (elapsed >= detail::slTimeoutMilliseconds) throw std::runtime_error("Streamline GPU retirement timed out");
        const auto result = WaitForSingleObject(event, static_cast<DWORD>(detail::slTimeoutMilliseconds - elapsed));
        if (result != WAIT_OBJECT_0) throw std::runtime_error("Streamline GPU retirement wait failed or timed out");
        // A timed-out registration can leave an older auto-reset event signal.
        // Only the queried fence and device health prove this target completed.
    }
}
void SlPresenter::Impl::retire(Slot& slot) {
    wait(copiedFence.Get(), slot.copied);
    slot.producer.reset();
    wait(slot.consumedFence.Get(), slot.consumed);
    if (slot.tagged && slot.ticket) runtime->clearTags(*slot.ticket);
    slot.tagged = false; slot.ticket.reset(); slot.consumedFence.Reset(); slot.consumed = 0; slot.copied = 0;
}
void SlPresenter::Impl::drain() {
    for (auto& slot : slots) retire(slot);
    if (queue && copiedFence) {
        graphicsCheck(queue->Signal(copiedFence.Get(), ++nextFence), "Signal Streamline queue drain");
        wait(copiedFence.Get(), nextFence);
    }
}
void SlPresenter::Impl::options(SlGenerationMode mode, const SlGenerationRequest& request) {
    sl::DLSSGOptions value{};
    value.mode = mode == SlGenerationMode::Fixed ? sl::DLSSGMode::eOn : mode == SlGenerationMode::Dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOff;
    value.numFramesToGenerate = mode == SlGenerationMode::Fixed ? request.generatedFrames : 1;
    value.dynamicTargetFrameRate = mode == SlGenerationMode::Dynamic ? request.dynamicTargetFrameRate : 0;
    if (optionsConfigured && observation.active == mode && configuredGeneratedFrames == value.numFramesToGenerate &&
        configuredDynamicTarget == value.dynamicTargetFrameRate) return;
    value.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    value.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    value.enableUserInterfaceRecomposition = sl::eTrue; value.onErrorCallback = &onApiError;
    detail::slCheck(runtime->api.dlssOptions(sl::ViewportHandle(0u), value), "Configure DLSS frame generation");
    observation.active = mode;
    optionsConfigured = true; configuredGeneratedFrames = value.numFramesToGenerate;
    configuredDynamicTarget = value.dynamicTargetFrameRate;
}
void SlPresenter::Impl::pause() {
    options(SlGenerationMode::Off);
    drain(); forceReset = true;
}
sl::DLSSGState SlPresenter::Impl::query() {
    sl::DLSSGState state{};
    detail::slCheck(runtime->api.dlssState(sl::ViewportHandle(0u), state, nullptr), "Query DLSS FG state on presentation thread");
    observation.maximumGeneratedFrames = state.numFramesToGenerateMax;
    observation.minimumDimension = state.minWidthOrHeight;
    observation.dynamicSupported = state.bIsDynamicMFGSupported == sl::eTrue;
    observation.vsyncSupported = state.bIsVsyncSupportAvailable == sl::eTrue;
    observation.sdkStatus = static_cast<uint32_t>(state.status);
    observation.asynchronousPresentError = asynchronousError.load();
    return state;
}
void SlPresenter::Impl::ensure(Image& destination, const D3D12_RESOURCE_DESC& input) {
    auto target = input;
    target.Format = sampleFormat(target.Format); target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    target.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; target.Alignment = 0;
    if (destination.resource) {
        const auto old = destination.resource->GetDesc();
        if (old.Width == target.Width && old.Height == target.Height && old.Format == target.Format) return;
    }
    destination.resource.Reset(); destination.state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    graphicsCheck(runtime->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target, destination.state,
                   nullptr, IID_PPV_ARGS(&destination.resource)), "Create private Streamline input");
}
void SlPresenter::Impl::prepare(Slot& slot, const PresentationFrame& frame, bool generate) {
    if (!slot.allocator) {
        graphicsCheck(runtime->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)), "Create SL input allocator");
        graphicsCheck(runtime->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                       IID_PPV_ARGS(&slot.list)), "Create SL input command list");
        graphicsCheck(slot.list->Close(), "Close fresh SL command list");
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.NumDescriptors = 1; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        graphicsCheck(runtime->device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.srv)), "Create SL HUDless SRV heap");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        graphicsCheck(runtime->device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.rtv)), "Create SL HUDless RTV heap");
    }
    const auto& images = *frame.images;
    ensure(slot.finalColor, images.finalColor.resource->GetDesc());
    if (!generate) return;
    ensure(slot.rawHudless, images.hudless.resource->GetDesc());
    auto color = images.hudless.resource->GetDesc(); color.Format = description.Format;
    ensure(slot.hudless, color);
    ensure(slot.depth, images.depth.resource->GetDesc()); ensure(slot.motion, images.motion.resource->GetDesc());
    ensure(slot.alpha, images.uiInfluence.resource->GetDesc());
    if (images.slDistortion.resource) ensure(slot.distortion, images.slDistortion.resource->GetDesc());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = sampleFormat(images.hudless.resource->GetDesc().Format); srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    runtime->device->CreateShaderResourceView(slot.rawHudless.resource.Get(), &srv, slot.srv->GetCPUDescriptorHandleForHeapStart());
    runtime->device->CreateRenderTargetView(slot.hudless.resource.Get(), nullptr, slot.rtv->GetCPUDescriptorHandleForHeapStart());
}
void SlPresenter::Impl::copy(Slot& slot, Image& destination, const PresentImage& source) {
    destination.transition(slot.list.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    Dx11Dx12::transition(slot.list.Get(), source.resource.Get(), source.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    slot.list->CopyResource(destination.resource.Get(), source.resource.Get());
    Dx11Dx12::transition(slot.list.Get(), source.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source.state);
}
void SlPresenter::Impl::copyFrame(Slot& slot, const PresentationFrame& frame, bool generate) {
    graphicsCheck(slot.allocator->Reset(), "Reset retired SL allocator");
    graphicsCheck(slot.list->Reset(slot.allocator.Get(), nullptr), "Reset retired SL command list");
    const auto& images = *frame.images;
    copy(slot, slot.finalColor, images.finalColor);
    slot.finalColor.transition(slot.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (generate) {
        copy(slot, slot.rawHudless, images.hudless); copy(slot, slot.depth, images.depth);
        copy(slot, slot.motion, images.motion); copy(slot, slot.alpha, images.uiInfluence);
        if (images.slDistortion.resource) copy(slot, slot.distortion, images.slDistortion);
        slot.rawHudless.transition(slot.list.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        slot.hudless.transition(slot.list.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto rtv = slot.rtv->GetCPUDescriptorHandleForHeapStart();
        slot.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {slot.srv.Get()}; slot.list->SetDescriptorHeaps(1, heaps);
        slot.list->SetGraphicsRootSignature(root.Get()); slot.list->SetPipelineState(pipeline.Get());
        slot.list->SetGraphicsRootDescriptorTable(0, slot.srv->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(description.Width), static_cast<LONG>(description.Height)};
        slot.list->RSSetViewports(1, &viewport); slot.list->RSSetScissorRects(1, &scissor);
        slot.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); slot.list->DrawInstanced(3, 1, 0, 0);
        for (auto* input : {&slot.hudless, &slot.depth, &slot.motion, &slot.alpha}) input->transition(slot.list.Get(), D3D12_RESOURCE_STATE_COMMON);
        if (images.slDistortion.resource) slot.distortion.transition(slot.list.Get(), D3D12_RESOURCE_STATE_COMMON);
    }
    // These two calls MUST go through the proxy, which exposes the SDK's client
    // backbuffer rather than whichever real OS buffer its pacer is presenting.
    const auto index = chain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backbuffer;
    graphicsCheck(chain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)), "Get proxy-owned Streamline backbuffer");
    const auto backDescription = backbuffer->GetDesc();
    if (backDescription.Width != description.Width || backDescription.Height != description.Height ||
        sampleFormat(backDescription.Format) != sampleFormat(description.Format))
        throw std::runtime_error("Streamline proxy backbuffer does not match the presentation generation");
    Dx11Dx12::transition(slot.list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    slot.list->CopyResource(backbuffer.Get(), slot.finalColor.resource.Get());
    Dx11Dx12::transition(slot.list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    graphicsCheck(slot.list->Close(), "Close Streamline input copies");
    if (frame.readyValue) graphicsCheck(queue->Wait(frame.readyFence.Get(), frame.readyValue), "Wait for immutable production frame");
    ID3D12CommandList* lists[] = {slot.list.Get()}; queue->ExecuteCommandLists(1, lists);
    slot.copied = ++nextFence;
    graphicsCheck(queue->Signal(copiedFence.Get(), slot.copied), "Publish Streamline source-copy retirement");
}
void SlPresenter::Impl::tag(Slot& slot, const PresentationFrame& frame) {
    sl::Constants constants{};
    constants.cameraViewToClip = matrix(frame.cameraViewToClip); constants.clipToCameraView = matrix(frame.clipToCameraView);
    constants.clipToPrevClip = matrix(frame.clipToPrevClip); constants.prevClipToClip = matrix(frame.prevClipToClip);
    constants.jitterOffset = {frame.jitterX, frame.jitterY};
    constants.cameraPinholeOffset = {0, 0}; // This adapter supplies its full projection matrix, not an additional pinhole shift.
    const auto motion = slot.motion.resource->GetDesc();
    constants.mvecScale = {frame.motionScaleX / static_cast<float>(motion.Width), frame.motionScaleY / motion.Height};
    constants.cameraPos = vector(frame.cameraPosition); constants.cameraUp = vector(frame.cameraUp);
    constants.cameraRight = vector(frame.cameraRight); constants.cameraFwd = vector(frame.cameraForward);
    constants.cameraNear = frame.cameraNear; constants.cameraFar = frame.cameraFar; constants.cameraFOV = frame.verticalFov;
    constants.cameraAspectRatio = static_cast<float>(description.Width) / description.Height;
    constants.depthInverted = boolean(frame.depthInverted); constants.cameraMotionIncluded = boolean(frame.cameraMotionIncluded);
    constants.motionVectors3D = sl::eFalse; constants.reset = boolean(frame.reset || forceReset || !haveFrame || frame.applicationFrameId != lastApplicationFrame + 1);
    constants.orthographicProjection = sl::eFalse; constants.motionVectorsDilated = sl::eFalse;
    constants.motionVectorsJittered = boolean(frame.motionVectorsJittered);
    detail::slCheck(runtime->api.constants(constants, *slot.ticket->token, sl::ViewportHandle(0u)), "Set exact application-frame constants");
    sl::Resource resources[] = {
        {sl::ResourceType::eTex2d, slot.depth.resource.Get(), static_cast<uint32_t>(slot.depth.state)},
        {sl::ResourceType::eTex2d, slot.motion.resource.Get(), static_cast<uint32_t>(slot.motion.state)},
        {sl::ResourceType::eTex2d, slot.hudless.resource.Get(), static_cast<uint32_t>(slot.hudless.state)},
        {sl::ResourceType::eTex2d, slot.alpha.resource.Get(), static_cast<uint32_t>(slot.alpha.state)},
        {sl::ResourceType::eTex2d, slot.distortion.resource.Get(), static_cast<uint32_t>(slot.distortion.state)}};
    const auto depth = slot.depth.resource->GetDesc();
    const sl::Extent low{0, 0, static_cast<uint32_t>(depth.Width), depth.Height};
    const sl::Extent full{0, 0, description.Width, description.Height};
    sl::Extent area = full;
    const auto& rectangle = frame.generationRect;
    if (rectangle.right > rectangle.left && rectangle.bottom > rectangle.top)
        area = {static_cast<uint32_t>(rectangle.top), static_cast<uint32_t>(rectangle.left),
                static_cast<uint32_t>(rectangle.right - rectangle.left), static_cast<uint32_t>(rectangle.bottom - rectangle.top)};
    sl::Extent distortion{};
    if (frame.images->slDistortion.resource) {
        const auto distortionDescription = slot.distortion.resource->GetDesc();
        distortion = {0, 0, static_cast<uint32_t>(distortionDescription.Width), distortionDescription.Height};
    }
    sl::ResourceTag tags[] = {
        {&resources[0], sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &low},
        {&resources[1], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &low},
        {&resources[2], sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &full},
        {&resources[3], sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &full},
        {nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent},
        {frame.images->slDistortion.resource ? &resources[4] : nullptr, sl::kBufferTypeBidirectionalDistortionField, sl::ResourceLifecycle::eValidUntilPresent, &distortion},
        {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &area}};
    // Producer and private-input lifetimes are different: tags own private
    // native resources until the SDK's inputsProcessingCompletionFence retires.
    slot.tagged = true;
    detail::slCheck(runtime->api.tags(*slot.ticket->token, sl::ViewportHandle(0u), tags, static_cast<uint32_t>(std::size(tags)), nullptr), "Tag private native Streamline frame inputs");
}
bool SlPresenter::Impl::eligible(const PresentationFrame& frame, const PresentArguments& arguments,
                                  const SlGenerationRequest& request, const std::shared_ptr<detail::SlTicket>& ticket) {
    auto reject = [this](const char* reason) { observation.reason = reason; return false; };
    if (request.mode == SlGenerationMode::Off) { observation.reason = "DLSS frame generation is off"; return false; }
    if (!ticket || !ticket->generationReady) return reject("The matching before-input ticket and required PCL phases are missing");
    if (runtime->reflexMode == SlReflexMode::Off) return reject("DLSS FG requires Reflex enabled; its before-input sleep is still maintained");
    if (!frame.inputsComplete || frame.generation != creation.generation || !frame.renderWidth || !frame.renderHeight)
        return reject("The immutable frame is incomplete or belongs to another presentation generation");
    if (request.mode == SlGenerationMode::Fixed) {
        if (!request.generatedFrames || request.generatedFrames > observation.maximumGeneratedFrames)
            return reject("The requested additional-frame count exceeds this adapter/runtime's reported capability");
    } else if (request.mode == SlGenerationMode::Dynamic) {
        if (!observation.dynamicSupported) return reject("Dynamic MFG is unsupported by this adapter/runtime");
        if (!std::isfinite(request.dynamicTargetFrameRate) || request.dynamicTargetFrameRate < 0)
            return reject("Dynamic MFG target must be zero (monitor) or a finite positive frame rate");
    } else return reject("Unknown DLSS generation mode");
    if (arguments.syncInterval && (!observation.vsyncSupported || arguments.syncInterval > 1))
        return reject("This DLSS FG mode cannot preserve the requested VSync interval");
    if (description.Width < observation.minimumDimension || description.Height < observation.minimumDimension)
        return reject("The output is below this runtime's minimum FG dimension");
    if (description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
        return reject("DLSS FG does not support an FP16/scRGB swapchain; color encoding is not silently changed");
    if (frame.colorSpace != colorSpace) return reject("Frame and swapchain color encodings differ");
    if ((colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 && colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ||
        (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && description.Format != DXGI_FORMAT_R10G10B10A2_UNORM))
        return reject("This output color-space/format combination is not a supported DLSS FG encoding");
    const auto& images = *frame.images;
    if (!sizeIs(images.hudless, description.Width, description.Height) || !sizeIs(images.uiInfluence, description.Width, description.Height) ||
        !sizeIs(images.depth, frame.renderWidth, frame.renderHeight) || !sizeIs(images.motion, frame.renderWidth, frame.renderHeight))
        return reject("HUDless/UI influence or depth/MV domains do not match the immutable frame");
    const auto alpha = sampleFormat(images.uiInfluence.resource->GetDesc().Format);
    if (alpha != DXGI_FORMAT_R8_UNORM && alpha != DXGI_FORMAT_R16_FLOAT && alpha != DXGI_FORMAT_R32_FLOAT)
        return reject("SL UI influence must be a single-channel alpha image");
    if (images.slDistortion.resource && (!texture(images.slDistortion) ||
        (sampleFormat(images.slDistortion.resource->GetDesc().Format) != DXGI_FORMAT_R16G16B16A16_FLOAT &&
         sampleFormat(images.slDistortion.resource->GetDesc().Format) != DXGI_FORMAT_R32G32B32A32_FLOAT)))
        return reject("SL distortion requires its own signed four-channel bidirectional field");
    const RECT empty{};
    const RECT full{0, 0, static_cast<LONG>(description.Width), static_cast<LONG>(description.Height)};
    const auto& area = frame.generationRect;
    if ((area.left != empty.left || area.top != empty.top || area.right != empty.right || area.bottom != empty.bottom) &&
        (area.left != full.left || area.top != full.top || area.right != full.right || area.bottom != full.bottom))
        return reject("A cropped SL viewport needs matching per-input extents; this frame contract supplies full-domain inputs");
    if (!frame.cameraMotionIncluded || !std::isfinite(frame.cameraNear) || frame.cameraNear <= 0 ||
        !std::isfinite(frame.cameraFar) || frame.cameraFar <= frame.cameraNear || !std::isfinite(frame.verticalFov) ||
        frame.verticalFov <= 0 || frame.verticalFov >= 3.141593f || !std::isfinite(frame.motionScaleX) || !std::isfinite(frame.motionScaleY) ||
        !std::isfinite(frame.jitterX) || !std::isfinite(frame.jitterY))
        return reject("SL camera constants or full camera-motion vectors are invalid");
    auto finite = [](const auto& values) { return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }); };
    if (!finite(frame.cameraViewToClip) || !finite(frame.clipToCameraView) || !finite(frame.clipToPrevClip) || !finite(frame.prevClipToClip) ||
        !finite(frame.cameraPosition) || !finite(frame.cameraUp) || !finite(frame.cameraRight) || !finite(frame.cameraForward))
        return reject("SL camera matrices/basis contain non-finite values");
    observation.reason.clear();
    return true;
}
HRESULT SlPresenter::Impl::present(FrameLease frame, const PresentArguments& arguments, const SlGenerationRequest& request) {
    if (!usable()) return DXGI_ERROR_DEVICE_REMOVED;
    // TEST does not allocate/claim a token, change FG options or advance leases.
    if (arguments.flags & DXGI_PRESENT_TEST)
        return arguments.parameters ? chain->Present1(arguments.syncInterval, arguments.flags, arguments.parameters) : chain->Present(arguments.syncInterval, arguments.flags);
    if (FAILED(asynchronousError.load())) throw std::runtime_error("The Streamline asynchronous presentation callback reported failure");
    if (!frame || !frame->applicationFrameId || frame->generation != creation.generation ||
        (haveFrame && frame->applicationFrameId <= lastApplicationFrame) || !frame->images ||
        !sizeIs(frame->images->finalColor, description.Width, description.Height) ||
        sampleFormat(frame->images->finalColor.resource->GetDesc().Format) != sampleFormat(description.Format) ||
        (frame->readyValue && !frame->readyFence))
        return E_INVALIDARG;
    observation.requested = request.mode;
    auto ticket = runtime->claim(frame->applicationFrameId);
    bool generate = eligible(*frame, arguments, request, ticket);
    if (!generate) {
        // Pause is a transition, not a full queue/SDK drain on every menu frame.
        // It also must not be followed by a second SetOptions for this Present.
        if (!optionsConfigured || observation.active != SlGenerationMode::Off) pause();
        forceReset = true;
    }
    auto& slot = slots[nextSlot++ % slots.size()];
    retire(slot);
    prepare(slot, *frame, generate);
    slot.producer = frame; slot.ticket = std::move(ticket);
    copyFrame(slot, *frame, generate);
    if (generate) tag(slot, *frame);
    else if (slot.ticket) runtime->clearTags(*slot.ticket);
    const bool boundaryReady = !arguments.boundary || !arguments.boundary->before ||
        arguments.boundary->before(arguments.boundary->context, frame->applicationFrameId);
    if (generate && (!boundaryReady || !runtime->readyForPresent(slot.ticket))) {
        options(SlGenerationMode::Off);
        runtime->clearTags(*slot.ticket); slot.tagged = false; generate = false; forceReset = true;
        observation.reason = "Render submission end/Present start were not established at the actual boundary";
    } else if (generate) {
        // Options take effect at the next Present. Tagging alone does not enable
        // FG: commit On only after the real submission boundary is established.
        options(request.mode, request);
    }
    const auto result = arguments.parameters ? chain->Present1(arguments.syncInterval, arguments.flags, arguments.parameters) : chain->Present(arguments.syncInterval, arguments.flags);
    runtime->presented(slot.ticket);
    if (arguments.boundary && arguments.boundary->after)
        arguments.boundary->after(arguments.boundary->context, frame->applicationFrameId);
    ++observation.applicationPresents; observation.lastApplicationFrameId = frame->applicationFrameId;
    const auto state = query();
    observation.sdkReportedPresents += state.numFramesActuallyPresented;
    if (generate) {
        if (!state.inputsProcessingCompletionFence)
            throw std::runtime_error("DLSS FG did not provide its input-consumption fence; private inputs remain quarantined");
        slot.consumedFence = static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
        slot.consumed = state.lastPresentInputsProcessingCompletionFenceValue;
    }
    // Source inputs can be released independently of SDK-private input slots.
    for (auto& item : slots) if (item.producer && completed(copiedFence.Get(), item.copied)) item.producer.reset();
    haveFrame = true; lastApplicationFrame = frame->applicationFrameId;
    forceReset = !generate || FAILED(result) || state.status != sl::DLSSGStatus::eOk;
    if (generate && state.status != sl::DLSSGStatus::eOk) {
        observation.active = SlGenerationMode::Off;
        observation.reason = "DLSS FG reports status bits " + std::to_string(static_cast<uint32_t>(state.status));
    }
    if (FAILED(asynchronousError.load())) throw std::runtime_error("The Streamline asynchronous presenter failed while submitting this frame");
    if (FAILED(result) && result != DXGI_ERROR_WAS_STILL_DRAWING) graphicsCheck(result, "Present Streamline frame");
    return result;
}
PresentRetirement SlPresenter::Impl::stop() noexcept {
    if (stopped) return observation.quarantined ? PresentRetirement::Quarantined : PresentRetirement::Drained;
    if (runtime->quarantined) { observation.quarantined = true; return PresentRetirement::Quarantined; }
    try {
        const bool ownsRuntime = owned || runtime->presenterOwner == this;
        if (ownsRuntime) {
            runtime->stopFrameCalls();
            runtime->bounded([this] {
                if (runtime->api.dlssOptions) options(SlGenerationMode::Off);
                drain();
                // The proxy's final Release invokes eIDXGISwapChain_Destroyed.
                // Input fences alone do not prove retirement of SDK pacing or
                // swapchain threads; the entire destroy/unload path must return.
                nativeChain.Reset(); chain.Reset();
                for (auto& slot : slots) slot = Slot{};
                queue.Reset();
                runtime->deactivate(this);
            });
            owned = false;
        }
        auto* expected = &asynchronousError;
        errorTarget.compare_exchange_strong(expected, nullptr);
        stopped = true; observation.active = SlGenerationMode::Off;
        return PresentRetirement::Drained;
    } catch (const std::exception& exception) { quarantine(exception.what()); }
    catch (...) { quarantine("Unknown Streamline presenter retirement failure"); }
    return PresentRetirement::Quarantined;
}
SlPresenter::SlPresenter(const SlPresenterCreateInfo& info) {
    if (!info.runtime) throw SlPresenterCreationError("Streamline runtime is missing", PresentRetirement::Drained);
    impl_ = std::make_unique<Impl>(info, info.runtime->state_);
    try { impl_->initialize(); }
    catch (const std::exception& exception) {
        const std::string reason = exception.what();
        const auto retirement = impl_->stop();
        if (retirement == PresentRetirement::Quarantined) impl_.release();
        throw SlPresenterCreationError(reason, retirement);
    }
    catch (...) {
        const auto retirement = impl_->stop();
        if (retirement == PresentRetirement::Quarantined) impl_.release();
        throw SlPresenterCreationError("Unknown Streamline presenter construction failure", retirement);
    }
}
SlPresenter::~SlPresenter() { if (impl_ && impl_->stop() == PresentRetirement::Quarantined) impl_.release(); }
HRESULT SlPresenter::present(FrameLease frame, const PresentArguments& arguments, const SlGenerationRequest& request) noexcept {
    try { return impl_->present(std::move(frame), arguments, request); }
    catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown Streamline presentation failure"); }
    return DXGI_ERROR_DEVICE_REMOVED;
}
PresentRetirement SlPresenter::stop() noexcept { return impl_->stop(); }
HRESULT SlPresenter::resize(const DXGI_SWAP_CHAIN_DESC1& description, uint64_t generation) noexcept {
    if (!impl_->usable()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!description.Width || !description.Height || description.SampleDesc.Count != 1 || description.Stereo || description.BufferCount < 2 ||
        description.SwapEffect != impl_->description.SwapEffect || description.Flags != impl_->description.Flags) return E_INVALIDARG;
    try {
        impl_->pause();
        // Keep the actual DXGI/Win32 control on the caller's render/UI boundary.
        const auto result = impl_->chain->ResizeBuffers(description.BufferCount, description.Width, description.Height, description.Format, description.Flags);
        if (FAILED(result)) return result;
        impl_->description = description; impl_->creation.generation = generation;
        impl_->pipeline.Reset(); impl_->root.Reset(); impl_->createPipeline();
        for (auto& slot : impl_->slots) slot = Slot{};
        impl_->query();
        return S_OK;
    } catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown Streamline resize failure"); }
    return DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setFullscreenState(BOOL fullscreen, IDXGIOutput* output) noexcept {
    if (!impl_->usable()) return DXGI_ERROR_DEVICE_REMOVED;
    try { impl_->pause(); return impl_->chain->SetFullscreenState(fullscreen, output); }
    catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown Streamline fullscreen failure"); }
    return DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::resizeTarget(const DXGI_MODE_DESC& description) noexcept {
    if (!impl_->usable()) return DXGI_ERROR_DEVICE_REMOVED;
    try { impl_->pause(); return impl_->nativeChain->ResizeTarget(&description); }
    catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown Streamline target resize failure"); }
    return DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept {
    if (!impl_->usable()) return DXGI_ERROR_DEVICE_REMOVED;
    try {
        impl_->pause(); const auto result = impl_->nativeChain->SetColorSpace1(colorSpace);
        if (SUCCEEDED(result)) impl_->colorSpace = colorSpace;
        return result;
    } catch (const std::exception& exception) { impl_->quarantine(exception.what()); }
    catch (...) { impl_->quarantine("Unknown Streamline color-space failure"); }
    return DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setHdrMetadata(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) noexcept {
    return impl_->usable() ? impl_->nativeChain->SetHDRMetaData(type, size, data) : DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setMaximumFrameLatency(UINT latency) noexcept {
    if (!impl_->usable()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!(impl_->description.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) return DXGI_ERROR_INVALID_CALL;
    return impl_->nativeChain->SetMaximumFrameLatency(latency);
}
HRESULT SlPresenter::setSourceSize(UINT width, UINT height) noexcept {
    return impl_->usable() ? impl_->nativeChain->SetSourceSize(width, height) : DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setRotation(DXGI_MODE_ROTATION rotation) noexcept {
    return impl_->usable() ? impl_->nativeChain->SetRotation(rotation) : DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setMatrixTransform(const DXGI_MATRIX_3X2_F& matrix) noexcept {
    return impl_->usable() ? impl_->nativeChain->SetMatrixTransform(&matrix) : DXGI_ERROR_DEVICE_REMOVED;
}
HRESULT SlPresenter::setBackgroundColor(const DXGI_RGBA& color) noexcept {
    return impl_->usable() ? impl_->nativeChain->SetBackgroundColor(&color) : DXGI_ERROR_DEVICE_REMOVED;
}
IDXGISwapChain4* SlPresenter::swapChainForQueries() const noexcept { return impl_->usable() ? impl_->nativeChain.Get() : nullptr; }
SlPresenterStatus SlPresenter::status() const {
    auto result = impl_->observation;
    result.asynchronousPresentError = impl_->asynchronousError.load();
    result.quarantined = impl_->runtime->quarantined;
    return result;
}
} // namespace dspaa
