#include "bridge/api.h"
#include "bridge/output-backup.h"
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, DXGI_FORMAT format, UINT flags,
                                const void* pixels = nullptr, UINT width = 4, UINT height = 4) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = flags;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels;
    data.SysMemPitch = width * 8;
    ComPtr<ID3D11Texture2D> result;
    require(SUCCEEDED(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result)),
            "Create WARP texture");
    return result;
}
void execute(void* token) {
    require(token != nullptr, "Expected queued command");
    // Main-thread submission and render-thread consumption are separate, joined
    // semantically rather than by sleeping or asserting a timing deadline.
    std::thread render([token] { DspAaGetRenderEvent()(1, token); });
    render.join();
}

void exactImage(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* output,
                const std::vector<uint16_t>& expected) {
    D3D11_TEXTURE2D_DESC desc{};
    output->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Create exact image readback");
    context->CopyResource(staging.Get(), output);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Map exact image");
    bool identical = expected.size() == static_cast<size_t>(desc.Width) * desc.Height * 4;
    for (UINT y = 0; identical && y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                                            y * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width * 4; ++x)
            identical &= row[x] == expected[static_cast<size_t>(y) * desc.Width * 4 + x];
    }
    context->Unmap(staging.Get(), 0);
    require(identical, "Output did not preserve exact full-resolution fallback pixels");
}

class Lifetime final : public IUnknown {
  public:
    explicit Lifetime(std::shared_ptr<std::atomic<bool>> destroyed) : destroyed_(std::move(destroyed)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *output = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) {
            *destroyed_ = true;
            delete this;
        }
        return remaining;
    }

  private:
    std::atomic<ULONG> references_{1};
    std::shared_ptr<std::atomic<bool>> destroyed_;
};

void supportTests(ID3D11Device* device, bool supported) {
    DspAaSupport result{};
    result.size = sizeof(result);
    require(!DspAaQueueSupport(nullptr) && !DspAaGetSupport(nullptr, &result),
            "Null capability query accepted");
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto anchor = texture(device, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    constexpr GUID key{0x6cfc7e20, 0x1caa, 0x4528, {0xa9, 0x82, 0x24, 0xa6, 0x9e, 0xd8, 0x11, 0x71}};
    auto* sentinel = new Lifetime(destroyed);
    require(SUCCEEDED(anchor->SetPrivateDataInterface(key, sentinel)), "Attach support lifetime sentinel");
    sentinel->Release();
    auto token = DspAaQueueSupport(anchor.Get());
    require(token && DspAaGetSupport(token, &result) && result.result == 0,
            "Capability query completed before render consumption");
    anchor.Reset();
    require(!*destroyed, "Capability queue did not retain its resource");
    execute(token);
    require(*destroyed, "Capability completion leaked its device anchor");
    result.size--;
    require(!DspAaGetSupport(token, &result) && !DspAaGetSupport(token, nullptr),
            "Invalid capability output accepted");
    result.size++;
    require(DspAaGetSupport(token, &result), "Missing capability completion");
    if (result.result != (supported ? 1 : -1))
        throw std::runtime_error(std::string("Unexpected capability result: ") + result.message);
    require(supported || result.message[0] != '\0', "Unavailable device has no reason");
    require(!DspAaGetSupport(token, &result), "Capability completion was not consumed exactly once");
    execute(token);
    require(!DspAaGetSupport(token, &result), "Duplicate event recreated a consumed capability result");

    anchor = texture(device, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    token = DspAaQueueSupport(anchor.Get());
    DspAaCancel(token);
    execute(token);
    require(!DspAaGetSupport(token, &result), "Cancelled capability query executed or leaked its record");
    std::vector<void*> pending;
    for (unsigned i = 0; i < 1024; ++i) {
        auto next = DspAaQueueSupport(anchor.Get());
        if (!next)
            break;
        pending.push_back(next);
    }
    require(!pending.empty() && pending.size() < 1024, "Capability queries have no bounded backpressure");
    for (auto next : pending)
        execute(next);
    require(!DspAaQueueSupport(anchor.Get()), "Unread capability completions grew without a bound");
    for (auto next : pending)
        require(DspAaGetSupport(next, &result), "Bounded capability result lost");
    token = DspAaQueueSupport(anchor.Get());
    require(token != nullptr, "Consuming capabilities did not free query capacity");
    DspAaCancel(token);
}

void queryTests(ID3D11Device* device, DspAaFrame frame) {
    DspAaOptimalSettings settings{};
    settings.size = sizeof(settings);
    require(!DspAaGetOptimalSettings(91, &settings), "Unexpected prior query status");
    for (const auto invalid :
         std::array<std::array<uint32_t, 3>, 4>{{{0, 720, 1}, {1280, 0, 1}, {16385, 720, 1}, {1280, 720, 5}}})
        require(!DspAaQueueOptimalSettings(91, frame.color, invalid[0], invalid[1], invalid[2]),
                "Invalid query accepted");
    require(!DspAaQueueOptimalSettings(0, frame.color, 1280, 720, 1) &&
                !DspAaQueueOptimalSettings(91, nullptr, 1280, 720, 1),
            "Invalid query identity/resource accepted");
    auto first = DspAaQueueOptimalSettings(91, frame.color, 1280, 720, 0);
    require(first && DspAaGetOptimalSettings(91, &settings) && settings.result == 0,
            "Query not pending at acceptance");
    execute(first);
    require(DspAaGetOptimalSettings(91, &settings) && settings.result == 1 && settings.optimalWidth == 1280 &&
                settings.optimalHeight == 720 && settings.minWidth == 1280 && settings.maxHeight == 720,
            "DLAA query must return 1:1 dimensions");
    auto old = DspAaQueueOptimalSettings(91, frame.color, 640, 360, 0);
    auto newer = DspAaQueueOptimalSettings(91, frame.color, 1920, 1080, 0);
    require(DspAaGetOptimalSettings(91, &settings) && settings.result == 0 && settings.outputWidth == 1920,
            "New query exposed stale ready settings");
    execute(old);
    require(DspAaGetOptimalSettings(91, &settings) && settings.result == 0 && settings.outputWidth == 1920,
            "Old query completion replaced newer pending settings");
    execute(newer);
    require(DspAaGetOptimalSettings(91, &settings) && settings.result == 1 && settings.optimalWidth == 1920,
            "Newest query did not complete");
    auto superseded = DspAaQueueOptimalSettings(91, frame.color, 640, 360, 0);
    auto current = DspAaQueueOptimalSettings(91, frame.color, 800, 600, 0);
    DspAaCancel(superseded);
    require(DspAaGetOptimalSettings(91, &settings) && settings.result == 0 && settings.outputWidth == 800,
            "Cancelling old query removed newer pending settings");
    DspAaCancel(current);
    require(!DspAaGetOptimalSettings(91, &settings), "Cancelled query leaked pending status");
    execute(superseded);
    execute(current);
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto anchor = texture(device, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    constexpr GUID key{0xd0dd6ac1, 0xda77, 0x4d35, {0x94, 0xa9, 0x87, 0x5d, 0x3b, 0xd6, 0x7e, 0x99}};
    auto* lifetime = new Lifetime(destroyed);
    require(SUCCEEDED(anchor->SetPrivateDataInterface(key, lifetime)), "Attach lifetime sentinel");
    lifetime->Release();
    auto retained = DspAaQueueOptimalSettings(92, anchor.Get(), 1280, 720, 1);
    require(retained != nullptr, "SR query rejected before render-thread consumption");
    anchor.Reset();
    require(!*destroyed, "Query failed to retain its anchor resource");
    DspAaCancel(retained); // Do not execute an SR capability query on WARP.
    require(*destroyed && !DspAaGetOptimalSettings(92, &settings),
            "Query cancellation leaked its resource/status");
    execute(DspAaQueueOptimalSettings(91, frame.color, 1280, 720, 0));
    std::vector<void*> pending;
    for (unsigned i = 0; i < 1024; ++i) {
        auto token = DspAaQueueOptimalSettings(100 + i, frame.color, 1280, 720, 1);
        if (!token)
            break;
        pending.push_back(token);
    }
    require(!pending.empty() && pending.size() < 1024 && !DspAaQueueFrame(&frame),
            "Queries escaped shared backpressure");
    auto release = DspAaQueueRelease(91);
    require(release != nullptr, "Query backpressure blocked release");
    for (auto token : pending)
        DspAaCancel(token);
    execute(release);
    require(!DspAaGetOptimalSettings(91, &settings), "Release leaked query settings");
    settings.size--;
    require(!DspAaGetOptimalSettings(91, &settings) && !DspAaGetOptimalSettings(91, nullptr),
            "Invalid query ABI accepted");
}

void superResolutionTests(ID3D11Device* device, ID3D11DeviceContext* context, DspAaFrame frame,
                          DXGI_FORMAT colorFormat, DXGI_FORMAT motionFormat, bool typeless) {
    auto color = texture(device, colorFormat, D3D11_BIND_SHADER_RESOURCE);
    auto depth = texture(device, typeless ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                         D3D11_BIND_SHADER_RESOURCE);
    auto motion = texture(device, motionFormat, D3D11_BIND_SHADER_RESOURCE);
    std::vector<uint16_t> prefill(8 * 6 * 4);
    for (size_t i = 0; i < prefill.size(); ++i)
        prefill[i] = static_cast<uint16_t>(0x3000 + i);
    auto output = texture(device, colorFormat, D3D11_BIND_UNORDERED_ACCESS, prefill.data(), 8, 6);
    frame.camera = 93;
    frame.color = color.Get();
    frame.output = output.Get();
    frame.depth = depth.Get();
    frame.motion = motion.Get();
    frame.outputWidth = 8;
    frame.outputHeight = 6;
    frame.quality = 1;
    // All four roles must validate at their proper unequal dimensions, then stop
    // before NGX: invalid temporal parameters make the test hardware-independent.
    frame.jitterX = std::numeric_limits<float>::quiet_NaN();
    DspAaStatus status{};
    status.size = sizeof(status);
    execute(DspAaQueueFrame(&frame));
    require(DspAaGetStatus(frame.camera, &status) && status.result == -1 &&
                std::string(status.message).find("temporal") != std::string::npos,
            "Valid unequal input/output roles rejected");
    exactImage(device, context, output.Get(), prefill);
    frame.outputWidth = 7;
    execute(DspAaQueueFrame(&frame));
    require(DspAaGetStatus(frame.camera, &status) &&
                std::string(status.message).find("output texture mismatch") != std::string::npos,
            "Wrong output dimensions escaped validation");
    frame.outputWidth = 8;
    auto wrongMotion = texture(device, motionFormat, D3D11_BIND_SHADER_RESOURCE, nullptr, 8, 6);
    frame.motion = wrongMotion.Get();
    execute(DspAaQueueFrame(&frame));
    require(DspAaGetStatus(frame.camera, &status) &&
                std::string(status.message).find("motion texture mismatch") != std::string::npos,
            "Output-sized motion vectors escaped low-resolution validation");
    exactImage(device, context, output.Get(), prefill);
    frame.motion = motion.Get();
    D3D11_TEXTURE2D_DESC base{};
    color->GetDesc(&base);
    std::vector<D3D11_TEXTURE2D_DESC> invalid;
    auto desc = base;
    desc.Width = 3;
    invalid.push_back(desc);
    desc = base;
    desc.MipLevels = 2;
    invalid.push_back(desc);
    desc = base;
    desc.ArraySize = 2;
    invalid.push_back(desc);
    desc = base;
    desc.BindFlags = 0;
    invalid.push_back(desc);
    desc = base;
    desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
    invalid.push_back(desc);
    UINT sampleQualities = 0;
    require(
        SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, 2, &sampleQualities)),
        "Check WARP multisampling");
    if (sampleQualities) {
        desc = base;
        desc.SampleDesc.Count = 2;
        invalid.push_back(desc);
    }
    for (const auto& wrong : invalid) {
        ComPtr<ID3D11Texture2D> resource;
        require(SUCCEEDED(device->CreateTexture2D(&wrong, nullptr, &resource)),
                "Create invalid-role texture");
        frame.color = resource.Get();
        execute(DspAaQueueFrame(&frame));
        require(DspAaGetStatus(frame.camera, &status) && status.result == -1 &&
                    std::string(status.message).find("color texture mismatch") != std::string::npos,
                "Strict color descriptor validation was relaxed");
        exactImage(device, context, output.Get(), prefill);
    }
    frame.color = color.Get();
    ComPtr<ID3D11Device> otherDevice;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                        D3D11_SDK_VERSION, &otherDevice, nullptr, nullptr)),
            "Create second WARP device");
    auto foreignDepth = texture(otherDevice.Get(), DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    frame.depth = foreignDepth.Get();
    execute(DspAaQueueFrame(&frame));
    require(DspAaGetStatus(frame.camera, &status) &&
                std::string(status.message).find("different devices") != std::string::npos,
            "Cross-device frame resource accepted");
    exactImage(device, context, output.Get(), prefill);
    frame.depth = depth.Get();
    frame.quality = 0;
    require(!DspAaQueueFrame(&frame), "Unequal DLAA dimensions accepted");
    frame.quality = 5;
    require(!DspAaQueueFrame(&frame), "Invalid quality accepted");
    frame.quality = 1;
    frame.reserved2 = 1;
    require(!DspAaQueueFrame(&frame), "New reserved field accepted");
    frame.reserved2 = 0;
    frame.outputWidth = 0;
    require(!DspAaQueueFrame(&frame), "Zero output dimension accepted");
    // Exercise the production backup with a real partial output overwrite, not a
    // mock CopyResource or a second implementation of fallback behavior.
    dspaa::OutputBackup backup;
    backup.capture(context, output.Get());
    std::array<uint16_t, 4> damaged{0, 0, 0, 0};
    D3D11_BOX box{2, 1, 0, 3, 2, 1};
    context->UpdateSubresource(output.Get(), 0, &box, damaged.data(), 8, 0);
    backup.restore(context, output.Get());
    exactImage(device, context, output.Get(), prefill);
    for (auto& pixel : prefill)
        ++pixel;
    context->UpdateSubresource(output.Get(), 0, nullptr, prefill.data(), 8 * 8, 0);
    backup.capture(context, output.Get());
    context->UpdateSubresource(output.Get(), 0, &box, damaged.data(), 8, 0);
    backup.restore(context, output.Get());
    exactImage(device, context, output.Get(), prefill); // Fresh capture, not the previous frame.
    execute(DspAaQueueRelease(frame.camera));
}

void fsrExtensionTests(ID3D11Device* device, ID3D11DeviceContext* context, DspAaFrame frame,
                       DXGI_FORMAT format) {
    frame.camera = 95;
    frame.quality = 1;
    frame.outputWidth = 8;
    frame.outputHeight = 6;
    std::vector<uint16_t> pixels(8 * 6 * 4, 0x3400);
    auto output = texture(device, format, D3D11_BIND_UNORDERED_ACCESS, pixels.data(), 8, 6);
    frame.output = output.Get();
    DspAaFsrParameters parameters{sizeof(DspAaFsrParameters), 0.1f, 1000.f, 1.f, 1.f, 1.f, 0.f, 0, nullptr};
    require(!DspAaQueueFsrFrame(&frame, nullptr), "FSR accepted absent parameters");
    --parameters.size;
    require(!DspAaQueueFsrFrame(&frame, &parameters), "FSR accepted a wrong parameter ABI");
    ++parameters.size;
    parameters.reserved = 1;
    require(!DspAaQueueFsrFrame(&frame, &parameters), "FSR accepted reserved data");
    parameters.reserved = 0;
    auto opaque = texture(device, format, D3D11_BIND_SHADER_RESOURCE);
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    constexpr GUID key{0x60427d5a, 0x8136, 0x48ea, {0x80, 0x7f, 0x12, 0x86, 0x44, 0x30, 0x51, 0x2b}};
    auto* lifetime = new Lifetime(destroyed);
    require(SUCCEEDED(opaque->SetPrivateDataInterface(key, lifetime)), "Attach FSR opaque lifetime sentinel");
    lifetime->Release();
    parameters.opaqueColor = opaque.Get();
    auto token = DspAaQueueFsrFrame(&frame, &parameters);
    require(token != nullptr, "FSR queue rejected a structurally valid packet");
    opaque.Reset();
    parameters.opaqueColor = nullptr;
    require(!*destroyed, "FSR queue did not retain its opaque input");
    execute(token); // Deliberately wrong depth stops before either vendor runtime initializes.
    require(*destroyed, "FSR consumption leaked its optional input");
    DspAaStatus status{};
    status.size = sizeof(status);
    require(DspAaGetStatus(95, &status) && status.result == -1 &&
                std::string(status.message).find("depth") != std::string::npos,
            "FSR did not report the rejected input role");
    exactImage(device, context, output.Get(), pixels);
    DspAaFsrOptimalSettings query{};
    query.settings.size = sizeof(query);
    token = DspAaQueueOptimalSettingsForBackend(95, output.Get(), 1280, 720, 1, 1);
    require(token && DspAaGetFsrOptimalSettings(95, &query) && query.settings.result == 0 &&
                query.jitterPhases == 0,
            "FSR sizing did not expose a pending backend-specific result");
    DspAaCancel(token);
    execute(token);
    require(!DspAaGetFsrOptimalSettings(95, &query), "FSR cancellation left a sizing result");
    require(!DspAaQueueSupportForBackend(output.Get(), 2) &&
                !DspAaQueueOptimalSettingsForBackend(95, output.Get(), 1280, 720, 1, 2),
            "Unknown reconstruction backend was accepted");
    execute(DspAaQueueRelease(95));
    require(DspAaGetStatus(95, &status) && status.result == 2, "FSR camera retirement missing");
}

} // namespace
int wmain(int argc, wchar_t** argv) {
    fs::path data;
    try {
        const bool typeless = argc == 3 && std::wstring(argv[2]) == L"--typeless";
        const bool hardwareSupport = argc == 3 && std::wstring(argv[2]) == L"--hardware-support";
        require(argc == 2 || typeless || hardwareSupport,
                "Expected runtime directory and optional --typeless/--hardware-support");
        require(DspAaGetAbiVersion() == 2, "Unexpected bridge ABI");
        static_assert(sizeof(DspAaFrame) == 112 && offsetof(DspAaFrame, outputWidth) == 96);
        static_assert(sizeof(DspAaStatus) == 288 && sizeof(DspAaOptimalSettings) == 300 &&
                      sizeof(DspAaSupport) == 264);
        require(DspAaQueueFrame(nullptr) == nullptr, "Null frame accepted");
        require(DspAaQueueShutdown() == nullptr, "Uninitialized bridge accepted shutdown");
        require(!DspAaInitialize(nullptr, nullptr), "Null paths accepted");
        data =
            fs::temp_directory_path() / (L"DSPAAMod-bridge-test-" + std::to_wstring(GetCurrentProcessId()));
        require(!fs::exists(data), "Refusing preexisting test output");
        require(DspAaInitialize(argv[1], data.c_str()) == 1, "Initialize bridge paths");
        require(DspAaInitialize(argv[1], data.c_str()) == 0, "Double initialization accepted");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(
                    nullptr, hardwareSupport ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0,
                    nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)),
                "Create headless query device");
        supportTests(device.Get(), hardwareSupport);
        if (hardwareSupport) {
            execute(DspAaQueueShutdown());
            fs::remove_all(data);
            std::cout << "Real-device DLSS capability query, consumption, cancellation and lifetime passed. "
                         "No DLSS feature or window was created.\n";
            return 0;
        }
        std::array<uint16_t, 4 * 4 * 4> pixels{};
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<uint16_t>(0x3000 + i);
        const auto colorFormat =
            typeless ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto motionFormat = typeless ? DXGI_FORMAT_R16G16_TYPELESS : DXGI_FORMAT_R16G16_FLOAT;
        auto color = texture(device.Get(), colorFormat, D3D11_BIND_SHADER_RESOURCE, pixels.data());
        auto output = texture(device.Get(), colorFormat, D3D11_BIND_UNORDERED_ACCESS);
        auto motion = texture(device.Get(), motionFormat, D3D11_BIND_SHADER_RESOURCE);
        DspAaFrame frame{};
        frame.size = sizeof(frame);
        frame.version = 2;
        frame.camera = 7;
        frame.frame = 42;
        frame.width = frame.height = frame.outputWidth = frame.outputHeight = 4;
        frame.color = color.Get();
        frame.output = output.Get();
        frame.depth = color.Get(); // Deliberately wrong depth format: NGX must NEVER be initialized.
        frame.motion = motion.Get();
        frame.preset = 11;
        frame.size--;
        require(!DspAaQueueFrame(&frame), "Wrong struct size accepted");
        frame.size++;
        frame.reserved = 1;
        require(!DspAaQueueFrame(&frame), "Reserved bits accepted");
        frame.reserved = 0;
        DspAaStatus status{};
        status.size = sizeof(status);
        queryTests(device.Get(), frame);
        superResolutionTests(device.Get(), context.Get(), frame, colorFormat, motionFormat, typeless);
        fsrExtensionTests(device.Get(), context.Get(), frame, colorFormat);
        require(!DspAaGetStatus(frame.camera, &status), "Unexecuted frame has status");
        std::vector<void*> pending;
        for (int i = 0; i < 1024; ++i) {
            auto token = DspAaQueueFrame(&frame);
            if (!token)
                break;
            pending.push_back(token);
        }
        require(!pending.empty() && pending.size() < 1024 && !DspAaQueueFrame(&frame),
                "Queue did not apply finite backpressure");
        for (auto token : pending) {
            DspAaCancel(token);
            execute(token); // A cancelled token must not dereference resources or create status.
        }
        require(!DspAaGetStatus(frame.camera, &status), "Cancelled frame executed");
        auto shutdown = DspAaQueueShutdown();
        require(shutdown != nullptr && !DspAaQueueFrame(&frame), "Shutdown did not stop submissions");
        DspAaCancel(shutdown);
        auto token = DspAaQueueFrame(&frame);
        require(token != nullptr, "Cancelling shutdown did not reopen queue");
        DspAaGetRenderEvent()(99, token);
        require(!DspAaGetStatus(frame.camera, &status), "Unknown event ID consumed frame");
        color.Reset();
        motion.Reset(); // Only queued COM references now keep both inputs alive.
        execute(token);
        require(DspAaGetStatus(frame.camera, &status) && status.result == -1 && status.frame == 42,
                "Malformed depth must produce a recoverable fallback status");
        const std::string diagnostic = status.message;
        require(diagnostic.find("depth") != std::string::npos &&
                    diagnostic.find("expected") != std::string::npos &&
                    diagnostic.find("got") != std::string::npos,
                "A rejected texture did not identify its role and expected/actual descriptor");
        std::ifstream logFile(data / "ngx.log");
        const std::string log((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
        logFile.close();
        for (const char* role : {"color:", "output:", "depth:", "motion:"})
            require(log.find(role) != std::string::npos, "A failed frame omitted a resource descriptor");
        require(log.find("fmt=" + std::to_string(colorFormat)) != std::string::npos &&
                    log.find("fmt=" + std::to_string(motionFormat)) != std::string::npos,
                "Failure diagnostics omitted the actual submitted resource formats");
        D3D11_TEXTURE2D_DESC desc{};
        output->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)), "Create readback");
        context->CopyResource(readback.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                "Read fallback image");
        bool identical = true;
        for (size_t y = 0; y < 4; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                                                y * mapped.RowPitch);
            for (size_t x = 0; x < 16; ++x)
                identical &= row[x] == pixels[y * 16 + x];
        }
        context->Unmap(readback.Get(), 0);
        require(identical, "Failure did not copy exact source pixels to output");
        execute(token); // Duplicate event after resources have gone must be harmless.
        execute(DspAaQueueRelease(frame.camera));
        require(DspAaGetStatus(frame.camera, &status) && status.result == 2,
                "Missing retirement acknowledgement");
        require(!DspAaGetStatus(frame.camera, &status), "Retired camera status leaked");
        auto pendingQuery = DspAaQueueOptimalSettings(94, output.Get(), 1280, 720, 1);
        require(pendingQuery != nullptr, "Queue query before shutdown");
        execute(DspAaQueueShutdown());
        DspAaOptimalSettings cleared{};
        cleared.size = sizeof(cleared);
        require(!DspAaGetOptimalSettings(94, &cleared), "Shutdown leaked pending query state");
        execute(pendingQuery); // Shutdown retired the retained resource and cancelled the token.
        require(!DspAaQueueRelease(7), "Shutdown left bridge ready");
        require(DspAaInitialize(argv[1], data.c_str()) == 1, "Clean shutdown prevented reinitialization");
        execute(DspAaQueueShutdown());
        fs::remove_all(data);
        std::cout << "Bridge queue, retained texture lifetime, GPU fallback, retirement and shutdown passed "
                     "on WARP.\n";
        return 0;
    } catch (const std::exception& error) {
        if (auto token = DspAaQueueShutdown())
            execute(token);
        std::cerr << error.what() << "\nTest artifacts retained: " << data << '\n';
        return 1;
    }
}
