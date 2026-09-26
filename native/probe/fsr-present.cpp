#include "fsr/presenter.h"
#include "graphics/dx11-dx12.h"
#include <DirectXPackedVector.h>
#include <array>
#include <cstdio>
#include <d3d12sdklayers.h>
#include <dbghelp.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
namespace {
// Probe-only crash evidence. No debugger attachment, registry changes or game
// process inspection; symbols are limited to this build's own directory.
LONG WINAPI crashTrace(EXCEPTION_POINTERS* failure) {
    std::fprintf(stderr, "probe_exception=%08lX address=%p\n", failure->ExceptionRecord->ExceptionCode,
                 failure->ExceptionRecord->ExceptionAddress);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const auto process = GetCurrentProcess();
    if (SymInitialize(process, "build/native/Release", TRUE)) {
        CONTEXT context = *failure->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC = {context.Rip, 0, AddrModeFlat};
        frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
        frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
        for (unsigned i = 0; i < 40; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 offset = 0;
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            SymGetModuleInfo64(process, frame.AddrPC.Offset, &module);
            if (SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol))
                std::fprintf(stderr, "  %s!%s+%llx\n", module.ModuleName, symbol->Name, offset);
            else
                std::fprintf(stderr, "  %s+%llx (%llx)\n", module.ModuleName,
                             frame.AddrPC.Offset - module.BaseOfImage, frame.AddrPC.Offset);
        }
        SymCleanup(process);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct HiddenWindow {
    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND handle = nullptr;
    bool retirementProved = true;
    static constexpr wchar_t className[] = L"DSPAASR FSR presentation probe";
    HiddenWindow() {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = module;
        type.lpszClassName = className;
        require(RegisterClassW(&type) != 0, "Register hidden probe window class");
        // No WS_VISIBLE, ShowWindow, activation, input or desktop capture.
        handle = CreateWindowExW(0, className, className, WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, module,
                                 nullptr);
        require(handle != nullptr, "Create hidden probe window");
    }
    ~HiddenWindow() {
        if (retirementProved) {
            if (handle)
                DestroyWindow(handle);
            UnregisterClassW(className, module);
        }
        // A failed probe exits its process; never tear down a live SDK HWND
        // merely to tidy up before process termination.
    }
};
void messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
unsigned debugErrors(ID3D12InfoQueue* diagnostics) {
    unsigned errors = 0;
    if (!diagnostics)
        return errors;
    for (uint64_t i = 0; i < diagnostics->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        dspaa::graphicsCheck(diagnostics->GetMessage(i, nullptr, &bytes), "Size D3D12 diagnostic");
        std::vector<char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        dspaa::graphicsCheck(diagnostics->GetMessage(i, message, &bytes), "Read D3D12 diagnostic");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++errors;
            std::cerr << "D3D12 validation: " << message->pDescription << '\n';
        }
    }
    return errors;
}
struct Inputs {
    std::shared_ptr<dspaa::FrameImages> images = std::make_shared<dspaa::FrameImages>();
    std::vector<dspaa::SharedTexture> resources;
    Inputs(dspaa::Dx11Dx12& bridge, unsigned width, unsigned height, unsigned renderWidth,
           unsigned renderHeight, unsigned sequence, bool sameFormat) {
        auto access = bridge.lock();
        auto create = [&](dspaa::PresentImage& destination, DXGI_FORMAT format, const void* data, UINT pitch,
                          unsigned w, unsigned h) {
            auto texture = bridge.texture(w, h, format, false);
            bridge.context11()->UpdateSubresource(texture.dx11.Get(), 0, nullptr, data, pitch, 0);
            destination.resource = texture.dx12;
            resources.push_back(std::move(texture));
        };
        const size_t pixels = static_cast<size_t>(width) * height;
        std::vector<uint32_t> final(pixels), hudlessBytes(pixels);
        std::vector<HALF> hudless(pixels * 4);
        const size_t rasterPixels = static_cast<size_t>(renderWidth) * renderHeight;
        std::vector<float> depth(rasterPixels);
        std::vector<HALF> motion(rasterPixels * 2), distortion(pixels * 2, 0);
        // Exercise changing bindings and Prepare's direct reads with distinct,
        // nonzero data, rather than a perpetually zero MV map. Image fidelity is
        // validated separately; successful dispatch is only submission evidence.
        for (unsigned y = 0; y < renderHeight; ++y)
            for (unsigned x = 0; x < renderWidth; ++x) {
                const size_t pixel = static_cast<size_t>(y) * renderWidth + x;
                depth[pixel] = .35f + .002f * sequence + .1f * y / renderHeight;
                motion[pixel * 2] = XMConvertFloatToHalf(.001f * (1 + sequence % 3));
                motion[pixel * 2 + 1] = XMConvertFloatToHalf(-.0005f * (1 + sequence % 2));
            }
        size_t uiPixels = 0;
        // A spatially varying postprocessed scene, with a 60%-opaque screen
        // panel only in Final. Both inputs use the same SDR-encoded values;
        // FP16 storage does not make HUDless linear. Background values agree
        // after format conversion, so a global encoding error is not our "UI".
        constexpr std::array<unsigned, 3> panel{230, 210, 40};
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const size_t pixel = static_cast<size_t>(y) * width + x;
                const std::array<unsigned, 3> scene{32 + ((x + sequence) % width) * 96 / width,
                                                    48 + y * 80 / height, 96};
                const bool ui = x >= width / 8 && x < width / 2 && y >= height / 4 && y < height / 2;
                uint32_t background = 0xff000000u;
                final[pixel] = background;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    hudless[pixel * 4 + channel] =
                        XMConvertFloatToHalf(static_cast<float>(scene[channel]) / 255.f);
                    background |= scene[channel] << (channel * 8);
                    const unsigned value =
                        ui ? (3 * panel[channel] + 2 * scene[channel] + 2) / 5 : scene[channel];
                    final[pixel] |= value << (channel * 8);
                }
                hudless[pixel * 4 + 3] = XMConvertFloatToHalf(1.f);
                hudlessBytes[pixel] = background;
                if (final[pixel] != background)
                    ++uiPixels;
            }
        require(uiPixels && uiPixels < pixels,
                "FSR probe must contain both nonzero screen UI and matching background");
        // This exercises SDK HUDless UI extraction, not a pixel-fidelity check.
        create(images->finalColor, DXGI_FORMAT_R8G8B8A8_UNORM, final.data(), width * sizeof(uint32_t), width,
               height);
        if (sameFormat)
            create(images->hudless, DXGI_FORMAT_R8G8B8A8_UNORM, hudlessBytes.data(), width * sizeof(uint32_t),
                   width, height);
        else
            create(images->hudless, DXGI_FORMAT_R16G16B16A16_FLOAT, hudless.data(), width * 4 * sizeof(HALF),
                   width, height);
        create(images->depth, DXGI_FORMAT_R32_FLOAT, depth.data(), renderWidth * sizeof(float), renderWidth,
               renderHeight);
        create(images->motion, DXGI_FORMAT_R16G16_FLOAT, motion.data(), renderWidth * 2 * sizeof(HALF),
               renderWidth, renderHeight);
        create(images->fsrDistortion, DXGI_FORMAT_R16G16_FLOAT, distortion.data(), width * 2 * sizeof(HALF),
               width, height);
        bridge.handoffTo12();
    }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crashTrace);
    std::cout << std::unitbuf;
    std::wcout << std::unitbuf;
    if (argc != 2) {
        std::cerr << "Usage: fsr-present-probe <official-runtime-directory>\n"
                     "Opt-in hidden-window real-adapter lifecycle/ABI probe. Not a display-FPS or "
                     "image-quality test.\n";
        return 2;
    }
    ComPtr<ID3D12InfoQueue> diagnostics;
    try {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            std::cout << "d3d12_debug=enabled\n";
        } else
            std::cout << "d3d12_debug=unavailable\n";
        ComPtr<IDXGIFactory1> factory;
        dspaa::graphicsCheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
                             "Create FSR presentation probe factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapters1(i, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            dspaa::graphicsCheck(result, "Enumerate FSR probe adapters");
            DXGI_ADAPTER_DESC1 description{};
            dspaa::graphicsCheck(candidate->GetDesc1(&description), "Read FSR probe adapter");
            if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                std::wcout << L"adapter=" << description.Description << '\n';
                break;
            }
        }
        require(adapter.Get() != nullptr, "No hardware adapter; this is not a default CTest");
        ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        dspaa::graphicsCheck(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &device, nullptr, nullptr),
                             "Create synthetic D3D11 game device");
        dspaa::Dx11Dx12 bridge(device.Get());
        (void)bridge.device12()->QueryInterface(IID_PPV_ARGS(&diagnostics));
        ComPtr<ID3D12Fence> ready;
        dspaa::graphicsCheck(bridge.device12()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ready)),
                             "Create probe producer fence");
        HiddenWindow window;
        dspaa::FsrPresenterCreateInfo create;
        create.window = window.handle;
        create.factory = factory.Get();
        create.device = bridge.device12();
        create.gameQueue = bridge.queue12();
        create.runtimeDirectory = std::filesystem::absolute(argv[1]);
        create.generation = 1;
        auto& desc = create.swapChain;
        desc.Width = 640;
        desc.Height = 360;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags =
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        ComPtr<IDXGIFactory5> featureFactory;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&featureFactory)) &&
            SUCCEEDED(featureFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                          sizeof(tearing))) &&
            tearing)
            desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        window.retirementProved = false;
        dspaa::FsrPresenter presenter(create);
        dspaa::graphicsCheck(presenter.setMaximumFrameLatency(1), "Set FSR probe maximum frame latency");
        dspaa::PresentArguments test;
        test.flags = DXGI_PRESENT_TEST;
        dspaa::graphicsCheck(presenter.present({}, test, true), "Forward FSR TEST without a frame");
        require(presenter.status().applicationPresents == 0 && presenter.status().successfulDispatches == 0,
                "TEST consumed an FSR application frame or generated work");
        require(presenter.present({}, {}, true) == E_INVALIDARG, "An absent non-TEST Final was accepted");
        // The SDK may activate long after simulation began; its first token is
        // not necessarily one, and dropped captures can leave intentional gaps.
        uint64_t frameId = 1000, readyValue = 0;
        std::weak_ptr<const dspaa::PresentationFrame> firstLease;
        std::vector<std::weak_ptr<Inputs>> producers;
        const std::array<std::array<unsigned, 2>, 3> sizes{{{640, 360}, {800, 450}, {640, 360}}};
        for (unsigned stage = 0; stage < sizes.size(); ++stage) {
            desc.Width = sizes[stage][0];
            desc.Height = sizes[stage][1];
            if (stage)
                dspaa::graphicsCheck(presenter.resize(desc, stage + 1), "Resize FSR probe presenter");
            const unsigned renderWidth = stage == 1 ? 400 : stage == 2 ? 1280 : desc.Width;
            const unsigned renderHeight = stage == 1 ? 300 : stage == 2 ? 960 : desc.Height;
            const auto before = presenter.status();
            for (unsigned index = 0; index < 24; ++index) {
                messages();
                // Change H storage within each surface generation. Each frame
                // owns fresh D/M data; after submission only the presenter lease
                // retains the producer, including its D3D11-side shared owners.
                auto inputs = std::make_shared<Inputs>(bridge, desc.Width, desc.Height, renderWidth,
                                                       renderHeight, index, (index / 4 + stage) % 2 == 0);
                producers.push_back(inputs);
                dspaa::graphicsCheck(bridge.queue12()->Signal(ready.Get(), ++readyValue),
                                     "Publish probe producer images");
                auto frame = std::make_shared<dspaa::PresentationFrame>();
                frame->images = inputs->images;
                frame->producerLifetime = inputs;
                frame->readyFence = ready;
                frame->readyValue = readyValue;
                if (index == 13)
                    frameId += 3;
                frame->generation = stage + 1;
                frame->applicationFrameId = ++frameId;
                frame->inputsComplete = true;
                frame->reset = index == 0;
                frame->renderWidth = renderWidth;
                frame->renderHeight = renderHeight;
                if (stage) {
                    const LONG contentWidth = static_cast<LONG>(desc.Height * 4 / 3);
                    const LONG left = (static_cast<LONG>(desc.Width) - contentWidth) / 2;
                    frame->generationRect = {left, 0, left + contentWidth, static_cast<LONG>(desc.Height)};
                    // A same-generation rectangle change must invalidate history.
                    if (index >= 16) {
                        ++frame->generationRect.left;
                        --frame->generationRect.right;
                    }
                }
                frame->cameraNear = .1f;
                frame->cameraFar = 1000;
                frame->verticalFov = 1.04719755f;
                frame->deltaMilliseconds = 1000.f / 60;
                frame->cameraUp = {0, 1, 0};
                frame->cameraRight = {1, 0, 0};
                frame->cameraForward = {0, 0, 1};
                frame->depthInverted = stage % 2 != 0;
                // Exercise Off after a live FG context, missing-temporal fallback,
                // and re-enable without substituting a different SDK provider.
                const bool enabled = index != 8;
                if (index == 12)
                    frame->inputsComplete = false;
                if (index == 11) {
                    auto missingHudless = std::make_shared<dspaa::FrameImages>(*inputs->images);
                    missingHudless->hudless = {};
                    frame->images = std::move(missingHudless);
                }
                if (!stage && !index)
                    firstLease = frame;
                inputs.reset();
                dspaa::graphicsCheck(presenter.present(frame, {}, enabled), "Present FSR probe frame");
                if (!index) {
                    require(presenter.present(frame, {}, enabled) == E_INVALIDARG,
                            "A repeated simulation token was consumed twice");
                    auto invalid = std::make_shared<dspaa::PresentationFrame>(*frame);
                    invalid->applicationFrameId = 0;
                    require(presenter.present(invalid, {}, enabled) == E_INVALIDARG,
                            "A missing simulation token was accepted");
                }
                const auto observed = presenter.status();
                require(!observed.quarantined, "FSR probe owner was quarantined");
                if (!enabled || !frame->inputsComplete || index == 11)
                    require(!observed.generationActive,
                            "Missing HUDless/incomplete/Off frame still enabled FSR generation");
                else if (!observed.generationActive)
                    throw std::runtime_error("FSR probe generation inactive: " + observed.reason);
            }
            // This owner entry proves retirement, pauses generation and preserves
            // the HWND. It permits releasing this stage's D3D11 producer pool.
            dspaa::graphicsCheck(presenter.setColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
                                 "Drain and pause FSR probe stage");
            const auto after = presenter.status();
            require(after.providerVerified && after.successfulDispatches > before.successfulDispatches,
                    "FSR HUDless-path generation dispatches were not observed");
            require(after.applicationPresents - before.applicationPresents == 24,
                    "FSR application frame accounting changed");
            require(!after.generationActive, "Drained FSR controls left generation active");
            for (const auto& producer : producers)
                require(producer.expired(), "A drained FSR stage retained a scalar metadata producer");
            std::cout << "stage=" << stage << " size=" << desc.Width << 'x' << desc.Height
                      << " application_presents=" << after.applicationPresents
                      << " dispatches=" << after.successfulDispatches << " reason=" << after.reason << '\n';
        }
        require(presenter.stop() == dspaa::PresentRetirement::Drained, "FSR stop did not prove retirement");
        window.retirementProved = true;
        require(firstLease.expired(), "A retired producer frame lease remained retained");
        for (const auto& producer : producers)
            require(producer.expired(), "A retired FSR input producer remained retained");
        require(presenter.stop() == dspaa::PresentRetirement::Drained,
                "Repeated FSR stop changed retirement");
        require(!presenter.swapChainForQueries(), "Retired FSR owner exposed its chain");
        const auto errors = debugErrors(diagnostics.Get());
        require(errors == 0, "D3D12 validation errors in FSR presentation probe");
        std::cout << "stages=3 gpu_retirement=complete producer_lease=released d3d12_errors=" << errors
                  << " screen_ui=nonzero hudless_formats=alternating depth_motion=time_varying_nonzero "
                     "ui_alpha=absent visible_windows=0 display_fps=not_measured "
                     "pixel_fidelity=not_measured\n";
        return 0;
    } catch (const dspaa::FsrPresenterCreationError& error) {
        std::cerr << "ERROR: " << error.what() << " initialization_retirement="
                  << (error.retirement == dspaa::PresentRetirement::Drained ? "drained" : "quarantined")
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
    }
    try {
        (void)debugErrors(diagnostics.Get());
    } catch (...) {
    }
    return 1;
}
