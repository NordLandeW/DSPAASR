#include <DirectXPackedVector.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "bridge/api.h"
#include "core/preset-evidence.h"
#include "ngx/session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using DirectX::PackedVector::HALF;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

namespace {
constexpr uint32_t width = 1280;
constexpr uint32_t height = 720;
constexpr unsigned frameCount = 32;
std::mutex logMutex;
dspaa::PresetEvidence presetEvidence;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << static_cast<uint32_t>(result);
        throw std::runtime_error(message.str());
    }
}

void NVSDK_CONV ngxLog(const char* text, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature) {
    std::lock_guard lock(logMutex);
    presetEvidence.observe(text ? text : "");
    std::cerr << "[NGX " << static_cast<int>(level) << '/' << static_cast<int>(feature) << "] "
              << (text ? text : "") << '\n';
}

float halton(unsigned index, unsigned base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index != 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result - 0.5f;
}

unsigned presetValue(const std::wstring& value) {
    if (value == L"E")
        return 5;
    if (value == L"F")
        return 6;
    if (value == L"K")
        return 11;
    if (value == L"L")
        return 12;
    if (value == L"M")
        return 13;
    throw std::runtime_error("Preset must be E, F, K, L, or M.");
}

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
                                UINT bindFlags, const void* data = nullptr, UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    initial.SysMemPitch = pitch;
    ComPtr<ID3D11Texture2D> result;
    check(device->CreateTexture2D(&desc, data ? &initial : nullptr, &result), "CreateTexture2D");
    return result;
}

void drawInput(std::vector<HALF>& pixels, float jitterX, float jitterY, uint32_t renderWidth = width,
               uint32_t renderHeight = height) {
    for (uint32_t y = 0; y < renderHeight; ++y) {
        for (uint32_t x = 0; x < renderWidth; ++x) {
            // Sample the scene directly at the true input resolution, not by
            // downsampling an already-rendered target-size image.
            const float sx = (static_cast<float>(x) + 0.5f + jitterX) * width / renderWidth;
            const float sy = (static_cast<float>(y) + 0.5f + jitterY) * height / renderHeight;
            const bool line = std::fmod(std::abs(sx - sy * 1.37f), 23.0f) < 0.9f;
            const bool grid = (static_cast<int>(sx / 17.0f) + static_cast<int>(sy / 17.0f)) % 2 == 0;
            const size_t offset = (static_cast<size_t>(y) * renderWidth + x) * 4;
            pixels[offset] = XMConvertFloatToHalf(line ? 2.0f : (grid ? 0.2f : 0.04f));
            pixels[offset + 1] = XMConvertFloatToHalf(line ? 0.5f : sx / static_cast<float>(width));
            pixels[offset + 2] = XMConvertFloatToHalf(line ? 0.1f : sy / static_cast<float>(height));
            pixels[offset + 3] = XMConvertFloatToHalf(1.0f);
        }
    }
}

void drawStaticBox(std::vector<HALF>& pixels, float jitterX, float jitterY) {
    // A static box sampled at Unity's projection jitter: geometry moves by -jitter
    // in render-texture pixels. Analytic pixel coverage avoids a threshold artifact.
    for (uint32_t y = 0; y < height; ++y) {
        const float sy = static_cast<float>(y) + 0.5f + jitterY;
        const float cy = std::clamp(sy - (height * 0.375f + 0.19f) + 0.5f, 0.0f, 1.0f) *
                         std::clamp(height * 0.625f + 0.19f - sy + 0.5f, 0.0f, 1.0f);
        for (uint32_t x = 0; x < width; ++x) {
            const float sx = static_cast<float>(x) + 0.5f + jitterX;
            const float cx = std::clamp(sx - (width * 0.375f + 0.37f) + 0.5f, 0.0f, 1.0f) *
                             std::clamp(width * 0.625f + 0.37f - sx + 0.5f, 0.0f, 1.0f);
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            float coverage = cx * cy;
            // Point-sampled thin diagonals exercise the high-frequency shimmer
            // which an already-filtered long box edge can hide.
            if (std::abs(sx - width * 0.5f) < 64.0f && std::abs(sy - height * 0.5f) < 64.0f)
                coverage = std::fmod(std::abs(sx - sy * 0.73f), 11.0f) < 0.8f ? 1.0f : 0.0f;
            pixels[i] = pixels[i + 1] = pixels[i + 2] = XMConvertFloatToHalf(0.1f + 0.8f * coverage);
            pixels[i + 3] = XMConvertFloatToHalf(1.0f);
        }
    }
}

std::array<double, 2> readBoxEdges(ID3D11DeviceContext* context, ID3D11Texture2D* output,
                                   ID3D11Texture2D* staging, std::vector<double>& patch) {
    context->CopyResource(staging, output);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Read edge position");
    const auto value = [&](uint32_t x, uint32_t y) {
        const auto* row = reinterpret_cast<const HALF*>(static_cast<const uint8_t*>(mapped.pData) +
                                                        static_cast<size_t>(y) * mapped.RowPitch);
        return (XMConvertHalfToFloat(row[x * 4]) - 0.1) / 0.8;
    };
    constexpr uint32_t edgeX = width * 3 / 8, edgeY = height * 3 / 8;
    std::array<double, 2> result{edgeX + 8.0, edgeY + 8.0};
    for (int offset = -8; offset < 8; ++offset) {
        result[0] -= value(static_cast<uint32_t>(static_cast<int>(edgeX) + offset), height / 2);
        result[1] -= value(width / 2, static_cast<uint32_t>(static_cast<int>(edgeY) + offset));
    }
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            patch[y * 64 + x] = value(width / 2 - 32 + x, height / 2 - 32 + y);
    context->Unmap(staging, 0);
    if (!std::isfinite(result[0]) || !std::isfinite(result[1]))
        throw std::runtime_error("Nonfinite edge position");
    return result;
}

void saveOutput(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* output,
                const fs::path& file);

void jitterStability(dspaa::DlssFeature& feature, ID3D11Device* device, ID3D11DeviceContext* context,
                     dspaa::DlssFrame input, std::vector<HALF>& pixels, const fs::path& directory) {
    auto* color = static_cast<ID3D11Texture2D*>(input.color);
    auto* output = static_cast<ID3D11Texture2D*>(input.output);
    D3D11_TEXTURE2D_DESC desc{};
    output->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "Create edge readback");
    std::ofstream csv(directory / "jitter-edges.csv");
    csv << "sign,frame,jitter_x,jitter_y,edge_x,edge_y\n" << std::setprecision(10);
    std::array<std::array<double, 2>, 2> deviations{};
    for (unsigned variant = 0; variant < 2; ++variant) {
        const float sign = variant == 0 ? 1.0f : -1.0f;
        std::array<double, 2> sum{}, squares{};
        std::vector<double> patch(64 * 64), pixelSum(patch.size()), pixelSquares(patch.size());
        for (unsigned frame = 0; frame < frameCount * 3; ++frame) {
            const float jx = halton(frame % frameCount + 1, 2);
            const float jy = halton(frame % frameCount + 1, 3);
            drawStaticBox(pixels, jx, jy);
            context->UpdateSubresource(color, 0, nullptr, pixels.data(), width * 4 * sizeof(HALF), 0);
            input.jitterX = sign * jx;
            input.jitterY = sign * jy;
            input.reset = frame == 0;
            feature.evaluate(input);
            if (frame < frameCount * 2)
                continue;
            const auto edges = readBoxEdges(context, output, staging.Get(), patch);
            for (size_t i = 0; i < patch.size(); ++i) {
                if (!std::isfinite(patch[i]))
                    throw std::runtime_error("Nonfinite detail sample");
                pixelSum[i] += patch[i];
                pixelSquares[i] += patch[i] * patch[i];
            }
            csv << sign << ',' << frame << ',' << jx << ',' << jy << ',' << edges[0] << ',' << edges[1]
                << '\n';
            for (size_t axis = 0; axis < 2; ++axis) {
                sum[axis] += edges[axis];
                squares[axis] += edges[axis] * edges[axis];
            }
        }
        for (size_t axis = 0; axis < 2; ++axis) {
            const double mean = sum[axis] / frameCount;
            deviations[variant][axis] = std::sqrt(std::max(0.0, squares[axis] / frameCount - mean * mean));
        }
        double variance = 0.0;
        for (size_t i = 0; i < patch.size(); ++i) {
            const double mean = pixelSum[i] / frameCount;
            variance += std::max(0.0, pixelSquares[i] / frameCount - mean * mean);
        }
        std::cout << "detail_temporal_stddev=" << std::sqrt(variance / patch.size()) << '\n';
        saveOutput(device, context, output, directory / (variant == 0 ? "positive.ppm" : "negative.ppm"));
        std::cout << "jitter_sign=" << sign << " measured_frames=" << frameCount
                  << " edge_x_stddev_pixels=" << deviations[variant][0]
                  << " edge_y_stddev_pixels=" << deviations[variant][1] << '\n';
    }
    if (!csv)
        throw std::runtime_error("Cannot write jitter measurements");
}

void saveOutput(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* output,
                const fs::path& file) {
    D3D11_TEXTURE2D_DESC desc{};
    output->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "Create readback");
    context->CopyResource(staging.Get(), output);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Readback");
    std::ofstream image(file, std::ios::binary);
    image << "P6\n" << width << ' ' << height << "\n255\n";
    uint64_t nonFinite = 0;
    double sum = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const HALF*>(static_cast<const uint8_t*>(mapped.pData) +
                                                        static_cast<size_t>(y) * mapped.RowPitch);
        for (uint32_t x = 0; x < width; ++x) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                float value = XMConvertHalfToFloat(row[x * 4 + channel]);
                if (!std::isfinite(value)) {
                    ++nonFinite;
                    value = 0.0f;
                }
                sum += value;
                const auto byte = static_cast<unsigned char>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
                image.put(static_cast<char>(byte));
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    if (!image)
        throw std::runtime_error("Cannot write output image.");
    std::cout << "output_nonfinite=" << nonFinite
              << " output_mean=" << sum / (static_cast<double>(width) * height * 3.0) << '\n';
    if (nonFinite != 0 || sum <= 0.0)
        throw std::runtime_error("DLSS produced invalid/empty output.");
}

void bridgeEvent(void* token) {
    if (!token)
        throw std::runtime_error("Bridge rejected command.");
    std::thread renderer([token] { DspAaGetRenderEvent()(1, token); });
    renderer.join();
}

void bridgeSuperResolution(ID3D11Device* device, ID3D11DeviceContext* context, const fs::path& directory) {
    struct Stage {
        uint32_t quality;
        uint32_t preset;
        unsigned phases;
    };
    constexpr Stage stages[] = {{0, 11, 32}, {1, 11, 32}, {2, 11, 32}, {3, 13, 32},
                                {4, 12, 72}, {1, 5, 32},  {1, 12, 32}, {1, 13, 32}};
    auto output = texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    std::vector<HALF> prefill(static_cast<size_t>(width) * height * 4);
    uint64_t frameIndex = 0;
    for (const auto stage : stages) {
        auto query = DspAaQueueOptimalSettings(1, output.Get(), width, height, stage.quality);
        DspAaOptimalSettings settings{};
        settings.size = sizeof(settings);
        if (!query || !DspAaGetOptimalSettings(1, &settings) || settings.result != 0)
            throw std::runtime_error("Optimal query did not publish pending state.");
        bridgeEvent(query);
        if (!DspAaGetOptimalSettings(1, &settings) || settings.result != 1 ||
            settings.quality != stage.quality || settings.outputWidth != width ||
            settings.outputHeight != height)
            throw std::runtime_error(std::string("Optimal query failed: ") + settings.message);
        const auto w = settings.optimalWidth, h = settings.optimalHeight;
        if ((stage.quality == 0 && (w != width || h != height)) ||
            (stage.quality != 0 && (w >= width || h >= height)))
            throw std::runtime_error("SDK resolution does not represent the requested DLAA/SR mode.");
        auto color = texture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        std::vector<float> depthValues(static_cast<size_t>(w) * h, 0.5f);
        auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                             depthValues.data(), w * sizeof(float));
        std::vector<HALF> motionValues(static_cast<size_t>(w) * h * 2, 0);
        auto motion = texture(device, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                              motionValues.data(), w * 2 * sizeof(HALF));
        std::vector<HALF> pixels(static_cast<size_t>(w) * h * 4);
        for (unsigned frame = 0; frame < stage.phases; ++frame) {
            const float jx = halton(frame + 1, 2), jy = halton(frame + 1, 3);
            drawInput(pixels, jx, jy, w, h);
            context->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(), w * 4 * sizeof(HALF), 0);
            // A simple nearest-neighbor spatial fallback from the actual low-res
            // samples. Never CopyResource between unequal source/output sizes.
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    const size_t source = (static_cast<size_t>(y * h / height) * w + x * w / width) * 4;
                    const size_t destination = (static_cast<size_t>(y) * width + x) * 4;
                    std::copy_n(pixels.data() + source, 4, prefill.data() + destination);
                }
            context->UpdateSubresource(output.Get(), 0, nullptr, prefill.data(), width * 4 * sizeof(HALF), 0);
            DspAaFrame packet{};
            packet.size = sizeof(packet);
            packet.version = 2;
            packet.camera = 1;
            packet.frame = ++frameIndex;
            packet.color = color.Get();
            packet.output = output.Get();
            packet.depth = depth.Get();
            packet.motion = motion.Get();
            packet.width = w;
            packet.height = h;
            packet.outputWidth = width;
            packet.outputHeight = height;
            packet.quality = stage.quality;
            packet.preset = stage.preset;
            packet.flags = 1 | (frame == 0 ? 4 : 0);
            packet.jitterX = -jx;
            packet.jitterY = -jy;
            packet.motionScaleX = packet.motionScaleY = 1;
            packet.frameTimeMilliseconds = 1000.0f / 60.0f;
            const D3D11_VIEWPORT viewport{2, 3, 101, 103, 0.2f, 0.9f};
            context->RSSetViewports(1, &viewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            context->CSSetShader(nullptr, nullptr, 0);
            bridgeEvent(DspAaQueueFrame(&packet));
            DspAaStatus status{};
            status.size = sizeof(status);
            if (!DspAaGetStatus(1, &status) || status.result != 1 || status.frame != frameIndex ||
                status.requestedPreset != stage.preset || status.observedPreset != 'A' + stage.preset - 1 ||
                status.verification != static_cast<unsigned>(dspaa::PresetVerdict::Matched))
                throw std::runtime_error(std::string("SR frame failed: ") + status.message);
            D3D11_VIEWPORT observed{};
            UINT count = 1;
            context->RSGetViewports(&count, &observed);
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->IAGetPrimitiveTopology(&topology);
            ComPtr<ID3D11ComputeShader> shader;
            context->CSGetShader(&shader, nullptr, nullptr);
            if (count != 1 || observed.Width != viewport.Width || observed.Height != viewport.Height ||
                observed.TopLeftX != viewport.TopLeftX || observed.TopLeftY != viewport.TopLeftY ||
                observed.MinDepth != viewport.MinDepth || observed.MaxDepth != viewport.MaxDepth ||
                topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST || shader)
                throw std::runtime_error("SR changed sampled D3D11 immediate-context state.");
        }
        const std::string name =
            "sr-" + std::to_string(stage.quality) + "-" + static_cast<char>('A' + stage.preset - 1);
        saveOutput(device, context, output.Get(), directory / (name + ".ppm"));
        std::cout << "sr_quality=" << stage.quality << " preset=" << static_cast<char>('A' + stage.preset - 1)
                  << " input=" << w << 'x' << h << " output=" << width << 'x' << height
                  << " range=" << settings.minWidth << 'x' << settings.minHeight << ".." << settings.maxWidth
                  << 'x' << settings.maxHeight << " frames=" << stage.phases
                  << " verification=matched-runtime-log state_samples=preserved\n";
    }
    bridgeEvent(DspAaQueueRelease(1));
    DspAaStatus retired{};
    retired.size = sizeof(retired);
    DspAaOptimalSettings settings{};
    settings.size = sizeof(settings);
    if (!DspAaGetStatus(1, &retired) || retired.result != 2 || DspAaGetOptimalSettings(1, &settings))
        throw std::runtime_error("SR release did not retire camera/query state.");
    bridgeEvent(DspAaQueueShutdown());
    std::cout << "bridge_super_resolution_stages=8 evaluated_frames=" << frameIndex << " shutdown=complete\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool typeless = argc == 5 && std::wstring(argv[4]) == L"--bridge-switch-typeless";
    const bool superResolution = argc == 5 && std::wstring(argv[4]) == L"--bridge-super-resolution";
    const bool bridge =
        typeless || superResolution || (argc == 5 && std::wstring(argv[4]) == L"--bridge-switch");
    const bool stability = argc == 5 && std::wstring(argv[4]) == L"--jitter-stability";
    if (argc != 4 && !bridge && !stability) {
        std::cerr
            << "Usage: ngx-probe <runtime-directory> <output-directory> <E|F|K|L|M> "
               "[--bridge-switch|--bridge-switch-typeless|--bridge-super-resolution|--jitter-stability]\n"
               "Headless, synthetic D3D11/DLSS execution probe. No game or driver settings modified.\n";
        return 2;
    }
    try {
        const auto runtime = fs::absolute(argv[1]);
        const auto outputDirectory = fs::absolute(argv[2]);
        const std::wstring presetName(argv[3]);
        const unsigned preset = presetValue(presetName);
        if (!fs::is_regular_file(runtime / "nvngx_dlss.dll"))
            throw std::runtime_error("Missing runtime DLL.");
        fs::create_directories(outputDirectory);
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto status = factory->EnumAdapters1(i, &candidate);
            if (status == DXGI_ERROR_NOT_FOUND)
                break;
            check(status, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 desc{};
            check(candidate->GetDesc1(&desc), "Get adapter description");
            if (desc.VendorId == 0x10de && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                adapter = candidate;
                std::wcout << L"adapter=" << desc.Description << L" requested_preset=" << presetName << L'\n';
                break;
            }
        }
        if (!adapter)
            throw std::runtime_error("No NVIDIA hardware adapter.");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL actualLevel{};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                D3D11_SDK_VERSION, &device, &actualLevel, &context),
              "D3D11CreateDevice");
        std::unique_ptr<dspaa::NgxDevice> ngx;
        std::unique_ptr<dspaa::DlssFeature> feature;
        if (bridge) {
            if (!DspAaInitialize(runtime.c_str(), outputDirectory.c_str()))
                throw std::runtime_error("Bridge initialization failed.");
        } else {
            ngx = std::make_unique<dspaa::NgxDevice>(device.Get(), runtime, outputDirectory, ngxLog);
            feature = std::make_unique<dspaa::DlssFeature>(*ngx);
        }
        if (superResolution) {
            bridgeSuperResolution(device.Get(), context.Get(), outputDirectory);
            return 0;
        }
        dspaa::DlssConfiguration configuration;
        configuration.inputWidth = configuration.outputWidth = width;
        configuration.inputHeight = configuration.outputHeight = height;
        configuration.preset = static_cast<NVSDK_NGX_DLSS_Hint_Render_Preset>(preset);
        configuration.autoExposure = false;
        if (feature)
            feature->configure(configuration);
        constexpr UINT colorPitch = width * 4 * sizeof(HALF);
        const auto colorFormat =
            typeless ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::cout << "typeless_resources=" << typeless << '\n';
        auto color = texture(device.Get(), width, height, colorFormat, D3D11_BIND_SHADER_RESOURCE);
        auto output = texture(device.Get(), width, height, colorFormat,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        std::vector<float> depthValues(static_cast<size_t>(width) * height, 0.5f);
        auto depth =
            texture(device.Get(), width, height, typeless ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                    D3D11_BIND_SHADER_RESOURCE, depthValues.data(), width * sizeof(float));
        std::vector<HALF> motionValues(static_cast<size_t>(width) * height * 2, 0);
        auto motion = texture(device.Get(), width, height,
                              typeless ? DXGI_FORMAT_R16G16_TYPELESS : DXGI_FORMAT_R16G16_FLOAT,
                              D3D11_BIND_SHADER_RESOURCE, motionValues.data(), width * 2 * sizeof(HALF));
        const float exposureValue = 1.0f;
        auto exposure = texture(device.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                                &exposureValue, sizeof(float));
        std::vector<HALF> pixels(static_cast<size_t>(width) * height * 4);
        if (stability) {
            dspaa::DlssFrame input;
            input.color = color.Get();
            input.output = output.Get();
            input.depth = depth.Get();
            input.motionVectors = motion.Get();
            input.exposure = exposure.Get();
            jitterStability(*feature, device.Get(), context.Get(), input, pixels, outputDirectory);
            std::lock_guard lock(logMutex);
            const auto verdict = presetEvidence.verify(static_cast<char>(presetName.front()));
            std::cout << "verification=" << dspaa::verdictName(verdict) << '\n';
            return verdict == dspaa::PresetVerdict::Matched ? 0 : 3;
        }
        const std::vector<unsigned> stages =
            bridge ? std::vector<unsigned>{preset, 6, 12, 13, 5, 11, preset} : std::vector<unsigned>{preset};
        for (const auto stage : stages) {
            for (unsigned frame = 0; frame < frameCount; ++frame) {
                const float jx = halton(frame + 1, 2);
                const float jy = halton(frame + 1, 3);
                drawInput(pixels, jx, jy);
                context->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(), colorPitch, 0);
                dspaa::DlssFrame evaluate;
                evaluate.color = color.Get();
                evaluate.output = output.Get();
                evaluate.depth = depth.Get();
                evaluate.motionVectors = motion.Get();
                evaluate.exposure = exposure.Get();
                evaluate.jitterX = -jx;
                evaluate.jitterY = -jy;
                evaluate.reset = frame == 0;
                if (feature)
                    feature->evaluate(evaluate);
                else {
                    DspAaFrame packet{};
                    packet.size = sizeof(packet);
                    packet.version = 2;
                    packet.camera = 1;
                    packet.frame = frame + 1;
                    packet.color = color.Get();
                    packet.output = output.Get();
                    packet.depth = depth.Get();
                    packet.motion = motion.Get();
                    packet.width = width;
                    packet.height = height;
                    packet.outputWidth = width;
                    packet.outputHeight = height;
                    packet.preset = stage;
                    packet.flags = 1 | (frame == 0 ? 4 : 0);
                    packet.jitterX = -jx;
                    packet.jitterY = -jy;
                    packet.motionScaleX = packet.motionScaleY = 1;
                    packet.frameTimeMilliseconds = 1000.0f / 60.0f;
                    const auto token = DspAaQueueFrame(&packet);
                    if (!token)
                        throw std::runtime_error("Bridge rejected frame.");
                    const D3D11_VIEWPORT viewport{2, 3, 101, 103, 0.2f, 0.9f};
                    context->RSSetViewports(1, &viewport);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                    std::thread renderer([token] { DspAaGetRenderEvent()(1, token); });
                    renderer.join();
                    DspAaStatus status{};
                    status.size = sizeof(status);
                    if (!DspAaGetStatus(1, &status) || status.result != 1 ||
                        status.requestedPreset != stage || status.observedPreset != 'A' + stage - 1 ||
                        status.verification != static_cast<unsigned>(dspaa::PresetVerdict::Matched))
                        throw std::runtime_error(std::string("Bridge frame failed: ") + status.message);
                    D3D11_VIEWPORT observedViewport{};
                    UINT viewportCount = 1;
                    context->RSGetViewports(&viewportCount, &observedViewport);
                    D3D11_PRIMITIVE_TOPOLOGY topology{};
                    context->IAGetPrimitiveTopology(&topology);
                    ComPtr<ID3D11ComputeShader> shader;
                    context->CSGetShader(&shader, nullptr, nullptr);
                    if (viewportCount != 1 || observedViewport.Width != viewport.Width ||
                        observedViewport.TopLeftX != viewport.TopLeftX ||
                        topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST || shader)
                        throw std::runtime_error("NGX did not restore sampled D3D11 context state.");
                }
            }
            if (bridge) {
                saveOutput(device.Get(), context.Get(), output.Get(),
                           outputDirectory /
                               (std::string("bridge-") + static_cast<char>('A' + stage - 1) + ".ppm"));
                std::cout << "bridge_stage=" << static_cast<char>('A' + stage - 1) << " frames=" << frameCount
                          << " verification=matched-runtime-log state_samples=preserved\n";
            }
        }
        if (bridge) {
            DspAaGetRenderEvent()(1, DspAaQueueRelease(1));
            DspAaStatus retired{};
            retired.size = sizeof(retired);
            if (!DspAaGetStatus(1, &retired) || retired.result != 2)
                throw std::runtime_error("Bridge retirement failed.");
            DspAaGetRenderEvent()(1, DspAaQueueShutdown());
            std::cout << "bridge_switch_stages=" << stages.size() << " shutdown=complete\n";
            return 0;
        }
        saveOutput(device.Get(), context.Get(), output.Get(),
                   outputDirectory / (L"preset-" + presetName + L".ppm"));
        dspaa::PresetEvidence evidence;
        {
            std::lock_guard lock(logMutex);
            evidence = presetEvidence;
        }
        const auto verdict = evidence.verify(static_cast<char>(presetName.front()));
        std::cout << "evaluated_frames=" << frameCount << " request_preset_value=" << preset
                  << " observed_preset=" << (evidence.selected ? evidence.selected : '?')
                  << " verification=" << dspaa::verdictName(verdict) << '\n';
        return verdict == dspaa::PresetVerdict::Matched ? 0 : 3;
    } catch (const std::exception& error) {
        if (bridge)
            DspAaGetRenderEvent()(1, DspAaQueueShutdown());
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
