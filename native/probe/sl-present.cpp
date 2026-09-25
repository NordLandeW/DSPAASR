#include "graphics/dx11-dx12.h"
#include "sl/presenter.h"
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <array>
#include <cstring>
#include <d3d12sdklayers.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Window {
    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND handle = nullptr;
    bool drained = true;
    static constexpr wchar_t name[] = L"DSPAASR Streamline hidden lifecycle probe";
    Window() {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = module;
        type.lpszClassName = name;
        require(RegisterClassW(&type) != 0, "Register hidden SL window class");
        // No visible style, activation, ShowWindow, input injection or capture.
        handle = CreateWindowExW(0, name, name, WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr, module, nullptr);
        require(handle != nullptr, "Create hidden SL window");
    }
    ~Window() {
        if (drained) {
            if (handle)
                DestroyWindow(handle);
            UnregisterClassW(name, module);
        }
    }
};
unsigned validationErrors(ID3D12InfoQueue* diagnostics) {
    unsigned result = 0;
    if (!diagnostics)
        return result;
    for (uint64_t index = 0; index < diagnostics->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        dspaa::graphicsCheck(diagnostics->GetMessage(index, nullptr, &bytes), "Size SL probe diagnostic");
        std::vector<char> buffer(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        dspaa::graphicsCheck(diagnostics->GetMessage(index, message, &bytes), "Read SL probe diagnostic");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++result;
            std::cerr << message->pDescription << '\n';
        }
    }
    return result;
}
struct Images {
    std::shared_ptr<dspaa::FrameImages> frame = std::make_shared<dspaa::FrameImages>();
    std::vector<dspaa::SharedTexture> keep11;
    RECT content{};
    unsigned renderWidth, renderHeight;
    float cameraAspect;
    Images(dspaa::Dx11Dx12& bridge, unsigned width, unsigned height, RECT area = {}, unsigned rasterWidth = 0,
           unsigned rasterHeight = 0)
        : content(area) {
        if (!content.left && !content.top && !content.right && !content.bottom)
            content = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        const auto contentWidth = static_cast<unsigned>(content.right - content.left);
        const auto contentHeight = static_cast<unsigned>(content.bottom - content.top);
        renderWidth = rasterWidth ? rasterWidth : width;
        renderHeight = rasterHeight ? rasterHeight : height;
        cameraAspect = static_cast<float>(contentWidth) / contentHeight;
        auto access = bridge.lock();
        auto make = [&](dspaa::PresentImage& output, unsigned textureWidth, unsigned textureHeight,
                        DXGI_FORMAT format, const void* source, UINT pitch) {
            auto image = bridge.texture(textureWidth, textureHeight, format, false);
            bridge.context11()->UpdateSubresource(image.dx11.Get(), 0, nullptr, source, pitch, 0);
            output.resource = image.dx12;
            keep11.push_back(std::move(image));
        };
        const size_t pixels = static_cast<size_t>(width) * height;
        std::vector<uint32_t> final(pixels);
        std::vector<HALF> color(pixels * 4);
        const size_t rasterPixels = static_cast<size_t>(renderWidth) * renderHeight;
        std::vector<float> depth(rasterPixels, .5f);
        std::vector<HALF> motion(rasterPixels * 2, 0), distortion(rasterPixels * 4, 0);
        size_t uiPixels = 0;
        // Final has a 60%-opaque screen panel over the same SDR-encoded scene
        // as HUDless. Their backgrounds agree after FP16-to-UNORM conversion;
        // no UI alpha/color surface or global encoding mismatch stands in for UI.
        constexpr std::array<unsigned, 3> panel{230, 210, 40};
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const size_t pixel = static_cast<size_t>(y) * width + x;
                color[pixel * 4 + 3] = XMConvertFloatToHalf(1.f);
                final[pixel] = 0xff000000u;
                if (static_cast<LONG>(x) < content.left || static_cast<LONG>(x) >= content.right ||
                    static_cast<LONG>(y) < content.top || static_cast<LONG>(y) >= content.bottom)
                    continue; // Real canonical H/Final bars, not metadata over a stretched full-screen image.
                const unsigned localX = x - static_cast<unsigned>(content.left),
                               localY = y - static_cast<unsigned>(content.top);
                const std::array<unsigned, 3> scene{32 + localX * 96 / contentWidth,
                                                    48 + localY * 80 / contentHeight, 96};
                const bool ui = localX >= contentWidth / 8 && localX < contentWidth / 2 &&
                                localY >= contentHeight / 4 && localY < contentHeight / 2;
                uint32_t background = 0xff000000u;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    color[pixel * 4 + channel] =
                        XMConvertFloatToHalf(static_cast<float>(scene[channel]) / 255.f);
                    background |= scene[channel] << (channel * 8);
                    const unsigned value =
                        ui ? (3 * panel[channel] + 2 * scene[channel] + 2) / 5 : scene[channel];
                    final[pixel] |= value << (channel * 8);
                }
                if (final[pixel] != background)
                    ++uiPixels;
            }
        require(uiPixels && uiPixels < pixels,
                "SL probe must contain both nonzero screen UI and matching background");
        make(frame->finalColor, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, final.data(),
             width * sizeof(uint32_t));
        make(frame->hudless, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, color.data(),
             width * 4 * sizeof(HALF));
        make(frame->depth, renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT, depth.data(),
             renderWidth * sizeof(float));
        make(frame->motion, renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT, motion.data(),
             renderWidth * 2 * sizeof(HALF));
        make(frame->slDistortion, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
             distortion.data(), renderWidth * 4 * sizeof(HALF));
        bridge.handoffTo12();
    }
};
void matrices(dspaa::PresentationFrame& frame, float cameraAspect, bool reflectY = false) {
    auto projection = XMMatrixPerspectiveFovLH(frame.verticalFov, cameraAspect,
                                               frame.depthInverted ? frame.cameraFar : frame.cameraNear,
                                               frame.depthInverted ? frame.cameraNear : frame.cameraFar);
    if (reflectY)
        projection = XMMatrixMultiply(projection, XMMatrixScaling(1, -1, 1));
    auto store = [](std::array<float, 16>& output, FXMMATRIX input) {
        XMFLOAT4X4 matrix;
        XMStoreFloat4x4(&matrix, input);
        std::memcpy(output.data(), &matrix, sizeof(matrix));
    };
    store(frame.cameraViewToClip, projection);
    store(frame.clipToCameraView, XMMatrixInverse(nullptr, projection));
    store(frame.clipToPrevClip, XMMatrixIdentity());
    store(frame.prevClipToClip, XMMatrixIdentity());
}
void marker(dspaa::SlRuntime& runtime, uint64_t frame, dspaa::SlMarker marker) {
    if (!runtime.marker(frame, marker))
        throw std::runtime_error("SL probe marker rejected: " + runtime.status().reason);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::cout << std::unitbuf;
    std::wcout << std::unitbuf;
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: sl-present-probe <verified-official-runtime-directory> [diagnostic-directory]\n"
                     "Opt-in real-adapter hidden-window API/lifecycle probe, not physical FPS or "
                     "pixel-fidelity acceptance.\n";
        return 2;
    }
    ComPtr<ID3D12InfoQueue> diagnostics;
    try {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            std::cout << "d3d12_debug=enabled\n";
        }
        dspaa::SlRuntimeCreateInfo runtimeInfo;
        runtimeInfo.runtimeDirectory = std::filesystem::absolute(argv[1]);
        if (argc == 3)
            runtimeInfo.logDirectory = std::filesystem::absolute(argv[2]);
        // Early SL init, before the application's factory/device/chain. Manual
        // hooking must not depend on intercepting a late, already-live chain.
        auto runtime = std::make_shared<dspaa::SlRuntime>(runtimeInfo);
        require(!runtime->beginFrame(1), "An inactive SL runtime generated a before-input ticket");
        ComPtr<IDXGIFactory1> factory;
        dspaa::graphicsCheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create SL probe factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            dspaa::graphicsCheck(result, "Enumerate SL probe adapter");
            DXGI_ADAPTER_DESC1 description{};
            dspaa::graphicsCheck(candidate->GetDesc1(&description), "Describe SL adapter");
            if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                std::wcout << L"adapter=" << description.Description << '\n';
                break;
            }
        }
        require(adapter.Get() != nullptr, "No hardware adapter for opt-in SL probe");
        ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        dspaa::graphicsCheck(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &device, nullptr, nullptr),
                             "Create synthetic Unity D3D11 device");
        dspaa::Dx11Dx12 bridge(device.Get());
        (void)bridge.device12()->QueryInterface(IID_PPV_ARGS(&diagnostics));
        runtime->attach(bridge.device12(), factory.Get());
        const auto support = runtime->status();
        if (!support.dlssSupported || !support.reflexSupported || !support.pclSupported) {
            std::cout << "SKIP unsupported_runtime=" << support.reason << " dlss=" << support.dlssSupported
                      << " reflex=" << support.reflexSupported << " pcl=" << support.pclSupported << '\n';
            require(runtime->shutdown() == dspaa::PresentRetirement::Drained,
                    "Unsupported runtime failed clean shutdown");
            return 77;
        }
        ComPtr<ID3D12Fence> ready;
        dspaa::graphicsCheck(bridge.device12()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ready)),
                             "Create SL producer fence");
        Window window;
        dspaa::SlPresenterCreateInfo create;
        create.runtime = runtime;
        create.window = window.handle;
        create.generation = 1;
        auto& desc = create.swapChain;
        desc.Width = 1280;
        desc.Height = 720;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags =
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        window.drained = false;
        dspaa::SlPresenter presenter(create);
        dspaa::graphicsCheck(presenter.setMaximumFrameLatency(1), "Set application-owned SL latency");
        dspaa::PresentArguments test;
        test.flags = DXGI_PRESENT_TEST;
        dspaa::SlGenerationRequest request;
        request.mode = dspaa::SlGenerationMode::Fixed;
        dspaa::graphicsCheck(presenter.present({}, test, request), "SL TEST without a frame/token");
        require(presenter.status().applicationPresents == 0 && runtime->status().acceptedFrameTickets == 0,
                "SL TEST advanced frame tracking");
        require(presenter.present({}, {}, request) == E_INVALIDARG, "SL accepted absent non-TEST Final");
        uint64_t id = 1000;
        std::weak_ptr<const dspaa::PresentationFrame> firstLease;
        const std::array<std::array<unsigned, 2>, 3> sizes{{{1280, 720}, {1600, 900}, {1280, 720}}};
        for (unsigned stage = 0; stage < sizes.size(); ++stage) {
            desc.Width = sizes[stage][0];
            desc.Height = sizes[stage][1];
            if (stage)
                dspaa::graphicsCheck(presenter.resize(desc, stage + 1), "Resize SL presentation generation");
            Images inputs(bridge, desc.Width, desc.Height);
            const unsigned contentWidth = desc.Height * 4 / 3;
            const auto inset = static_cast<LONG>((desc.Width - contentWidth) / 2);
            const RECT cropped{inset, 0, inset + static_cast<LONG>(contentWidth),
                               static_cast<LONG>(desc.Height)};
            const RECT shifted{cropped.left + 16, cropped.top, cropped.right + 16, cropped.bottom};
            // A 4:3 camera in a 16:9 output, with intentionally rounded low-res
            // D/M. Neither swapchain nor raster aspect is the actual projection.
            Images cropInputs(bridge, desc.Width, desc.Height, cropped, contentWidth * 2 / 3 + 1,
                              desc.Height * 2 / 3);
            Images shiftInputs(bridge, desc.Width, desc.Height, shifted, contentWidth * 2 / 3 + 1,
                               desc.Height * 2 / 3);
            const LONG w = static_cast<LONG>(desc.Width), h = static_cast<LONG>(desc.Height);
            const std::array<RECT, 8> invalidAreas{{{-1, 0, w, h},
                                                    {0, -1, w, h},
                                                    {0, 0, w + 1, h},
                                                    {0, 0, w, h + 1},
                                                    {1, 0, 1, h},
                                                    {0, 1, w, 1},
                                                    {2, 0, 1, h},
                                                    {0, 2, w, 1}}};
            constexpr unsigned cropBegin = 24, shiftBegin = 28, fullBegin = 32, invalidBegin = 36;
            constexpr unsigned minimumCase = 44, badProjectionBegin = 45, recoverBegin = 50,
                               framesPerStage = 54;
            dspaa::graphicsCheck(bridge.queue12()->Signal(ready.Get(), stage + 1),
                                 "Publish immutable SL probe images");
            for (unsigned index = 0; index < framesPerStage; ++index) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (index == 13)
                    id += 3; // Real application IDs need not begin at one or remain contiguous.
                ++id;
                const bool missingTicket = index == 14;
                if (index == 17)
                    require(runtime->setReflex(dspaa::SlReflexMode::Off), "Reflex Off rejected");
                if (index == 18)
                    require(runtime->setReflex(dspaa::SlReflexMode::On), "Reflex On rejected");
                if (!missingTicket) {
                    require(runtime->beginFrame(id), "SL before-input ticket rejected");
                    const auto count = runtime->status().acceptedFrameTickets;
                    require(runtime->beginFrame(id) && runtime->status().acceptedFrameTickets == count,
                            "Duplicate BeginFrame allocated a second token");
                    marker(*runtime, id, dspaa::SlMarker::SimulationStart);
                }
                const auto capability = presenter.status();
                const Images& selected = index >= cropBegin && index < shiftBegin   ? cropInputs
                                         : index >= shiftBegin && index < fullBegin ? shiftInputs
                                                                                    : inputs;
                const bool invalidArea = index >= invalidBegin && index < minimumCase;
                const bool belowMinimum = index == minimumCase && capability.minimumDimension > 1;
                const bool badProjection = index >= badProjectionBegin && index < recoverBegin;
                auto frame = std::make_shared<dspaa::PresentationFrame>();
                frame->generation = stage + 1;
                frame->applicationFrameId = id;
                frame->images = selected.frame;
                if (index >= cropBegin && index < invalidBegin - 2)
                    frame->generationRect =
                        selected.content; // Two explicit-full frames, then two zero-full frames.
                if (invalidArea)
                    frame->generationRect = invalidAreas[index - invalidBegin];
                if (belowMinimum) {
                    const auto minimumExtent = static_cast<LONG>(capability.minimumDimension - 1);
                    frame->generationRect = {0, 0, minimumExtent, minimumExtent};
                }
                frame->readyFence = ready;
                frame->readyValue = stage + 1;
                frame->inputsComplete = index != 12;
                frame->reset = index == 0;
                if (index == 11) {
                    auto missingHudless = std::make_shared<dspaa::FrameImages>(*inputs.frame);
                    missingHudless->hudless = {};
                    frame->images = std::move(missingHudless);
                }
                frame->renderWidth = selected.renderWidth;
                frame->renderHeight = selected.renderHeight;
                frame->cameraNear = .1f;
                frame->cameraFar = 1000;
                frame->verticalFov = XM_PI / 3.f;
                frame->cameraUp = {0, 1, 0};
                frame->cameraRight = {1, 0, 0};
                frame->cameraForward = {0, 0, 1};
                frame->depthInverted = stage % 2 != 0;
                matrices(*frame, selected.cameraAspect, index >= shiftBegin && index < fullBegin);
                if (badProjection) {
                    switch (index - badProjectionBegin) {
                    case 0:
                        frame->cameraViewToClip[0] = 0;
                        break;
                    case 1:
                        frame->cameraViewToClip[5] = 0;
                        break;
                    case 2:
                        frame->cameraViewToClip[0] = std::numeric_limits<float>::quiet_NaN();
                        break;
                    case 3:
                        frame->cameraViewToClip[5] = std::numeric_limits<float>::infinity();
                        break;
                    case 4:
                        frame->cameraViewToClip[0] = std::numeric_limits<float>::min();
                        frame->cameraViewToClip[5] = std::numeric_limits<float>::max();
                        break;
                    }
                }
                if (!missingTicket) {
                    marker(*runtime, id, dspaa::SlMarker::SimulationEnd);
                    marker(*runtime, id, dspaa::SlMarker::RenderSubmitStart);
                }
                request.mode = index == 8 ? dspaa::SlGenerationMode::Off : dspaa::SlGenerationMode::Fixed;
                request.generatedFrames = 1;
                if (index == 16)
                    request.generatedFrames = capability.maximumGeneratedFrames + 1;
                if (index == 20)
                    request.mode = dspaa::SlGenerationMode::Dynamic;
                if (index == 22)
                    request.generatedFrames = capability.maximumGeneratedFrames;
                if (!stage && !index)
                    firstLease = frame;
                struct Timing {
                    dspaa::SlRuntime* runtime;
                    uint64_t expected;
                    unsigned before = 0, after = 0;
                    bool good = true;
                } timing{runtime.get(), id};
                dspaa::PresentBoundary boundary;
                boundary.context = &timing;
                boundary.before = [](void* p, uint64_t frameId) noexcept {
                    auto& t = *static_cast<Timing*>(p);
                    ++t.before;
                    const bool end = t.runtime->marker(frameId, dspaa::SlMarker::RenderSubmitEnd);
                    const bool start = t.runtime->marker(frameId, dspaa::SlMarker::PresentStart);
                    t.good = t.good && frameId == t.expected && end && start;
                    return end && start;
                };
                boundary.after = [](void* p, uint64_t frameId) noexcept {
                    auto& t = *static_cast<Timing*>(p);
                    ++t.after;
                    t.good = t.good && frameId == t.expected &&
                             t.runtime->marker(frameId, dspaa::SlMarker::PresentEnd);
                };
                dspaa::PresentArguments arguments;
                arguments.boundary = &boundary;
                dspaa::graphicsCheck(presenter.present(frame, arguments, request), "Present SL probe frame");
                require(timing.before == 1 && timing.after == 1 && (missingTicket || timing.good),
                        "Actual Present boundary markers were missing/duplicated");
                const auto observed = presenter.status();
                require(!observed.quarantined, "SL presenter quarantined");
                require(observed.applicationPresents == capability.applicationPresents + 1 &&
                            observed.lastApplicationFrameId == id,
                        "SL original Present did not advance for this input ID");
                const bool fallback = index == 8 || index == 11 || index == 12 || index == 14 ||
                                      index == 16 || index == 17 || invalidArea || belowMinimum ||
                                      badProjection || (index == 20 && !capability.dynamicSupported);
                if (index == cropBegin || index == shiftBegin || index == fullBegin ||
                    (index >= invalidBegin && index <= recoverBegin))
                    std::cout << "region_case=" << index << " stage=" << stage
                              << " expected_fallback=" << fallback
                              << " active=" << static_cast<unsigned>(observed.active)
                              << " sdk_status=" << observed.sdkStatus << " reason=" << observed.reason
                              << '\n';
                if (fallback)
                    require(observed.active == dspaa::SlGenerationMode::Off,
                            "SL invalid/Off input generated frames");
                else if (observed.active == dspaa::SlGenerationMode::Off)
                    throw std::runtime_error("SL generation inactive: " + observed.reason);
                if (!index)
                    require(presenter.present(frame, {}, request) == E_INVALIDARG,
                            "SL reused one application frame twice");
            }
            dspaa::graphicsCheck(presenter.setColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
                                 "Pause and retire SL stage inputs");
            const auto observed = presenter.status();
            std::cout << "stage=" << stage << " application_presents=" << observed.applicationPresents
                      << " sdk_reported_presents=" << observed.sdkReportedPresents
                      << " max_generated=" << observed.maximumGeneratedFrames
                      << " dynamic=" << observed.dynamicSupported << " status=" << observed.sdkStatus << '\n';
        }
        require(presenter.stop() == dspaa::PresentRetirement::Drained, "SL presenter retirement failed");
        window.drained = true;
        require(firstLease.expired(), "Retired SL producer lease remained held");
        require(presenter.stop() == dspaa::PresentRetirement::Drained && !presenter.swapChainForQueries(),
                "SL repeated stop changed retirement");
        const auto tickets = runtime->status().acceptedFrameTickets;
        require(!runtime->beginFrame(++id) && runtime->status().acceptedFrameTickets == tickets,
                "Inactive SL backend kept advancing frames");
        require(runtime->shutdown() == dspaa::PresentRetirement::Drained, "SL runtime shutdown failed");
        const auto errors = validationErrors(diagnostics.Get());
        require(!errors, "D3D12 SL validation errors");
        std::cout << "stages=3 gpu_retirement=complete producer_lease=released d3d12_errors=" << errors
                  << " screen_ui=nonzero ui_alpha=absent visible_windows=0 physical_display_fps=not_measured "
                     "content_rect=accepted_shifted_restored invalid_rect=original_only "
                     "invalid_projection=original_only "
                     "pixel_fidelity=not_measured reset_constants=not_independently_observed\n";
        return 0;
    } catch (const dspaa::SlPresenterCreationError& error) {
        std::cerr << "ERROR: " << error.what() << " construction_retirement="
                  << (error.retirement == dspaa::PresentRetirement::Drained ? "drained" : "quarantined")
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
    }
    try {
        (void)validationErrors(diagnostics.Get());
    } catch (...) {
    }
    return 1;
}
