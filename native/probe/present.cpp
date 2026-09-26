#include "graphics/dx11-dx12.h"
#include "present/channel.h"
#include "present/facade.h"
#include "present/transport.h"
#include "present/world-color.h"
#include <DirectXMath.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <d3d12sdklayers.h>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void check(HRESULT value, const char* message) {
    dspaa::graphicsCheck(value, message);
}
void report(const char* message) noexcept {
    std::fprintf(stderr, "facade: %s\n", message);
}
struct Window {
    HWND value = nullptr;
    Window() {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = DefWindowProcW;
        type.lpszClassName = L"DSPAASR hidden bridge probe";
        require(RegisterClassW(&type) != 0, "register probe window");
        value = CreateWindowExW(0, type.lpszClassName, type.lpszClassName, WS_POPUP, 0, 0, 640, 360, nullptr,
                                nullptr, type.hInstance, nullptr);
        require(value != nullptr, "create invisible window");
    }
    ~Window() {
        if (value)
            DestroyWindow(value);
    }
};
unsigned errors(ID3D12InfoQueue* queue) {
    unsigned count = 0;
    if (!queue)
        return count;
    for (uint64_t i = 0; i < queue->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "debug message size");
        std::vector<unsigned char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        check(queue->GetMessage(i, message, &size), "debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
            ++count;
        }
    }
    return count;
}
dspaa::FencePoint readyPoint(const dspaa::FrameLease& frame) {
    return {frame->readyFence, frame->readyValue};
}
uint32_t readPixel12(const std::shared_ptr<dspaa::Dx11Dx12>& bridge, ID3D12CommandQueue* queue,
                     const dspaa::PresentImage& image, const dspaa::FencePoint& ready, unsigned x = 0,
                     unsigned y = 0) {
    require(!ready.unpublished(), "readback received an unpublished producer ticket");
    struct ReadbackJob {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> consumerQueue;
        dspaa::PresentImage sourceImage;
        dspaa::FencePoint producerReady;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> consumed;
    };
    auto job = std::make_unique<ReadbackJob>();
    job->device = bridge->device12();
    job->consumerQueue = queue;
    job->sourceImage = image;
    job->producerReady = ready;
    const auto desc = image.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    bridge->device12()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto& readback = job->readback;
    check(bridge->device12()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&readback)),
          "producer readback");
    auto& allocator = job->allocator;
    auto& list = job->list;
    check(
        bridge->device12()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "producer readback allocator");
    check(bridge->device12()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&list)),
          "producer readback list");
    dspaa::Dx11Dx12::transition(list.Get(), image.resource.Get(), image.state,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = image.resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    dspaa::Dx11Dx12::transition(list.Get(), image.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                image.state);
    check(list->Close(), "close producer readback");
    auto& consumed = job->consumed;
    check(bridge->device12()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumed)),
          "readback consumer fence");
    bool submitted = false;
    try {
        if (ready.value)
            check(queue->Wait(ready.fence.Get(), ready.value), "readback waits for the actual producer");
        ID3D12CommandList* lists[]{list.Get()};
        submitted = true;
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(consumed.Get(), 1), "publish independent readback completion");
        bridge->wait({consumed, 1});
    } catch (...) {
        // The independent queue is not covered by a later bridge drain. A failed
        // proof retains the entire submitted job until this failing probe exits.
        if (submitted)
            (void)job.release();
        throw;
    }
    void* data = nullptr;
    const SIZE_T offset =
        static_cast<SIZE_T>(footprint.Offset) + static_cast<SIZE_T>(y) * footprint.Footprint.RowPitch + x * 4;
    D3D12_RANGE read{offset, offset + 4};
    check(readback->Map(0, &read, &data), "map producer readback");
    uint32_t value;
    std::memcpy(&value, static_cast<const unsigned char*>(data) + offset, 4);
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    return value;
}
void worldSnapshots(const std::shared_ptr<dspaa::Dx11Dx12>& bridge) {
    auto logical = bridge->texture(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto source = bridge->texture(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    ComPtr<ID3D11RenderTargetView> target, logicalTarget;
    check(bridge->device11()->CreateRenderTargetView(source.dx11.Get(), nullptr, &target),
          "world snapshot RTV");
    check(bridge->device11()->CreateRenderTargetView(logical.dx11.Get(), nullptr, &logicalTarget),
          "logical presentation RTV");
    const float staleColor[]{0, 0, 1, 1};
    bridge->context11()->ClearRenderTargetView(logicalTarget.Get(), staleColor);
    auto bind = [&](ID3D11RenderTargetView* view) {
        bridge->context11()->OMSetRenderTargets(view ? 1 : 0, view ? &view : nullptr, nullptr);
    };
    bind(target.Get());
    dspaa::WorldColor world(bridge);
    world.surface(logical.dx11.Get(), 7);
    auto fill = [&](float red, float green) {
        const float color[]{red, green, 0, 1};
        bridge->context11()->ClearRenderTargetView(target.Get(), color);
    };
    auto packet = [](uint64_t id, uint64_t generation = 7) {
        auto value = std::make_unique<dspaa::PresentationSubmission>();
        value->metadata.applicationFrameId = id;
        value->metadata.generation = generation;
        value->metadata.flags = 1;
        return value;
    };
    auto pixel = [&](ID3D11Resource* resource) {
        ComPtr<ID3D11Texture2D> texture;
        check(resource->QueryInterface(IID_PPV_ARGS(&texture)), "snapshot texture");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(bridge->device11()->CreateTexture2D(&desc, nullptr, &staging), "snapshot readback");
        bridge->context11()->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(bridge->context11()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "snapshot map");
        uint32_t result;
        std::memcpy(&result, mapped.pData, sizeof(result));
        bridge->context11()->Unmap(staging.Get(), 0);
        return result;
    };
    fill(1, 0);
    world.capture(1);
    fill(0, 1);
    std::array<std::unique_ptr<dspaa::PresentationSubmission>, 4> held;
    held[0] = packet(1);
    world.attach(*held[0]);
    require(held[0]->worldLifetime && pixel(held[0]->resources[0].Get()) == 0xff0000ffu,
            "world snapshot copied the stale logical surface instead of the actual world target, or lost its "
            "pixels");
    world.capture(1);
    auto duplicate = packet(1);
    world.attach(*duplicate);
    require(!(duplicate->metadata.flags & 1u), "duplicate camera draw replaced the same-frame snapshot");
    for (unsigned index = 1; index < held.size(); ++index) {
        world.capture(index + 1);
        held[index] = packet(index + 1);
        world.attach(*held[index]);
        require(held[index]->worldLifetime != nullptr, "world pool failed before its real leases filled");
    }
    world.capture(5);
    auto blocked = packet(5);
    world.attach(*blocked);
    require(!(blocked->metadata.flags & 1u) && pixel(held[0]->resources[0].Get()) == 0xff0000ffu,
            "world snapshot pool overwrote an in-flight lease");
    held[0].reset();
    world.capture(6);
    auto resumed = packet(6);
    world.attach(*resumed);
    require(resumed->worldLifetime && pixel(resumed->resources[0].Get()) == 0xff00ff00u,
            "retired world snapshot slot could not be reused");
    bridge->drain();
    world.surface(logical.dx11.Get(), 8);
    world.capture(7);
    auto stale = packet(7);
    world.attach(*stale);
    require(!(stale->metadata.flags & 1u), "world snapshot escaped its surface generation");
    auto current = packet(7, 8);
    world.attach(*current);
    require(current->worldLifetime != nullptr, "stale envelope consumed the current surface snapshot");
    bind(nullptr);
    world.capture(8);
    auto unbound = packet(8, 8);
    world.attach(*unbound);
    require(!(unbound->metadata.flags & 1u), "missing world target reused a previous or logical snapshot");
    auto undersized = bridge->texture(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    ComPtr<ID3D11RenderTargetView> smallTarget;
    check(bridge->device11()->CreateRenderTargetView(undersized.dx11.Get(), nullptr, &smallTarget),
          "mismatched world target");
    bind(smallTarget.Get());
    world.capture(9);
    auto mismatched = packet(9, 8);
    world.attach(*mismatched);
    require(!(mismatched->metadata.flags & 1u), "wrong-size world target entered a full-display envelope");
    auto logicalEnvelope = packet(9, 8);
    logicalEnvelope->metadata.logicalOutputWidth = logicalEnvelope->metadata.logicalOutputHeight = 4;
    world.attach(*logicalEnvelope);
    require(logicalEnvelope->worldLifetime != nullptr,
            "valid logical-size world target was rejected merely because the display is larger");
    bind(target.Get());
    world.capture(10);
    auto restored = packet(10, 8);
    world.attach(*restored);
    require(restored->worldLifetime && pixel(restored->resources[0].Get()) == 0xff00ff00u,
            "valid world target did not resume after missing/mismatched bindings");
    world.releaseSurfaceReference();
    world.capture(11);
    auto closed = packet(11, 8);
    world.attach(*closed);
    require(!(closed->metadata.flags & 1u) && pixel(current->resources[0].Get()) == 0xff00ff00u,
            "closed world producer submitted another copy or discarded a consumer lease");
    world.surface(nullptr, 0);
    bind(nullptr);
    bridge->drain();
    std::puts("world_snapshot: actual OM target, exact pre-UI pixels, missing/size/duplicate/stale "
              "rejection, held-slot backpressure and reuse passed");
}

void unidirectionalTransport(const std::shared_ptr<dspaa::Dx11Dx12>& bridge) {
    bridge->drain();
    auto* context = bridge->context11();
    auto final = bridge->texture(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto depth = bridge->texture(8, 8, DXGI_FORMAT_R32_FLOAT, false);
    auto motion = bridge->texture(8, 8, DXGI_FORMAT_R16G16_FLOAT, false);
    ComPtr<ID3D11RenderTargetView> target;
    check(bridge->device11()->CreateRenderTargetView(final.dx11.Get(), nullptr, &target),
          "direct producer target");
    const float red[]{1, 0, 0, 1}, green[]{0, 1, 0, 1};
    context->ClearRenderTargetView(target.Get(), red);
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> consumer;
    check(bridge->device12()->CreateCommandQueue(&desc, IID_PPV_ARGS(&consumer)),
          "independent producer consumer queue");
    ComPtr<ID3D12Fence> gate12, gate11;
    check(bridge->device12()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate12)),
          "independent D3D12 queue gate");
    check(bridge->device12()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate11)),
          "shared D3D11 producer gate");
    ComPtr<ID3D11Device5> fenceDevice;
    check(bridge->device11()->QueryInterface(IID_PPV_ARGS(&fenceDevice)), "D3D11 fence interface");
    HANDLE shared = nullptr;
    check(bridge->device12()->CreateSharedHandle(gate11.Get(), nullptr, GENERIC_ALL, nullptr, &shared),
          "share D3D11 producer gate");
    ComPtr<ID3D11Fence> gateOn11;
    const auto opened = fenceDevice->OpenSharedFence(shared, IID_PPV_ARGS(&gateOn11));
    CloseHandle(shared);
    check(opened, "open D3D11 producer gate");
    dspaa::PresentationTransport transport(bridge), pendingTransport(bridge);
    dspaa::WorldColor world(bridge);
    world.surface(final.dx11.Get(), 7);
    std::array<dspaa::FrameLease, 4> held;
    std::array<dspaa::FrameLease, 3> later;
    std::array<std::unique_ptr<dspaa::PresentationSubmission>, 3> heldWorld;
    std::string reason;
    check(bridge->queue12()->Wait(gate12.Get(), 1), "block unrelated D3D12 work");
    try {
        for (auto& frame : held)
            frame =
                transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, {}, reason);
        bool rejected = false;
        try {
            (void)transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, {}, reason);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "producer overwrote a leased transport slot behind the queue gate");
        // A separate real consumer waits on the published ticket and reads pixels
        // while the bridge queue is still gated. This detects a hidden middle hop.
        require(readPixel12(bridge, consumer.Get(), held.front()->images->finalColor,
                            readyPoint(held.front())) == 0xff0000ffu,
                "pure D3D11 publication depended on the blocked bridge queue or lost pixels");
        for (const auto& frame : held)
            bridge->wait(readyPoint(frame));
        held = {};
        auto resumed =
            transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, {}, reason);
        bridge->wait(readyPoint(resumed));
        resumed.reset();
        check(gate12->Signal(1), "release unrelated D3D12 queue gate");
        bridge->drain();

        check(context->Wait(gateOn11.Get(), 1), "block real D3D11 producer work");
        context->ClearRenderTargetView(target.Get(), green);
        auto* bound = target.Get();
        context->OMSetRenderTargets(1, &bound, nullptr);
        auto captureWorld = [&](uint64_t id) {
            auto input = std::make_unique<dspaa::PresentationSubmission>();
            input->metadata.applicationFrameId = id;
            input->metadata.generation = 7;
            input->metadata.flags = 1;
            input->metadata.renderWidth = input->metadata.renderHeight = 8;
            input->resources[1] = depth.dx11;
            input->resources[2] = motion.dx11;
            world.capture(id);
            world.attach(*input);
            return input;
        };
        auto first = pendingTransport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                              captureWorld(1), reason);
        require(first->inputsComplete, "gated producer world inputs missing");
        const auto ready = readyPoint(first);
        const auto firstFinal = first->images->finalColor, firstWorld = first->images->hudless;
        require(!ready.complete(), "D3D11 producer ticket became ready before its writes");
        first.reset(); // Only the retirement tickets now protect this slot/world source.
        for (auto& frame : later) {
            frame = pendingTransport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, {},
                                             reason);
            require(frame->images->finalColor.resource.Get() != firstFinal.resource.Get() &&
                        !readyPoint(frame).complete(),
                    "CPU early-release reused an unfinished D3D11 producer slot");
        }
        for (size_t i = 0; i < heldWorld.size(); ++i) {
            heldWorld[i] = captureWorld(i + 2);
            require(heldWorld[i]->worldLifetime != nullptr, "gated world consumed extra pool slots");
        }
        auto blocked = captureWorld(5);
        require(!(blocked->metadata.flags & 1u), "unfinished producer world source was recycled");
        check(gate11->Signal(1), "release D3D11 producer gate");
        bridge->wait(ready);
        require(readPixel12(bridge, consumer.Get(), firstFinal, ready) == 0xff00ff00u &&
                    readPixel12(bridge, consumer.Get(), firstWorld, ready) == 0xff00ff00u,
                "D3D11 ready publication did not cover exact Final/world writes");
        auto recycled = pendingTransport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                                 {}, reason);
        require(recycled->images->finalColor.resource.Get() == firstFinal.resource.Get(),
                "completed producer slot could not be reused");
        auto reclaimed = captureWorld(6);
        require(reclaimed->worldLifetime &&
                    reclaimed->worldLifetime->texture.dx12.Get() == firstWorld.resource.Get(),
                "completed producer world source could not be reused");
        bridge->wait(readyPoint(recycled));
        later = {};
        heldWorld = {};
        recycled.reset();
        reclaimed.reset();
        context->OMSetRenderTargets(0, nullptr, nullptr);
        bridge->drain();
        pendingTransport.reset();
        world.surface(nullptr, 8);
        dspaa::FencePoint unpublished;
        unpublished.arm();
        require(!unpublished.complete(), "armed ticket was treated as complete");
        rejected = false;
        try {
            bridge->wait(unpublished);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "unpublished ticket was registered as a normal fence wait");
    } catch (...) {
        (void)gate12->Signal(1);
        (void)gate11->Signal(1);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        bridge->drain();
        throw;
    }
    std::puts("producer_queue_direction: independent consumer pixels behind D3D12 gate, pending D3D11 "
              "ready/early-release protection, four-slot reuse and unpublished-ticket rejection passed");
}

void unpredicatedTransport(const std::shared_ptr<dspaa::Dx11Dx12>& bridge) {
    auto* device = bridge->device11();
    auto* context = bridge->context11();
    auto final = bridge->texture(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto hudless = bridge->texture(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto depth = bridge->texture(8, 8, DXGI_FORMAT_R32_FLOAT, false);
    auto motion = bridge->texture(8, 8, DXGI_FORMAT_R16G16_FLOAT, false);
    auto control = bridge->texture(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto fill = [&](const dspaa::SharedTexture& image, std::array<float, 4> color) {
        ComPtr<ID3D11RenderTargetView> view;
        check(device->CreateRenderTargetView(image.dx11.Get(), nullptr, &view), "predicate fixture RTV");
        context->ClearRenderTargetView(view.Get(), color.data());
    };
    auto pixel11 = [&](ID3D11Texture2D* image) {
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "predicate control readback");
        context->CopyResource(staging.Get(), image);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "predicate control map");
        uint32_t value;
        std::memcpy(&value, mapped.pData, 4);
        context->Unmap(staging.Get(), 0);
        return value;
    };
    auto pixel12 = [&](const dspaa::FrameLease& frame, const dspaa::PresentImage& image, unsigned x = 0,
                       unsigned y = 0) {
        return readPixel12(bridge, bridge->queue12(), image, readyPoint(frame), x, y);
    };
    auto packet = [&](uint64_t id) {
        auto result = std::make_unique<dspaa::PresentationSubmission>();
        result->metadata.applicationFrameId = id;
        result->metadata.generation = 7;
        result->metadata.flags = 1;
        result->metadata.renderWidth = result->metadata.renderHeight = 8;
        result->resources[0] = hudless.dx11;
        result->resources[1] = depth.dx11;
        result->resources[2] = motion.dx11;
        return result;
    };
    dspaa::PresentationTransport transport(bridge);
    std::string reason;
    fill(final, {1, 0, 0, 1});
    fill(hudless, {1, 0, 0, 1});
    fill(depth, {.5f, 0, 0, 1});
    fill(motion, {0, 0, 0, 1});
    auto seed =
        transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, packet(1), reason);
    require(seed->inputsComplete && pixel12(seed, seed->images->finalColor) == 0xff0000ffu,
            "producer slot seed failed");
    seed.reset();
    fill(final, {0, 1, 0, 1});
    fill(hudless, {0, 0, 1, 1});
    fill(depth, {.25f, 0, 0, 1});
    fill(motion, {.125f, .25f, 0, 1});
    D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    ComPtr<ID3D11Predicate> predicate;
    check(device->CreatePredicate(&query, &predicate), "empty occlusion predicate");
    context->Begin(predicate.Get());
    context->End(predicate.Get());
    std::array<uint32_t, 2> controls{};
    for (unsigned value = 0; value < 2; ++value) {
        fill(control, {0, 0, 1, 1});
        context->SetPredication(predicate.Get(), value ? TRUE : FALSE);
        context->CopyResource(control.dx11.Get(), final.dx11.Get());
        context->SetPredication(nullptr, FALSE);
        controls[value] = pixel11(control.dx11.Get());
    }
    require((controls[0] == 0xffff0000u && controls[1] == 0xff00ff00u) ||
                (controls[1] == 0xffff0000u && controls[0] == 0xff00ff00u),
            "control did not prove conditional CopyResource suppression");
    const BOOL blocked = controls[0] == 0xffff0000u ? FALSE : TRUE;
    context->SetPredication(predicate.Get(), blocked);
    auto frame =
        transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, packet(2), reason);
    ComPtr<ID3D11Predicate> retained;
    BOOL retainedValue = FALSE;
    context->GetPredication(&retained, &retainedValue);
    require(retained.Get() == predicate.Get() && retainedValue == blocked,
            "producer did not restore the engine predicate");
    context->SetPredication(nullptr, FALSE);
    require(frame->inputsComplete && pixel12(frame, frame->images->finalColor) == 0xff00ff00u,
            "producer fence advanced but the engine predicate suppressed the real Final copy");
    require(pixel12(frame, frame->images->hudless) == 0xffff0000u &&
                pixel12(frame, frame->images->depth) == 0x3e800000u &&
                pixel12(frame, frame->images->motion) == 0x34003000u,
            "conditional predicate suppressed a temporal producer copy");
    context->SetPredication(predicate.Get(), blocked);
    bool rejected = false;
    try {
        (void)transport.capture(nullptr, 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, {}, reason);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    retained.Reset();
    context->GetPredication(&retained, &retainedValue);
    require(rejected && retained.Get() == predicate.Get() && retainedValue == blocked,
            "producer exception lost the engine predicate");
    context->SetPredication(nullptr, FALSE);
    // Shared display-domain conversion must preserve Final, honor local D/M,
    // reset on logical-layout changes without requiring a swapchain generation,
    // and filter sRGB storage in linear light only with captured-view evidence.
    auto logicalColor = bridge->texture(2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    const uint32_t pattern[]{0xff000000u, 0xffffffffu, 0xff000000u, 0xffffffffu};
    context->UpdateSubresource(logicalColor.dx11.Get(), 0, nullptr, pattern, 8, 0);
    D3D11_VIEWPORT displayViewport{0, 0, 8, 8, 0, 1};
    context->RSSetViewports(1, &displayViewport);
    auto scaled = packet(3);
    scaled->resources[0] = logicalColor.dx11;
    scaled->metadata.logicalOutputWidth = scaled->metadata.logicalOutputHeight = 2;
    scaled->hudlessSrgbView = true;
    auto gamma = transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                   std::move(scaled), reason);
    require(gamma->inputsComplete && gamma->reset && gamma->images->hudless.resource->GetDesc().Width == 8,
            "logical output was not normalized into the display or did not reset history");
    const auto filtered = pixel12(gamma, gamma->images->hudless, 3, 3) & 255u;
    require(filtered >= 164 && filtered <= 166,
            "sRGB display resampling interpolated encoded values instead of linear light");
    require(pixel12(gamma, gamma->images->finalColor) == 0xff00ff00u &&
                gamma->images->depth.resource->GetDesc().Width == 8 && gamma->renderWidth == 8,
            "display normalization rewrote Final or repacked local temporal inputs");
    gamma.reset();
    auto wide = bridge->texture(4, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    fill(wide, {1, 0, 0, 1});
    displayViewport = {0, 2, 8, 4, 0, 1};
    context->RSSetViewports(1, &displayViewport);
    auto letterbox = packet(4);
    letterbox->resources[0] = wide.dx11;
    letterbox->metadata.logicalOutputWidth = 4;
    letterbox->metadata.logicalOutputHeight = 2;
    auto boxed = transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                   std::move(letterbox), reason);
    require(boxed->inputsComplete && boxed->reset && boxed->generationRect.top == 2 &&
                boxed->generationRect.bottom == 6 &&
                pixel12(boxed, boxed->images->hudless, 0, 0) == 0xff000000u &&
                pixel12(boxed, boxed->images->hudless, 3, 3) == 0xff0000ffu,
            "content extent did not produce exact black bars and local world color");
    boxed.reset();
    auto explicitCanonical = packet(5);
    explicitCanonical->metadata.generationRect[0] = 1;
    explicitCanonical->metadata.generationRect[1] = 2;
    explicitCanonical->metadata.generationRect[2] = 7;
    explicitCanonical->metadata.generationRect[3] = 6;
    auto canonical = transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                       std::move(explicitCanonical), reason);
    require(canonical->inputsComplete && canonical->reset && canonical->generationRect.left == 1 &&
                pixel12(canonical, canonical->images->hudless, 0, 0) == 0xffff0000u,
            "explicit canonical H was guessed, resized or cleared outside its SDK rectangle");
    canonical.reset();
    auto invalidDomain = packet(6);
    invalidDomain->metadata.logicalOutputWidth = 4;
    invalidDomain->metadata.logicalOutputHeight = 2;
    auto invalid = transport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                     std::move(invalidDomain), reason);
    require(!invalid->inputsComplete && invalid->reset &&
                pixel12(invalid, invalid->images->finalColor) == 0xff00ff00u,
            "mismatched logical input generated a frame or suppressed genuine Final");
    bridge->drain();
    {
        dspaa::WorldColor world(bridge);
        world.surface(final.dx11.Get(), 7);
        dspaa::PresentationTransport sharedTransport(bridge);
        ComPtr<ID3D11RenderTargetView> worldTarget;
        check(device->CreateRenderTargetView(hudless.dx11.Get(), nullptr, &worldTarget),
              "shared world target");
        auto* bound = worldTarget.Get();
        context->OMSetRenderTargets(1, &bound, nullptr);
        auto fromWorld = [&](uint64_t id) {
            auto value = packet(id);
            value->resources[0].Reset();
            world.capture(id);
            world.attach(*value);
            return value;
        };
        auto input = fromWorld(1);
        require(input->worldLifetime != nullptr, "missing shared world lease");
        auto* snapshot = input->worldLifetime->texture.dx12.Get();
        auto direct = sharedTransport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                              std::move(input), reason);
        require(direct->inputsComplete && direct->images->hudless.resource.Get() == snapshot &&
                    pixel12(direct, direct->images->hudless) == 0xffff0000u,
                "shared world H was recopied or changed its captured bytes");
        direct.reset();
        auto explicitInput = fromWorld(2);
        snapshot = explicitInput->worldLifetime->texture.dx12.Get();
        explicitInput->resources[0] = final.dx11;
        auto fallback = sharedTransport.capture(final.dx11.Get(), 7, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                                std::move(explicitInput), reason);
        require(fallback->inputsComplete && fallback->images->hudless.resource.Get() != snapshot &&
                    pixel12(fallback, fallback->images->hudless) == 0xff00ff00u,
                "explicit H borrowed an unrelated world snapshot instead of its copy fallback");
        fallback.reset();
        bridge->drain();
        world.surface(final.dx11.Get(), 8);
        worldTarget.Reset();
        check(device->CreateRenderTargetView(wide.dx11.Get(), nullptr, &worldTarget),
              "pending logical world target");
        bound = worldTarget.Get();
        context->OMSetRenderTargets(1, &bound, nullptr);
        auto logicalPacket = [&](uint64_t id) {
            auto value = packet(id);
            value->metadata.generation = 8;
            value->metadata.logicalOutputWidth = 4;
            value->metadata.logicalOutputHeight = 2;
            value->metadata.generationRect[1] = 2;
            value->metadata.generationRect[2] = 8;
            value->metadata.generationRect[3] = 6;
            value->resources[0].Reset();
            world.capture(id);
            world.attach(*value);
            return value;
        };
        auto pendingInput = logicalPacket(3);
        require(pendingInput->worldLifetime != nullptr, "missing pending world lease");
        snapshot = pendingInput->worldLifetime->texture.dx12.Get();
        ComPtr<ID3D12Fence> gate;
        check(bridge->device12()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)),
              "world resample queue gate");
        std::array<std::unique_ptr<dspaa::PresentationSubmission>, 3> heldWorld;
        check(bridge->queue12()->Wait(gate.Get(), 1), "hold world resample in flight");
        try {
            auto queued =
                sharedTransport.capture(final.dx11.Get(), 8, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                        std::move(pendingInput), reason);
            require(queued->inputsComplete && queued->images->hudless.resource.Get() != snapshot,
                    "logical world did not enter the shared display-domain resample");
            const auto ready = readyPoint(queued);
            const auto normalized = queued->images->hudless;
            const auto normalizedFinal = queued->images->finalColor;
            require(!ready.complete(), "normalized ready ignored its gated D3D12 conversion");
            queued.reset(); // No backend or CPU frame keeps the source lease alive.
            std::array<dspaa::FrameLease, 3> pure;
            for (size_t i = 0; i < pure.size(); ++i) {
                auto purePacket = packet(20 + i);
                purePacket->metadata.generation = 8;
                pure[i] =
                    sharedTransport.capture(final.dx11.Get(), 8, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                            std::move(purePacket), reason);
                require(pure[i]->inputsComplete &&
                            pure[i]->images->finalColor.resource.Get() != normalizedFinal.resource.Get(),
                        "pure producer overwrote a still-pending normalized slot");
                bridge->wait(readyPoint(pure[i]));
            }
            require(!ready.complete(), "pure publication replaced normalized completion proof");
            for (size_t index = 0; index < heldWorld.size(); ++index) {
                heldWorld[index] = logicalPacket(4 + index);
                require(heldWorld[index]->worldLifetime != nullptr,
                        "pending world lease consumed more than one pool slot");
            }
            fill(wide, {0, 1, 0, 1});
            auto blockedWorld = logicalPacket(7);
            require(!(blockedWorld->metadata.flags & 1u),
                    "CPU early-release recycled a world source still read by the gated resample");
            check(gate->Signal(1), "release pending world resample");
            bridge->wait(ready);
            require(readPixel12(bridge, bridge->queue12(), normalized, ready, 3, 3) == 0xff0000ffu,
                    "pending world resample lost its original snapshot after CPU early-release");
            auto resumedWorld = logicalPacket(8);
            require(resumedWorld->worldLifetime &&
                        resumedWorld->worldLifetime->texture.dx12.Get() == snapshot &&
                        pixel11(resumedWorld->worldLifetime->texture.dx11.Get()) == 0xff00ff00u,
                    "world source was not reusable after proven resample completion");
            auto pureInput = packet(30);
            pureInput->metadata.generation = 8;
            auto pureReuse = sharedTransport.capture(
                final.dx11.Get(), 8, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, std::move(pureInput), reason);
            require(pureReuse->images->finalColor.resource.Get() == normalizedFinal.resource.Get(),
                    "retired normalized slot was not reused by pure publication");
            bridge->wait(readyPoint(pureReuse));
            pureReuse.reset();
            auto resumedImage = resumedWorld->worldLifetime;
            auto normalizedAgain =
                sharedTransport.capture(final.dx11.Get(), 8, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                        std::move(resumedWorld), reason);
            require(normalizedAgain->inputsComplete &&
                        normalizedAgain->images->finalColor.resource.Get() ==
                            normalizedFinal.resource.Get() &&
                        pixel12(normalizedAgain, normalizedAgain->images->hudless, 3, 3) == 0xff00ff00u,
                    "same-generation normalize/pure/normalize reuse lost allocator or pixel safety");
            normalizedAgain.reset();
            // A failed submission may have armed a source without successfully
            // publishing its completion ticket. Only a drained surface reset may
            // release that quarantine, never CPU-reference retirement alone.
            resumedImage->ready.arm();
            resumedImage.reset();
            auto failedWorld = logicalPacket(9);
            require(!(failedWorld->metadata.flags & 1u), "failed world-use ticket was silently recycled");
            bridge->drain();
            world.surface(final.dx11.Get(), 9);
            auto afterDrain = packet(10);
            afterDrain->metadata.generation = 9;
            afterDrain->metadata.logicalOutputWidth = 4;
            afterDrain->metadata.logicalOutputHeight = 2;
            afterDrain->resources[0].Reset();
            world.capture(10);
            world.attach(*afterDrain);
            require(afterDrain->worldLifetime != nullptr, "drained surface did not reclaim failed world use");
            bridge->drain();
        } catch (...) {
            (void)gate->Signal(1);
            bridge->drain();
            throw;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        bridge->drain();
    }
    std::puts("shared_world: exact direct H, explicit-input fallback, pending-resample early-release "
              "protection, normalize/pure/normalize reuse and failed-ticket quarantine passed");
    std::puts("display_domain: logical resize, sRGB filtering, letterbox, local D/M, canonical bypass and "
              "layout reset passed");
    std::puts("producer_predication: negative control, exact Final/H/depth/motion bytes and normal/exception "
              "state restoration passed");
}

void backendSequence(IDXGISwapChain4* chain, ID3D11Device* device, ID3D11DeviceContext* context, UINT flags) {
    auto channel = dspaa::presentationChannel();
    require(channel != nullptr, "missing facade mailbox");
    check(chain->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_UNKNOWN, flags), "SDK test surface");
    check(chain->SetMaximumFrameLatency(1), "host latency policy before SDK replacement");
    auto bridge = dspaa::acquireDx11Dx12(device);
    std::array<dspaa::SharedTexture, 2> inputs;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] = bridge->texture(1280, 720, formats[i], false);
        ComPtr<ID3D11RenderTargetView> target;
        check(device->CreateRenderTargetView(inputs[i].dx11.Get(), nullptr, &target), "temporal RTV");
        const float color[] = {i == 0 ? .5f : 0.f, 0, 0, 1};
        context->ClearRenderTargetView(target.Get(), color);
    }
    ComPtr<ID3D11Texture2D> final;
    check(chain->GetBuffer(0, IID_PPV_ARGS(&final)), "stable SDK-switch logical buffer");
    ComPtr<ID3D11RenderTargetView> target;
    check(device->CreateRenderTargetView(final.Get(), nullptr, &target), "Final RTV");
    HANDLE latency = chain->GetFrameLatencyWaitableObject();
    require(latency != nullptr, "SDK stable waitable handle");
    auto event = dspaa::presentationRenderEvent();
    for (uint32_t backend : {1u, 2u, 1u, 0u}) {
        if (backend == 2 && !(channel->status().flags & 4u)) {
            std::puts("SKIP bridge DLSS: hardware/runtime unsupported");
            continue;
        }
        DspAaPresentationConfiguration selection{
            sizeof(selection), DspAaPresentationAbiVersion, backend, 0, 1, 1};
        require(channel->configure(selection), "backend selection rejected");
        unsigned generated = 0;
        for (unsigned index = 0; index < 18; ++index) {
            require(WaitForSingleObject(latency, 30000) == WAIT_OBJECT_0,
                    "host latency lost during backend replacement");
            DspAaPresentationBegin begin{};
            require(channel->begin(begin), "before-input mailbox begin failed");
            channel->marker(begin.applicationFrameId, dspaa::SlMarker::SimulationEnd);
            event(1, reinterpret_cast<void*>(begin.applicationFrameId));
            DspAaPresentationInputs frame{};
            frame.size = sizeof(frame);
            frame.version = DspAaPresentationAbiVersion;
            frame.applicationFrameId = begin.applicationFrameId;
            frame.generation = begin.generation;
            frame.depth = inputs[0].dx11.Get();
            frame.motion = inputs[1].dx11.Get();
            auto* boundWorld = target.Get();
            context->OMSetRenderTargets(1, &boundWorld, nullptr);
            const float worldColor[] = {.25f, .5f, .75f, 1};
            context->ClearRenderTargetView(target.Get(), worldColor);
            if (index != 9)
                event(3, reinterpret_cast<void*>(begin.applicationFrameId));
            frame.renderWidth = 1280;
            frame.renderHeight = 720;
            frame.flags = 1u | 16u | (!index ? 2u : 0u);
            if (index == 8)
                frame.flags &= ~1u; // Incomplete real frame pauses generation, not presentation.
            frame.motionScaleX = frame.motionScaleY = 1;
            frame.milliseconds = 16.667f;
            frame.cameraNear = .1f;
            frame.cameraFar = 1000;
            frame.verticalFov = DirectX::XM_PI / 3;
            frame.preExposure = frame.viewSpaceToMeters = 1;
            frame.maxLuminance = 100;
            frame.cameraUp[1] = frame.cameraRight[0] = frame.cameraForward[2] = 1;
            auto store = [](float* out, DirectX::FXMMATRIX value) {
                DirectX::XMFLOAT4X4 m;
                DirectX::XMStoreFloat4x4(&m, value);
                std::memcpy(out, &m, sizeof(m));
            };
            const auto projection =
                DirectX::XMMatrixPerspectiveFovLH(frame.verticalFov, 1280.f / 720, .1f, 1000);
            store(frame.cameraViewToClip, projection);
            store(frame.clipToCameraView, DirectX::XMMatrixInverse(nullptr, projection));
            store(frame.clipToPrevClip, DirectX::XMMatrixIdentity());
            store(frame.prevClipToClip, DirectX::XMMatrixIdentity());
            const auto token = dspaa::queuePresentationInputs(&frame);
            require(token != nullptr, "end-frame queue rejected");
            event(2, token);
            const float color[] = {.6f, .2f, .1f, 1};
            const D3D11_RECT panel{160, 180, 640, 360};
            bridge->context11()->ClearView(target.Get(), color, &panel, 1);
            const auto before = channel->status().applicationPresents;
            check(chain->Present(0, DXGI_PRESENT_TEST), "TEST did not preserve pending frame");
            require(channel->status().applicationPresents == before, "TEST advanced application telemetry");
            check(chain->Present(0, 0), "SDK facade Present");
            const auto status = channel->status();
            if (status.activeBackend != backend)
                throw std::runtime_error(std::string("backend selection fell back: ") + status.message);
            require(status.lastPresentedFrameId == begin.applicationFrameId &&
                        status.applicationPresents == before + 1,
                    "Present did not consume the matching end-of-frame identity");
            if (index == 8 || index == 9)
                require(!(status.flags & 64u), "incomplete world/color envelope generated frames");
            if (status.flags & 64u)
                ++generated;
            ComPtr<ID3D11Texture2D> retained;
            check(chain->GetBuffer(0, IID_PPV_ARGS(&retained)), "switch buffer identity");
            require(retained.Get() == final.Get(), "backend replacement invalidated Unity's cached buffer");
        }
        const auto status = channel->status();
        std::printf("facade_backend=%u generation=%llu apps=%llu generation_enabled=%u "
                    "generated_submissions=%llu sdk_reported=%llu\n",
                    backend, status.generation, status.applicationPresents, generated,
                    status.generatedSubmissions, status.sdkReportedPresents);
        if (backend)
            require(generated > 0, "valid envelopes never enabled the selected SDK");
    }
    CloseHandle(latency);
}

} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ComPtr<ID3D12InfoQueue> diagnostics;
    try {
        if (argc != 1 && argc != 3) {
            std::fputs("Usage: present-probe [FSR-runtime SL-runtime]\n", stderr);
            return 2;
        }
        const auto runtime = argc == 3 ? std::filesystem::absolute(argv[1]) : std::filesystem::path{};
        if (argc == 3)
            dspaa::initializePresentationRuntime(std::filesystem::absolute(argv[2]), {}, report);
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
        ComPtr<IDXGIFactory2> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                &device, nullptr, &context),
              "synthetic D3D11 device");
        auto bridge = dspaa::acquireDx11Dx12(device.Get());
        bridge->device12()->QueryInterface(IID_PPV_ARGS(&diagnostics));
        std::printf("d3d12_info_queue_available=%u\n", diagnostics ? 1u : 0u);
        worldSnapshots(bridge);
        unidirectionalTransport(bridge);
        unpredicatedTransport(bridge);
        Window window;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = 640;
        description.Height = 360;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> first;
        dspaa::PresentRetirement retirement;
        check(dspaa::createPresentationFacade(factory.Get(), device.Get(), window.value, description, nullptr,
                                              nullptr, runtime, report, &first, &retirement),
              "create facade");
        ComPtr<IDXGISwapChain4> chain;
        check(first.As(&chain), "swapchain4 facade");
        first.Reset();
        ComPtr<ID3D11Device> exposed;
        check(chain->GetDevice(IID_PPV_ARGS(&exposed)), "facade exposes D3D11");
        require(exposed.Get() == device.Get(), "device identity changed");
        ComPtr<ID3D12Device> leaked;
        require(chain->GetDevice(IID_PPV_ARGS(&leaked)) == E_NOINTERFACE && !leaked,
                "D3D12 device leaked to D3D11 caller");
        ComPtr<IDXGIFactory2> parent;
        check(chain->GetParent(IID_PPV_ARGS(&parent)), "facade parent");
        require(parent.Get() == factory.Get(), "factory identity changed");
        ComPtr<IUnknown> identity, marker;
        check(chain.As(&identity), "COM identity");
        check(chain->QueryInterface(dspaa::presentationFacadeId,
                                    reinterpret_cast<void**>(marker.GetAddressOf())),
              "private facade identity");
        require(marker.Get() == identity.Get(), "facade marker identity changed");
        marker.Reset();
        identity.Reset();
        constexpr GUID dataKey{0x2cad4ca9, 0x2a45, 0x41e6, {0xa9, 0x29, 0x64, 0x21, 0x43, 0x66, 0x38, 0x65}};
        const UINT stored = 42;
        check(chain->SetPrivateData(dataKey, sizeof(stored), &stored), "store private data");
        UINT got = 0, bytes = sizeof(got);
        check(chain->GetPrivateData(dataKey, &bytes, &got), "read private data");
        require(got == stored && bytes == sizeof(got), "private data changed");
        check(chain->SetPrivateData(dataKey, 0, nullptr), "remove private data");
        require(chain->GetPrivateData(dataKey, &bytes, &got) == DXGI_ERROR_NOT_FOUND,
                "deleted private data remained");
        HANDLE latency = chain->GetFrameLatencyWaitableObject();
        require(latency != nullptr, "stable latency handle missing");
        unsigned presents = 0;
        for (const auto size : std::array<std::array<UINT, 2>, 3>{{{640, 360}, {800, 450}, {640, 360}}}) {
            if (presents)
                check(chain->ResizeBuffers(0, size[0], size[1], DXGI_FORMAT_UNKNOWN, description.Flags),
                      "resize facade");
            ComPtr<ID3D11Resource> buffer;
            check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Unity ID3D11Resource buffer zero");
            ComPtr<ID3D11Texture2D> texture;
            check(buffer.As(&texture), "texture interface");
            D3D11_TEXTURE2D_DESC td{};
            texture->GetDesc(&td);
            require(td.Width == size[0] && td.Height == size[1], "logical buffer dimensions stale");
            require(chain->ResizeBuffers(0, size[0], size[1], DXGI_FORMAT_UNKNOWN, description.Flags) ==
                        DXGI_ERROR_INVALID_CALL,
                    "resize accepted outstanding buffer references");
            ComPtr<ID3D11Resource> nonzero;
            require(chain->GetBuffer(1, IID_PPV_ARGS(&nonzero)) == DXGI_ERROR_INVALID_CALL,
                    "unsupported logical buffer was fabricated");
            ComPtr<ID3D11RenderTargetView> rtv;
            check(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "logical render target view");
            for (unsigned frame = 0; frame < 12; ++frame) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                const float color[] = {frame / 12.f, .25f, .75f, 1};
                auto* target = rtv.Get();
                context->OMSetRenderTargets(1, &target, nullptr);
                context->ClearRenderTargetView(target, color);
                check(chain->Present(0, DXGI_PRESENT_TEST), "TEST facade");
                DXGI_PRESENT_PARAMETERS parameters{};
                check(frame % 2 ? chain->Present(0, 0) : chain->Present1(0, 0, &parameters),
                      "present facade");
                ComPtr<ID3D11RenderTargetView> bound;
                context->OMGetRenderTargets(1, &bound, nullptr);
                require(!bound, "successful flip did not unbind logical buffer zero");
                require(chain->GetCurrentBackBufferIndex() == 0, "physical D3D12 index leaked to engine");
                ComPtr<ID3D11Resource> again;
                check(chain->GetBuffer(0, IID_PPV_ARGS(&again)), "cached buffer identity");
                require(again.Get() == buffer.Get(), "cached D3D11 buffer identity rotated");
                ++presents;
            }
            std::printf("bridge_stage=%ux%u presents=%u\n", size[0], size[1], presents);
            std::fflush(stdout);
        }
        CloseHandle(latency);
        if (argc == 3)
            backendSequence(chain.Get(), device.Get(), context.Get(), description.Flags);
        chain.Reset();
        bridge->drain();
        if (auto runtimeService = dspaa::earlyPresentationRuntime())
            require(runtimeService->shutdown() == dspaa::PresentRetirement::Drained,
                    "facade SL shutdown did not retire");
        const auto count = errors(diagnostics.Get());
        require(count == 0, "bridge D3D12 validation errors");
        std::printf("facade_native_presents=%u resize=2 d3d12_validation=%s visible_windows=0 "
                    "pixel_fidelity=not_measured\n",
                    presents, diagnostics ? "zero_errors" : "unavailable");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
    }
    try {
        (void)errors(diagnostics.Get());
    } catch (...) {
    }
    return 1;
}
