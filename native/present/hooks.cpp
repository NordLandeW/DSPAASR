#include "bootstrap-api.h"
#include "facade.h"
#include "channel.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace {
using Microsoft::WRL::ComPtr;
struct Hook {
    void* target = nullptr;
    void* original = nullptr;
};
struct State {
    std::mutex installMutex, logMutex;
    HANDLE log = INVALID_HANDLE_VALUE;
    std::atomic<unsigned> initialization{0};
    std::atomic<unsigned long long> presents{0};
    uint32_t mode = 0;
    std::filesystem::path runtime;
    std::atomic<HWND> primaryWindow{nullptr};
    Hook device, deviceChain, factory, factory1, factory2;
    Hook createChain, createHwnd, present, present1, getBuffer, resize, fullscreen, index, colorSpace, hdr;
};
State& state() {
    // Hooks and graphics callbacks are process-lifetime. Do not run mutex/file
    // destructors from DllMain while another graphics thread may still be active.
    static auto* value = new State;
    return *value;
}
void trace(const char* event, const char* format = "", ...) noexcept {
    try {
        auto& s = state();
        if (s.log == INVALID_HANDLE_VALUE) return;
        char fields[1536]{};
        va_list args;
        va_start(args, format);
        const int extra = vsnprintf(fields, sizeof(fields), format, args);
        va_end(args);
        if (extra < 0 || static_cast<size_t>(extra) >= sizeof(fields)) return;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        char line[2048]{};
        const int size = snprintf(line, sizeof(line), "{\"event\":\"%s\",\"qpc\":%lld,\"thread\":%lu%s%s}\n",
                                  event, counter.QuadPart, GetCurrentThreadId(), extra ? "," : "", fields);
        if (size <= 0 || static_cast<size_t>(size) >= sizeof(line)) return;
        std::lock_guard lock(s.logMutex);
        DWORD written = 0;
        WriteFile(s.log, line, static_cast<DWORD>(size), &written, nullptr);
    } catch (...) {}
}
void facadeLog(const char* message) noexcept {
    char escaped[1400]{};
    size_t size = 0;
    for (const auto* c = message; c && *c && size + 3 < sizeof(escaped); ++c) {
        if (*c == '\\' || *c == '"') escaped[size++] = '\\';
        escaped[size++] = static_cast<unsigned char>(*c) < 32 ? ' ' : *c;
    }
    trace("facade", "\"message\":\"%s\"", escaped);
}
bool targetWindow(HWND window) noexcept {
    DWORD pid = 0;
    wchar_t type[64]{};
    if (!window || !GetWindowThreadProcessId(window, &pid) || pid != GetCurrentProcessId() ||
        GetAncestor(window, GA_ROOT) != window || !GetClassNameW(window, type, 64) || wcscmp(type, L"UnityWndClass")) return false;
    auto& primary = state().primaryWindow;
    auto prior = primary.load();
    if (prior == window) return true;
    if (prior && IsWindow(prior)) return false;
    return primary.compare_exchange_strong(prior, window) || prior == window;
}
template<class T> void* method(T* object, size_t slot) {
    return (*reinterpret_cast<void***>(object))[slot];
}
bool install(Hook& hook, void* target, void* detour, const char* name) {
    if (!target) return false;
    if (hook.target) {
        if (hook.target != target)
            trace("trace.unobservedImplementation", "\"method\":\"%s\",\"address\":\"%p\"", name, target);
        return hook.target == target;
    }
    const auto result = MH_CreateHook(target, detour, &hook.original);
    if (result != MH_OK) {
        trace("hook.createFailed", "\"method\":\"%s\",\"status\":\"%s\"", name, MH_StatusToString(result));
        return false;
    }
    hook.target = target;
    const auto queued = MH_QueueEnableHook(target);
    if (queued != MH_OK) {
        trace("hook.queueFailed", "\"method\":\"%s\",\"status\":\"%s\"", name, MH_StatusToString(queued));
        return false;
    }
    trace("hook.queued", "\"method\":\"%s\",\"address\":\"%p\"", name, target);
    return true;
}
bool apply() {
    const auto result = MH_ApplyQueued();
    trace("hook.applied", "\"status\":\"%s\"", MH_StatusToString(result));
    return result == MH_OK;
}
void observeFactory(IUnknown* factory) noexcept;
void observeChain(IDXGISwapChain* chain, IUnknown* device) noexcept;

using CreateDevice = decltype(&D3D11CreateDevice);
using CreateDeviceChain = decltype(&D3D11CreateDeviceAndSwapChain);
HRESULT WINAPI createDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags,
                            const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, ID3D11Device** device,
                            D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    trace("device.begin", "\"driver\":%u,\"flags\":%u", static_cast<unsigned>(driver), flags);
    const auto result = reinterpret_cast<CreateDevice>(state().device.original)(
        adapter, driver, software, flags, levels, count, sdk, device, level, context);
    trace("device.end", "\"hr\":\"%08X\",\"device\":\"%p\",\"level\":%u",
          static_cast<unsigned>(result), device && SUCCEEDED(result) ? *device : nullptr,
          level && SUCCEEDED(result) ? static_cast<unsigned>(*level) : 0);
    return result;
}
HRESULT WINAPI createDeviceChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags,
                                 const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
                                 const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain,
                                 ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    trace("deviceAndChain.begin");
    const auto result = reinterpret_cast<CreateDeviceChain>(state().deviceChain.original)(
        adapter, driver, software, flags, levels, count, sdk, desc, chain, device, level, context);
    trace("deviceAndChain.end", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    if (SUCCEEDED(result) && chain && *chain) observeChain(*chain, device ? *device : nullptr);
    return result;
}
using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
HRESULT WINAPI createFactory(REFIID iid, void** output) {
    const auto result = reinterpret_cast<CreateFactory>(state().factory.original)(iid, output);
    trace("factory0", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    if (SUCCEEDED(result) && output && *output) observeFactory(static_cast<IUnknown*>(*output));
    return result;
}
HRESULT WINAPI createFactory1(REFIID iid, void** output) {
    const auto result = reinterpret_cast<CreateFactory>(state().factory1.original)(iid, output);
    trace("factory1", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    if (SUCCEEDED(result) && output && *output) observeFactory(static_cast<IUnknown*>(*output));
    return result;
}
HRESULT WINAPI createFactory2(UINT flags, REFIID iid, void** output) {
    const auto result = reinterpret_cast<CreateFactory2>(state().factory2.original)(flags, iid, output);
    trace("factory2", "\"hr\":\"%08X\",\"flags\":%u", static_cast<unsigned>(result), flags);
    if (SUCCEEDED(result) && output && *output) observeFactory(static_cast<IUnknown*>(*output));
    return result;
}
using CreateChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE createChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                       IDXGISwapChain** output) {
    trace("chain.createLegacy.begin");
    const auto result = reinterpret_cast<CreateChain>(state().createChain.original)(self, device, desc, output);
    trace("chain.createLegacy.end", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    if (SUCCEEDED(result) && output && *output) observeChain(*output, device);
    return result;
}
HRESULT STDMETHODCALLTYPE createHwnd(IDXGIFactory2* self, IUnknown* device, HWND window,
                                      const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
                                      IDXGIOutput* restrictOutput, IDXGISwapChain1** output) {
    trace("chain.createHwnd.begin", "\"hwnd\":\"%p\"", window);
    if (state().mode == 1 && desc && output && targetWindow(window)) {
        ComPtr<ID3D11Device> renderingDevice;
        if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&renderingDevice)))) {
            dspaa::PresentRetirement retired = dspaa::PresentRetirement::Quarantined;
            const auto result = dspaa::createPresentationFacade(self, renderingDevice.Get(), window, *desc, full,
                restrictOutput, state().runtime, facadeLog, output, &retired);
            trace("chain.facade", "\"hr\":\"%08X\",\"retirement\":\"%s\"", static_cast<unsigned>(result),
                  retired == dspaa::PresentRetirement::Drained ? "drained" : "quarantined");
            if (SUCCEEDED(result) || retired == dspaa::PresentRetirement::Quarantined) return result;
            // The complete temporary owner proved retirement. Only now may the
            // original D3D11 path create its chain on this HWND.
        }
    }
    const auto result = reinterpret_cast<CreateHwnd>(state().createHwnd.original)(
        self, device, window, desc, full, restrictOutput, output);
    trace("chain.createHwnd.end", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    if (SUCCEEDED(result) && output && *output) observeChain(*output, device);
    return result;
}
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using GetBuffer = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Fullscreen = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using Index = UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
using ColorSpace = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using Hdr = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*, DXGI_HDR_METADATA_TYPE, UINT, void*);
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* self, UINT interval, UINT flags) {
    const auto n = state().presents.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool record = n <= 16 || n % 300 == 0 || (flags & DXGI_PRESENT_TEST);
    if (record) trace("present.begin", "\"chain\":\"%p\",\"count\":%llu,\"interval\":%u,\"flags\":%u",
                       self, n, interval, flags);
    const auto result = reinterpret_cast<Present>(state().present.original)(self, interval, flags);
    if (record || FAILED(result)) trace("present.end", "\"count\":%llu,\"hr\":\"%08X\"", n, static_cast<unsigned>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* self, UINT interval, UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* parameters) {
    const auto n = state().presents.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool record = n <= 16 || n % 300 == 0 || (flags & DXGI_PRESENT_TEST);
    if (record) trace("present1.begin", "\"chain\":\"%p\",\"count\":%llu,\"interval\":%u,\"flags\":%u,\"dirtyRects\":%u",
                       self, n, interval, flags, parameters ? parameters->DirtyRectsCount : 0);
    const auto result = reinterpret_cast<Present1>(state().present1.original)(self, interval, flags, parameters);
    if (record || FAILED(result)) trace("present1.end", "\"count\":%llu,\"hr\":\"%08X\"", n, static_cast<unsigned>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE getBuffer(IDXGISwapChain* self, UINT buffer, REFIID iid, void** output) {
    const auto result = reinterpret_cast<GetBuffer>(state().getBuffer.original)(self, buffer, iid, output);
    trace("buffer", "\"chain\":\"%p\",\"index\":%u,\"iidData1\":\"%08lX\",\"texture11\":%s,\"hr\":\"%08X\"",
          self, buffer, iid.Data1, iid == __uuidof(ID3D11Texture2D) ? "true" : "false", static_cast<unsigned>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    trace("resize.begin", "\"chain\":\"%p\",\"count\":%u,\"width\":%u,\"height\":%u,\"format\":%u,\"flags\":%u",
          self, count, width, height, static_cast<unsigned>(format), flags);
    const auto result = reinterpret_cast<Resize>(state().resize.original)(self, count, width, height, format, flags);
    trace("resize.end", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE fullscreen(IDXGISwapChain* self, BOOL enabled, IDXGIOutput* output) {
    trace("fullscreen.begin", "\"enabled\":%s", enabled ? "true" : "false");
    const auto result = reinterpret_cast<Fullscreen>(state().fullscreen.original)(self, enabled, output);
    trace("fullscreen.end", "\"hr\":\"%08X\"", static_cast<unsigned>(result));
    return result;
}
UINT STDMETHODCALLTYPE index(IDXGISwapChain3* self) {
    const auto result = reinterpret_cast<Index>(state().index.original)(self);
    if (state().presents.load(std::memory_order_relaxed) < 16) trace("buffer.current", "\"index\":%u", result);
    return result;
}
HRESULT STDMETHODCALLTYPE colorSpace(IDXGISwapChain3* self, DXGI_COLOR_SPACE_TYPE value) {
    const auto result = reinterpret_cast<ColorSpace>(state().colorSpace.original)(self, value);
    trace("colorSpace", "\"space\":%u,\"hr\":\"%08X\"", static_cast<unsigned>(value), static_cast<unsigned>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE hdr(IDXGISwapChain4* self, DXGI_HDR_METADATA_TYPE type, UINT size, void* data) {
    const auto result = reinterpret_cast<Hdr>(state().hdr.original)(self, type, size, data);
    trace("hdr", "\"type\":%u,\"size\":%u,\"hr\":\"%08X\"", static_cast<unsigned>(type), size, static_cast<unsigned>(result));
    return result;
}
void observeFactory(IUnknown* object) noexcept {
    try {
        ComPtr<IDXGIFactory> factory;
        if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&factory)))) return;
        auto& s = state();
        std::lock_guard lock(s.installMutex);
        install(s.createChain, method(factory.Get(), 10), reinterpret_cast<void*>(createChain), "CreateSwapChain");
        ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(factory.As(&factory2)))
            install(s.createHwnd, method(factory2.Get(), 15), reinterpret_cast<void*>(createHwnd), "CreateSwapChainForHwnd");
        apply();
    } catch (...) { trace("factory.observeFailed"); }
}
void observeChain(IDXGISwapChain* chain, IUnknown* suppliedDevice) noexcept {
    try {
        ComPtr<IUnknown> facade;
        if (SUCCEEDED(chain->QueryInterface(dspaa::presentationFacadeId, reinterpret_cast<void**>(facade.GetAddressOf())))) return;
        ComPtr<ID3D11Device> device;
        if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) {
            trace("chain.nonD3D11", "\"chain\":\"%p\",\"suppliedDevice\":\"%p\"", chain, suppliedDevice);
            return;
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(chain->GetDesc(&desc))) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(desc.OutputWindow, &pid);
        if (pid != GetCurrentProcessId()) return;
        trace("chain.observed", "\"chain\":\"%p\",\"hwnd\":\"%p\",\"device\":\"%p\",\"width\":%u,\"height\":%u,\"format\":%u,\"buffers\":%u,\"effect\":%u,\"flags\":%u,\"windowed\":%s",
              chain, desc.OutputWindow, device.Get(), desc.BufferDesc.Width, desc.BufferDesc.Height,
              static_cast<unsigned>(desc.BufferDesc.Format), desc.BufferCount, static_cast<unsigned>(desc.SwapEffect),
              desc.Flags, desc.Windowed ? "true" : "false");
        auto& s = state();
        std::lock_guard lock(s.installMutex);
        install(s.present, method(chain, 8), reinterpret_cast<void*>(present), "Present");
        install(s.getBuffer, method(chain, 9), reinterpret_cast<void*>(getBuffer), "GetBuffer");
        install(s.fullscreen, method(chain, 10), reinterpret_cast<void*>(fullscreen), "SetFullscreenState");
        install(s.resize, method(chain, 13), reinterpret_cast<void*>(resize), "ResizeBuffers");
        ComPtr<IDXGISwapChain1> chain1;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))))
            install(s.present1, method(chain1.Get(), 22), reinterpret_cast<void*>(present1), "Present1");
        ComPtr<IDXGISwapChain3> chain3;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
            install(s.index, method(chain3.Get(), 36), reinterpret_cast<void*>(index), "GetCurrentBackBufferIndex");
            install(s.colorSpace, method(chain3.Get(), 38), reinterpret_cast<void*>(colorSpace), "SetColorSpace1");
        }
        ComPtr<IDXGISwapChain4> chain4;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain4))))
            install(s.hdr, method(chain4.Get(), 40), reinterpret_cast<void*>(hdr), "SetHDRMetaData");
        apply();
    } catch (...) { trace("chain.observeFailed"); }
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl DspAaBootstrapPresentation(
    uint32_t abi, const wchar_t* runtimeDirectory, const wchar_t* dataDirectory, uint32_t mode) noexcept {
    try {
        if (abi != dspaaPresentationBootstrapAbi || !runtimeDirectory || !dataDirectory || mode > 1) return 0;
        auto& s = state();
        unsigned expected = 0;
        if (!s.initialization.compare_exchange_strong(expected, 1)) return expected == 2 ? 1 : 0;
        s.mode = mode;
        s.runtime = std::filesystem::absolute(runtimeDirectory);
        std::filesystem::create_directories(dataDirectory);
        const auto path = std::filesystem::path(dataDirectory) / "presentation.jsonl";
        s.log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s.log == INVALID_HANDLE_VALUE) return 0;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        trace("preinit", "\"abi\":%u,\"mode\":\"%s\",\"pid\":%lu,\"frequency\":%lld",
              abi, mode ? "present" : "trace", GetCurrentProcessId(), frequency.QuadPart);
        if (mode) dspaa::initializePresentationRuntime(s.runtime, dataDirectory, facadeLog);
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK) {
            trace("hook.initializeFailed", "\"status\":\"%s\"", MH_StatusToString(initialized));
            return 0;
        }
        const auto dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dxgi || !d3d11) return 0;
        std::lock_guard lock(s.installMutex);
        bool good = install(s.device, reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")),
                             reinterpret_cast<void*>(createDevice), "D3D11CreateDevice");
        good = install(s.deviceChain, reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")),
                        reinterpret_cast<void*>(createDeviceChain), "D3D11CreateDeviceAndSwapChain") && good;
        good = install(s.factory, reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory")),
                        reinterpret_cast<void*>(createFactory), "CreateDXGIFactory") && good;
        const auto factory1 = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (factory1 == s.factory.target) {
            s.factory1.target = s.factory.target;
            s.factory1.original = s.factory.original;
        } else {
            good = install(s.factory1, factory1, reinterpret_cast<void*>(createFactory1), "CreateDXGIFactory1") && good;
        }
        good = install(s.factory2, reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")),
                        reinterpret_cast<void*>(createFactory2), "CreateDXGIFactory2") && good;
        const bool applied = apply();
        s.initialization.store(good && applied ? 2 : 3);
        trace("preinit.complete", "\"success\":%s", good && applied ? "true" : "false");
        return good && applied ? 1 : 0;
    } catch (...) {
        trace("preinit.failed");
        return 0;
    }
}
