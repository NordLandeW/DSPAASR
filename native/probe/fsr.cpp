#include "bridge/api.h"
#include "fsr/session.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
namespace {
constexpr unsigned width = 640, height = 360;
void execute(void* token) {
    if (!token)
        throw std::runtime_error("Native FSR command was rejected");
    DspAaGetRenderEvent()(1, token);
}
float halton(unsigned index, unsigned base) {
    float value = 0, fraction = 1;
    while (index) {
        fraction /= base;
        value += fraction * (index % base);
        index /= base;
    }
    return value - 0.5f;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, unsigned w, unsigned h, DXGI_FORMAT format,
                                UINT flags) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = w;
    description.Height = h;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = format;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = flags;
    ComPtr<ID3D11Texture2D> result;
    dspaa::graphicsCheck(device->CreateTexture2D(&description, nullptr, &result), "Create FSR probe texture");
    return result;
}
void save(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
          const std::filesystem::path& path) {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    dspaa::graphicsCheck(device->CreateTexture2D(&description, nullptr, &staging), "Create FSR readback");
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    dspaa::graphicsCheck(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read FSR output");
    std::ofstream image(path, std::ios::binary);
    image << "P6\n" << description.Width << ' ' << description.Height << "\n255\n";
    double sum = 0;
    unsigned nonfinite = 0;
    for (unsigned y = 0; y < description.Height; ++y) {
        const auto* row =
            reinterpret_cast<const HALF*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
        for (unsigned x = 0; x < description.Width; ++x)
            for (unsigned channel = 0; channel < 3; ++channel) {
                const auto value = XMConvertHalfToFloat(row[x * 4 + channel]);
                if (!std::isfinite(value)) {
                    ++nonfinite;
                    image.put(0);
                } else {
                    sum += value;
                    image.put(static_cast<char>(std::clamp(value, 0.f, 1.f) * 255));
                }
            }
    }
    context->Unmap(staging.Get(), 0);
    if (!image || nonfinite || sum <= 0)
        throw std::runtime_error("FSR output is empty/nonfinite or cannot be saved");
    std::cout << "output=" << path.filename().string() << " nonfinite=" << nonfinite
              << " mean=" << sum / (description.Width * description.Height * 3) << '\n';
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "Usage: fsr-probe <official-runtime-directory> <output-directory>\n"
                     "Headless real-adapter analytical FSR AA/SR, interoperability and lifecycle probe.\n";
        return 2;
    }
    try {
        const auto runtime = std::filesystem::absolute(argv[1]);
        const auto destination = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(destination);
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            std::cout << "d3d12_debug=enabled\n";
        } else
            std::cout << "d3d12_debug=unavailable\n";
        ComPtr<IDXGIFactory1> factory;
        dspaa::graphicsCheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create adapter factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (unsigned i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapters1(i, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            dspaa::graphicsCheck(result, "Enumerate graphics adapters");
            DXGI_ADAPTER_DESC1 description{};
            dspaa::graphicsCheck(candidate->GetDesc1(&description), "Read adapter identity");
            if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                std::wcout << L"adapter=" << description.Description << '\n';
                break;
            }
        }
        if (!adapter)
            throw std::runtime_error("No hardware adapter; this probe is not a default CTest");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        dspaa::graphicsCheck(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &device, nullptr, &context),
                             "Create synthetic D3D11 game device");
        if (!DspAaInitialize(runtime.c_str(), destination.c_str()))
            throw std::runtime_error("Initialize FSR-only runtime directory");
        auto anchor = texture(device.Get(), 2, 2, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        auto supportToken = DspAaQueueSupportForBackend(anchor.Get(), 1);
        execute(supportToken);
        DspAaSupport support{};
        support.size = sizeof(support);
        if (!DspAaGetSupport(supportToken, &support) || support.result != 1)
            throw std::runtime_error(std::string("FSR capability failed: ") + support.message);
        std::cout << "analytical_provider=" << support.message << '\n';
        // Invalid SDK render sizes must fail safely before the jitter query divides by renderWidth.
        for (const auto& request : std::array<std::array<unsigned, 2>, 3>{{{1, 1}, {1, 4}, {2, 4}}}) {
            execute(
                DspAaQueueOptimalSettingsForBackend(99, anchor.Get(), request[0], request[0], request[1], 1));
            DspAaFsrOptimalSettings invalid{};
            invalid.settings.size = sizeof(invalid);
            if (!DspAaGetFsrOptimalSettings(99, &invalid) || invalid.settings.result >= 0)
                throw std::runtime_error("FSR accepted an SDK-zero-sized SR request");
        }
        execute(DspAaQueueRelease(99));
        DspAaStatus edgeRetired{};
        edgeRetired.size = sizeof(edgeRetired);
        if (!DspAaGetStatus(99, &edgeRetired) || edgeRetired.result != 2)
            throw std::runtime_error("FSR edge-query retirement was not acknowledged");
        std::cout << "zero_render_size_requests=3 safely_rejected\n";
        // D3D12 devices are singleton-per-adapter; retain its debug queue across native shutdown.
        ComPtr<ID3D12Device> diagnosticDevice;
        dspaa::graphicsCheck(
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&diagnosticDevice)),
            "Get diagnostic device");
        ComPtr<ID3D12InfoQueue> diagnostics;
        (void)diagnosticDevice.As(&diagnostics);
        auto output = texture(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        {
            uint64_t frameIndex = 0;
            const std::array<unsigned, 7> stages{0, 1, 2, 3, 4, 1, 0};
            for (unsigned stage = 0; stage < stages.size(); ++stage) {
                const auto quality = stages[stage];
                execute(DspAaQueueOptimalSettingsForBackend(1, anchor.Get(), width, height, quality, 1));
                DspAaFsrOptimalSettings optimal{};
                optimal.settings.size = sizeof(optimal);
                if (!DspAaGetFsrOptimalSettings(1, &optimal) || optimal.settings.result != 1)
                    throw std::runtime_error(std::string("FSR dimensions failed: ") +
                                             optimal.settings.message);
                const dspaa::FsrResolution size{optimal.settings.optimalWidth, optimal.settings.optimalHeight,
                                                optimal.jitterPhases};
                if ((quality == 0) != (size.width == width && size.height == height))
                    throw std::runtime_error("FSR Native AA / SR dimensions do not match requested mode");
                const bool inverted = stage % 2 == 0, reactive = stage % 2 != 0;
                auto opaque = texture(device.Get(), size.width, size.height,
                                      DXGI_FORMAT_R16G16B16A16_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
                auto color = texture(device.Get(), size.width, size.height, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                     D3D11_BIND_SHADER_RESOURCE);
                auto depth = texture(device.Get(), size.width, size.height, DXGI_FORMAT_R32_TYPELESS,
                                     D3D11_BIND_SHADER_RESOURCE);
                auto motion = texture(device.Get(), size.width, size.height, DXGI_FORMAT_R16G16_TYPELESS,
                                      D3D11_BIND_SHADER_RESOURCE);
                std::vector<HALF> pixels(static_cast<size_t>(size.width) * size.height * 4);
                std::vector<float> depths(static_cast<size_t>(size.width) * size.height, 0.5f);
                std::vector<HALF> vectors(static_cast<size_t>(size.width) * size.height * 2, 0);
                context->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(), size.width * sizeof(float),
                                           0);
                context->UpdateSubresource(motion.Get(), 0, nullptr, vectors.data(),
                                           size.width * sizeof(HALF) * 2, 0);
                const unsigned frames = size.phases * 2;
                for (unsigned frame = 0; frame < frames; ++frame) {
                    const auto jx = halton(frame % size.phases + 1, 2),
                               jy = halton(frame % size.phases + 1, 3);
                    for (unsigned y = 0; y < size.height; ++y)
                        for (unsigned x = 0; x < size.width; ++x) {
                            const float sx = (x + 0.5f + jx) * width / size.width;
                            const float sy = (y + 0.5f + jy) * height / size.height;
                            const bool line = std::fmod(std::abs(sx - 0.73f * sy), 19.f) < 1.f;
                            const auto i = (static_cast<size_t>(y) * size.width + x) * 4;
                            pixels[i] = XMConvertFloatToHalf(line ? 2.f : 0.05f);
                            pixels[i + 1] = XMConvertFloatToHalf(0.1f + sx / width * 0.7f);
                            pixels[i + 2] = XMConvertFloatToHalf(0.1f + sy / height * 0.7f);
                            pixels[i + 3] = XMConvertFloatToHalf(1.f);
                        }
                    if (reactive) {
                        context->UpdateSubresource(opaque.Get(), 0, nullptr, pixels.data(),
                                                   size.width * sizeof(HALF) * 4, 0);
                        // A changing bright alpha-effect proxy without motion vectors exercises
                        // auto-reactivity.
                        for (unsigned y = size.height / 3; y < size.height / 2; ++y)
                            for (unsigned x = size.width / 3; x < size.width / 2; ++x)
                                pixels[(static_cast<size_t>(y) * size.width + x) * 4] =
                                    XMConvertFloatToHalf(frame % 2 ? 4.f : 2.f);
                    }
                    context->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(),
                                               size.width * sizeof(HALF) * 4, 0);
                    D3D11_VIEWPORT viewport{2, 3, 101, 103, 0.2f, 0.9f};
                    context->RSSetViewports(1, &viewport);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                    DspAaFrame input{};
                    input.size = sizeof(input);
                    input.version = 2;
                    input.camera = 1;
                    input.frame = ++frameIndex;
                    input.color = color.Get();
                    input.depth = depth.Get();
                    input.motion = motion.Get();
                    input.output = output.Get();
                    input.width = size.width;
                    input.height = size.height;
                    input.outputWidth = width;
                    input.outputHeight = height;
                    input.quality = quality;
                    input.jitterX = -jx;
                    input.jitterY = -jy;
                    input.motionScaleX = input.motionScaleY = 1;
                    input.frameTimeMilliseconds = 1000.f / 60;
                    input.flags = 1u | (inverted ? 2u : 0u) | (frame == frames / 2 ? 4u : 0u);
                    DspAaFsrParameters parameters{sizeof(DspAaFsrParameters),
                                                  0.1f,
                                                  1000.f,
                                                  1.04719755f,
                                                  1.f,
                                                  1.f,
                                                  reactive ? 0.25f : 0.f,
                                                  0,
                                                  reactive ? opaque.Get() : nullptr};
                    execute(DspAaQueueFsrFrame(&input, &parameters));
                    DspAaStatus status{};
                    status.size = sizeof(status);
                    if (!DspAaGetStatus(1, &status) || status.result != 1 || status.frame != frameIndex ||
                        status.observedPreset != 0)
                        throw std::runtime_error(std::string("Native FSR evaluation failed: ") +
                                                 status.message);
                    D3D11_VIEWPORT observed{};
                    UINT count = 1;
                    context->RSGetViewports(&count, &observed);
                    D3D11_PRIMITIVE_TOPOLOGY topology{};
                    context->IAGetPrimitiveTopology(&topology);
                    if (count != 1 || observed.Width != viewport.Width ||
                        observed.TopLeftX != viewport.TopLeftX ||
                        topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST)
                        throw std::runtime_error("FSR bridge changed sampled D3D11 pipeline state");
                }
                save(device.Get(), context.Get(), output.Get(),
                     destination / ("fsr-" + std::to_string(stage) + ".ppm"));
                std::cout << "quality=" << quality << " input=" << size.width << 'x' << size.height
                          << " phases=" << size.phases << " evaluated_frames=" << frames
                          << " inverted_depth=" << inverted << " reactive_mask=" << reactive
                          << " state_samples=preserved\n";
            }
            execute(DspAaQueueRelease(1));
            DspAaStatus retired{};
            retired.size = sizeof(retired);
            if (!DspAaGetStatus(1, &retired) || retired.result != 2)
                throw std::runtime_error("FSR retirement was not acknowledged");
        }
        execute(DspAaQueueShutdown());
        unsigned errors = 0;
        if (diagnostics) {
            for (uint64_t i = 0; i < diagnostics->GetNumStoredMessages(); ++i) {
                SIZE_T bytes = 0;
                diagnostics->GetMessage(i, nullptr, &bytes);
                std::vector<char> buffer(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                dspaa::graphicsCheck(diagnostics->GetMessage(i, message, &bytes), "Read D3D12 diagnostics");
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++errors;
                    std::cerr << "D3D12 validation: " << message->pDescription << '\n';
                }
            }
        }
        if (errors)
            throw std::runtime_error("D3D12 validation errors: " + std::to_string(errors));
        std::cout << "fsr_stages=7 gpu_retirement=complete d3d12_errors=" << errors << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (auto token = DspAaQueueShutdown())
            DspAaGetRenderEvent()(1, token);
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
