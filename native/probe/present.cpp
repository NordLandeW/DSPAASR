#include "present/facade.h"
#include "graphics/dx11-dx12.h"
#include "present/channel.h"
#include <DirectXMath.h>
#include <cstring>
#include <d3d12sdklayers.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT value, const char* message) { dspaa::graphicsCheck(value, message); }
void report(const char* message) noexcept { std::fprintf(stderr, "facade: %s\n", message); }
struct Window {
    HWND value = nullptr;
    Window() {
        WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = DefWindowProcW;
        type.lpszClassName = L"DSPAASR hidden bridge probe";
        require(RegisterClassW(&type) != 0, "register probe window");
        value = CreateWindowExW(0, type.lpszClassName, type.lpszClassName, WS_POPUP, 0, 0, 640, 360,
                                nullptr, nullptr, type.hInstance, nullptr);
        require(value != nullptr, "create invisible window");
    }
    ~Window() { if (value) DestroyWindow(value); }
};
unsigned errors(ID3D12InfoQueue* queue) {
    unsigned count = 0;
    if (!queue) return count;
    for (uint64_t i = 0; i < queue->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; check(queue->GetMessage(i, nullptr, &size), "debug message size");
        std::vector<unsigned char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        check(queue->GetMessage(i, message, &size), "debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr, "D3D12: %s\n", message->pDescription); ++count;
        }
    }
    return count;
}
void backendSequence(IDXGISwapChain4* chain, ID3D11Device* device, ID3D11DeviceContext* context, UINT flags) {
    auto channel = dspaa::presentationChannel(); require(channel != nullptr, "missing facade mailbox");
    check(chain->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_UNKNOWN, flags), "SDK test surface");
    check(chain->SetMaximumFrameLatency(1), "host latency policy before SDK replacement");
    auto bridge = dspaa::acquireDx11Dx12(device);
    std::array<dspaa::SharedTexture, 5> inputs;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT};
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] = bridge->texture(1280, 720, formats[i], false);
        ComPtr<ID3D11RenderTargetView> target; check(device->CreateRenderTargetView(inputs[i].dx11.Get(), nullptr, &target), "temporal RTV");
        const float color[] = {.5f,.2f,.1f,1}; context->ClearRenderTargetView(target.Get(), color);
    }
    ComPtr<ID3D11Texture2D> final; check(chain->GetBuffer(0, IID_PPV_ARGS(&final)), "stable SDK-switch logical buffer");
    ComPtr<ID3D11RenderTargetView> target; check(device->CreateRenderTargetView(final.Get(), nullptr, &target), "Final RTV");
    HANDLE latency = chain->GetFrameLatencyWaitableObject(); require(latency != nullptr, "SDK stable waitable handle");
    auto event = dspaa::presentationRenderEvent();
    for (uint32_t backend : {1u,2u,1u,0u}) {
        if (backend == 2 && !(channel->status().flags & 4u)) { std::puts("SKIP bridge DLSS: hardware/runtime unsupported"); continue; }
        DspAaPresentationConfiguration selection{sizeof(selection),1,backend,0,1,1};
        require(channel->configure(selection), "backend selection rejected");
        unsigned generated = 0;
        for (unsigned index = 0; index < 18; ++index) {
            require(WaitForSingleObject(latency, 30000) == WAIT_OBJECT_0, "host latency lost during backend replacement");
            DspAaPresentationBegin begin{}; require(channel->begin(begin), "before-input mailbox begin failed");
            channel->marker(begin.applicationFrameId, dspaa::SlMarker::SimulationEnd);
            event(1, reinterpret_cast<void*>(begin.applicationFrameId));
            DspAaPresentationInputs frame{}; frame.size = sizeof(frame); frame.version = 1;
            frame.applicationFrameId = begin.applicationFrameId; frame.generation = begin.generation;
            frame.hudless = inputs[0].dx11.Get(); frame.depth = inputs[1].dx11.Get(); frame.motion = inputs[2].dx11.Get();
            frame.occlusionAlpha = inputs[3].dx11.Get(); frame.uiInfluence = inputs[4].dx11.Get();
            frame.renderWidth = 1280; frame.renderHeight = 720; frame.flags = 1u | 16u | (!index ? 2u : 0u);
            if (index == 8) frame.flags &= ~1u; // Incomplete real frame pauses generation, not presentation.
            frame.motionScaleX = frame.motionScaleY = 1; frame.milliseconds = 16.667f;
            frame.cameraNear = .1f; frame.cameraFar = 1000; frame.verticalFov = DirectX::XM_PI / 3;
            frame.preExposure = frame.viewSpaceToMeters = 1; frame.maxLuminance = 100;
            frame.cameraUp[1] = frame.cameraRight[0] = frame.cameraForward[2] = 1;
            auto store = [](float* out, DirectX::FXMMATRIX value) { DirectX::XMFLOAT4X4 m; DirectX::XMStoreFloat4x4(&m,value); std::memcpy(out,&m,sizeof(m)); };
            const auto projection = DirectX::XMMatrixPerspectiveFovLH(frame.verticalFov,1280.f/720,.1f,1000);
            store(frame.cameraViewToClip,projection); store(frame.clipToCameraView,DirectX::XMMatrixInverse(nullptr,projection));
            store(frame.clipToPrevClip,DirectX::XMMatrixIdentity()); store(frame.prevClipToClip,DirectX::XMMatrixIdentity());
            const auto token = dspaa::queuePresentationInputs(&frame); require(token != nullptr, "end-frame queue rejected");
            event(2,token);
            const float color[] = {.6f,.2f,.1f,1}; context->ClearRenderTargetView(target.Get(),color);
            const auto before = channel->status().applicationPresents;
            check(chain->Present(0,DXGI_PRESENT_TEST), "TEST did not preserve pending frame");
            require(channel->status().applicationPresents == before, "TEST advanced application telemetry");
            check(chain->Present(0,0), "SDK facade Present");
            const auto status = channel->status();
            if (status.activeBackend != backend) throw std::runtime_error(std::string("backend selection fell back: ") + status.message);
            require(status.lastPresentedFrameId == begin.applicationFrameId && status.applicationPresents == before + 1,
                    "Present did not consume the matching end-of-frame identity");
            if (index == 8) require(!(status.flags & 64u), "incomplete envelope generated frames");
            if (status.flags & 64u) ++generated;
            ComPtr<ID3D11Texture2D> retained; check(chain->GetBuffer(0,IID_PPV_ARGS(&retained)), "switch buffer identity");
            require(retained.Get() == final.Get(), "backend replacement invalidated Unity's cached buffer");
        }
        const auto status = channel->status();
        std::printf("facade_backend=%u generation=%llu apps=%llu generation_enabled=%u generated_submissions=%llu sdk_reported=%llu\n",
                    backend,status.generation,status.applicationPresents,generated,status.generatedSubmissions,status.sdkReportedPresents);
        if (backend) require(generated > 0, "valid envelopes never enabled the selected SDK");
    }
    CloseHandle(latency);
}

}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ComPtr<ID3D12InfoQueue> diagnostics;
    try {
        if (argc != 1 && argc != 3) { std::fputs("Usage: present-probe [FSR-runtime SL-runtime]\n", stderr); return 2; }
        const auto runtime = argc == 3 ? std::filesystem::absolute(argv[1]) : std::filesystem::path{};
        if (argc == 3) dspaa::initializePresentationRuntime(std::filesystem::absolute(argv[2]), {}, report);
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory2> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                &device, nullptr, &context), "synthetic D3D11 device");
        auto bridge = dspaa::acquireDx11Dx12(device.Get());
        bridge->device12()->QueryInterface(IID_PPV_ARGS(&diagnostics));
        Window window;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = 640; description.Height = 360; description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1; description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2; description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> first;
        dspaa::PresentRetirement retirement;
        check(dspaa::createPresentationFacade(factory.Get(), device.Get(), window.value, description, nullptr,
                                              nullptr, runtime, report, &first, &retirement), "create facade");
        ComPtr<IDXGISwapChain4> chain;
        check(first.As(&chain), "swapchain4 facade"); first.Reset();
        ComPtr<ID3D11Device> exposed;
        check(chain->GetDevice(IID_PPV_ARGS(&exposed)), "facade exposes D3D11");
        require(exposed.Get() == device.Get(), "device identity changed");
        ComPtr<ID3D12Device> leaked;
        require(chain->GetDevice(IID_PPV_ARGS(&leaked)) == E_NOINTERFACE && !leaked, "D3D12 device leaked to D3D11 caller");
        ComPtr<IDXGIFactory2> parent;
        check(chain->GetParent(IID_PPV_ARGS(&parent)), "facade parent");
        require(parent.Get() == factory.Get(), "factory identity changed");
        ComPtr<IUnknown> identity, marker;
        check(chain.As(&identity), "COM identity");
        check(chain->QueryInterface(dspaa::presentationFacadeId, reinterpret_cast<void**>(marker.GetAddressOf())), "private facade identity");
        require(marker.Get() == identity.Get(), "facade marker identity changed"); marker.Reset(); identity.Reset();
        constexpr GUID dataKey{0x2cad4ca9,0x2a45,0x41e6,{0xa9,0x29,0x64,0x21,0x43,0x66,0x38,0x65}};
        const UINT stored = 42;
        check(chain->SetPrivateData(dataKey, sizeof(stored), &stored), "store private data");
        UINT got = 0, bytes = sizeof(got);
        check(chain->GetPrivateData(dataKey, &bytes, &got), "read private data");
        require(got == stored && bytes == sizeof(got), "private data changed");
        check(chain->SetPrivateData(dataKey, 0, nullptr), "remove private data");
        require(chain->GetPrivateData(dataKey, &bytes, &got) == DXGI_ERROR_NOT_FOUND, "deleted private data remained");
        HANDLE latency = chain->GetFrameLatencyWaitableObject();
        require(latency != nullptr, "stable latency handle missing");
        unsigned presents = 0;
        for (const auto size : std::array<std::array<UINT,2>,3>{{{640,360},{800,450},{640,360}}}) {
            if (presents) check(chain->ResizeBuffers(0,size[0],size[1],DXGI_FORMAT_UNKNOWN,description.Flags), "resize facade");
            ComPtr<ID3D11Resource> buffer;
            check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Unity ID3D11Resource buffer zero");
            ComPtr<ID3D11Texture2D> texture; check(buffer.As(&texture), "texture interface");
            D3D11_TEXTURE2D_DESC td{}; texture->GetDesc(&td);
            require(td.Width == size[0] && td.Height == size[1], "logical buffer dimensions stale");
            require(chain->ResizeBuffers(0,size[0],size[1],DXGI_FORMAT_UNKNOWN,description.Flags) == DXGI_ERROR_INVALID_CALL,
                    "resize accepted outstanding buffer references");
            ComPtr<ID3D11Resource> nonzero;
            require(chain->GetBuffer(1, IID_PPV_ARGS(&nonzero)) == DXGI_ERROR_INVALID_CALL, "unsupported logical buffer was fabricated");
            ComPtr<ID3D11RenderTargetView> rtv;
            check(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "logical render target view");
            for (unsigned frame = 0; frame < 12; ++frame) {
                MSG message{}; while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                const float color[] = {frame / 12.f, .25f, .75f, 1};
                auto* target = rtv.Get(); context->OMSetRenderTargets(1, &target, nullptr);
                context->ClearRenderTargetView(target, color);
                check(chain->Present(0, DXGI_PRESENT_TEST), "TEST facade");
                DXGI_PRESENT_PARAMETERS parameters{};
                check(frame % 2 ? chain->Present(0,0) : chain->Present1(0,0,&parameters), "present facade");
                ComPtr<ID3D11RenderTargetView> bound;
                context->OMGetRenderTargets(1, &bound, nullptr);
                require(!bound, "successful flip did not unbind logical buffer zero");
                require(chain->GetCurrentBackBufferIndex() == 0, "physical D3D12 index leaked to engine");
                ComPtr<ID3D11Resource> again;
                check(chain->GetBuffer(0, IID_PPV_ARGS(&again)), "cached buffer identity");
                require(again.Get() == buffer.Get(), "cached D3D11 buffer identity rotated");
                ++presents;
            }
            std::printf("bridge_stage=%ux%u presents=%u\n", size[0], size[1], presents); std::fflush(stdout);
        }
        CloseHandle(latency);
        if (argc == 3) backendSequence(chain.Get(),device.Get(),context.Get(),description.Flags);
        chain.Reset(); bridge->drain();
        if (auto runtimeService = dspaa::earlyPresentationRuntime())
            require(runtimeService->shutdown() == dspaa::PresentRetirement::Drained, "facade SL shutdown did not retire");
        const auto count = errors(diagnostics.Get());
        require(count == 0, "bridge D3D12 validation errors");
        std::printf("facade_native_presents=%u resize=2 d3d12_errors=%u visible_windows=0 pixel_fidelity=not_measured\n", presents,count);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"ERROR: %s\n",e.what()); }
    try { (void)errors(diagnostics.Get()); } catch (...) {}
    return 1;
}
