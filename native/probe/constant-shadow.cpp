#include "graphics/dx11-dx12.h"
#include "ui-proof/shadow.h"
#include <MinHook.h>
#include <array>
#include <chrono>
#include <cstring>
#include <d3d11sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>

namespace {
using dspaa::proof::ConstantShadow;
using dspaa::proof::ShadowReadInfo;
using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void check(HRESULT result, const char* message) {
    if (FAILED(result))
        throw std::runtime_error(message);
}
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> extended;
    ComPtr<ID3D11Fence> fence;
    ComPtr<ID3D11InfoQueue> debug;
    HANDLE event = nullptr;
    uint64_t sequence = 0;
    Gpu() {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level{};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_DEBUG, levels,
                                2, D3D11_SDK_VERSION, &device, &level, &context),
              "Create headless shadow-probe device");
        check(context.As(&extended), "Query shadow-probe context");
        check(device.As(&debug), "Query shadow-probe debug queue");
        ComPtr<ID3D11Device5> newer;
        check(device.As(&newer), "Query shadow-probe fence device");
        check(newer->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "Create probe completion fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "Create probe completion event");
        const auto initialized = MH_Initialize();
        require(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED,
                "Initialize probe write hooks");
        dspaa::capture::install(extended.Get());
    }
    ~Gpu() {
        if (event)
            CloseHandle(event);
    }
    // Only the probe waits here. Production shadow polls its own GPU fence and
    // uses DO_NOT_WAIT for active publication, with no flush/wait path.
    void complete() {
        const auto value = ++sequence;
        check(extended->Signal(fence.Get(), value), "Signal probe completion fence");
        check(fence->SetEventOnCompletion(value, event), "Arm probe completion event");
        context->Flush();
        require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "Shadow probe GPU did not retire");
        const auto completed = fence->GetCompletedValue();
        require(completed != UINT64_MAX && completed >= value, "Probe event did not prove GPU completion");
    }
    void unbind() {
        ID3D11Buffer* empty = nullptr;
        context->PSSetConstantBuffers(0, 1, &empty);
    }
    unsigned errors() {
        unsigned result = 0;
        for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(debug->GetMessage(i, nullptr, &size), "Size probe debug message");
            std::vector<unsigned char> bytes(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            check(debug->GetMessage(i, message, &size), "Read probe debug message");
            if (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
                ++result;
                std::cerr << message->pDescription << '\n';
            }
        }
        return result;
    }
};
// A CPU-signallable private fence makes the unretired branch deterministic.
// No timing assumption or application/desktop resource is involved. Every exit
// releases the gate, including an assertion exception before the explicit open.
struct GpuGate {
    std::shared_ptr<dspaa::Dx11Dx12> graphics;
    ComPtr<ID3D12Fence> release;
    ComPtr<ID3D11Fence> wait;
    explicit GpuGate(Gpu& gpu) : graphics(dspaa::acquireDx11Dx12(gpu.device.Get())) {
        check(graphics->device12()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&release)),
              "Create private probe gate");
        ComPtr<ID3D11Device5> device;
        check(gpu.device.As(&device), "Query private probe gate device");
        HANDLE shared = nullptr;
        check(graphics->device12()->CreateSharedHandle(release.Get(), nullptr, GENERIC_ALL, nullptr, &shared),
              "Share private probe gate");
        const auto opened = device->OpenSharedFence(shared, IID_PPV_ARGS(&wait));
        CloseHandle(shared);
        check(opened, "Open private probe gate in D3D11");
        check(gpu.extended->Wait(wait.Get(), 1), "Queue private probe GPU gate");
    }
    ~GpuGate() {
        release->Signal(1);
    }
    void open() {
        check(release->Signal(1), "Open private probe GPU gate");
    }
};
std::vector<float> values(unsigned bytes, float first) {
    std::vector<float> result(bytes / 4);
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = first + static_cast<float>(i);
    return result;
}
ComPtr<ID3D11Buffer> buffer(ID3D11Device* device, unsigned bytes, D3D11_USAGE usage,
                            const std::vector<float>* initial = nullptr) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = usage;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (usage == D3D11_USAGE_DYNAMIC)
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA data{};
    if (initial)
        data.pSysMem = initial->data();
    ComPtr<ID3D11Buffer> result;
    check(device->CreateBuffer(&desc, initial ? &data : nullptr, &result), "Create probe constant buffer");
    return result;
}
void write(Gpu& gpu, ID3D11Buffer* target, const std::vector<float>& data) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(gpu.context->Map(target, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
          "Map probe constants for writing");
    std::memcpy(mapped.pData, data.data(), data.size() * sizeof(float));
    gpu.context->Unmap(target, 0);
}
struct Session {
    Gpu& gpu;
    std::shared_ptr<ConstantShadow> shadow;
    bool attached = true;
    explicit Session(Gpu& g, uint64_t limit = ConstantShadow::maximumBytes)
        : gpu(g), shadow(std::make_shared<ConstantShadow>(g.device.Get(), g.context.Get(), limit)) {
        dspaa::capture::attachWrites(shadow);
    }
    void detach() {
        if (attached) {
            dspaa::capture::detachWrites(shadow.get());
            attached = false;
        }
    }
    void collect() {
        gpu.complete();
        // GPU retirement does not guarantee that the driver's first DO_NOT_WAIT
        // Map is CPU-ready. Active publication must also await a successful Map;
        // stopped owners instead discard already-retired tickets without it.
        // This opt-in probe may wait; the deadline only isolates a driver hang.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            shadow->collect();
            const auto stats = shadow->stats();
            require(stats.failed == 0, "Shadow readback reported a permanent failure");
            if (!stats.pending)
                return;
            require(std::chrono::steady_clock::now() < deadline, "Shadow probe CPU readback did not retire");
            Sleep(1);
        } while (true);
    }
    void close() {
        detach();
        gpu.unbind();
        collect();
        require(shadow->stop(), "Completed shadow tickets failed to retire");
    }
    ~Session() {
        detach();
    }
    bool read(ID3D11Buffer* target, unsigned offset, unsigned components, std::array<float, 4>& output,
              ShadowReadInfo& info) {
        gpu.context->PSSetConstantBuffers(0, 1, &target);
        ComPtr<ID3D11Buffer> actual;
        UINT first = 0, count = 0;
        gpu.extended->PSGetConstantBuffers1(0, 1, &actual, &first, &count);
        return shadow->read(actual.Get(), first, count, offset, components, output, info);
    }
};
void released(Session& session) {
    session.gpu.unbind();
    session.collect();
    // collect() releases the ticket's final resource reference. D3D11 defers
    // object destruction until a later Flush; the preceding fence is too early.
    session.gpu.complete();
    const auto stats = session.shadow->stats();
    if (stats.bytes || stats.liveBuffers)
        std::cerr << "release: bytes=" << stats.bytes << " live=" << stats.liveBuffers
                  << " pending=" << stats.pending << " queued=" << stats.queued
                  << " published=" << stats.published << " stale=" << stats.stale << '\n';
    require(stats.bytes == 0 && stats.liveBuffers == 0, "Resource retirement leaked a shadow or COM cycle");
}
} // namespace
int main() {
    try {
        Gpu gpu;
        unsigned scenarios = 0;
        // Map/Unmap happens before any bind or pin registration. The tiny WC
        // snapshot must make this first read work without a second application write.
        {
            Session s(gpu);
            auto target = buffer(gpu.device.Get(), 96, D3D11_USAGE_DYNAMIC);
            write(gpu, target.Get(), values(96, 1));
            std::array<float, 4> output{9, 9, 9, 9};
            ShadowReadInfo info;
            require(s.read(target.Get(), 92, 1, output, info) && output == std::array<float, 4>{24, 0, 0, 0},
                    "First binding lost a one-shot small dynamic write");
            require(s.shadow->stats().queued == 0 && s.shadow->stats().wcBytes == 96 &&
                        s.shadow->stats().wcCopies == 1,
                    "Small WC accounting lost the actual copied bytes");
            write(gpu, target.Get(), values(96, 101));
            require(s.read(target.Get(), 92, 1, output, info) && output[0] == 124,
                    "Small snapshot froze the object's first write forever");
            require(s.shadow->stats().wcBytes == 192 && s.shadow->stats().wcCopies == 2,
                    "Small WC refresh copied the pool or double-counted cached cell copies");
            output.fill(9);
            require(!s.read(target.Get(), 92, 2, output, info) && output == std::array<float, 4>{},
                    "Physical-tail failure exposed a partial shadow vector");
            require(s.shadow->stats().queued == 0, "Invalid byte range scheduled a cold GPU copy");
            gpu.unbind();
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // This buffer AND its only write predate the owner. No write callback
        // or future application upload can rescue it: the real cold copy must.
        {
            auto target = buffer(gpu.device.Get(), 288, D3D11_USAGE_DYNAMIC);
            write(gpu, target.Get(), values(288, 10));
            Session s(gpu);
            std::array<float, 4> output{9, 9, 9, 9};
            ShadowReadInfo info;
            require(!s.read(target.Get(), 48, 4, output, info) && info.owned &&
                        output == std::array<float, 4>{},
                    "Cold data was trusted before asynchronous completion");
            require(s.shadow->stats().pending == 1 && s.shadow->stats().queued == 1 &&
                        s.shadow->stats().wcBytes == 0,
                    "Late adoption failed to request one bounded copy");
            // Repeated demand before collection must not create another ticket,
            // regardless of whether the GPU happened to finish in the meantime.
            require(!s.read(target.Get(), 48, 4, output, info) && s.shadow->stats().queued == 1,
                    "A cold resource acquired duplicate pending tickets");
            s.collect();
            const auto recovered = s.read(target.Get(), 48, 4, output, info);
            if (!recovered || output != std::array<float, 4>{22, 23, 24, 25}) {
                const auto stats = s.shadow->stats();
                std::cerr << "cold recovery: success=" << recovered
                          << " failure=" << (info.failure ? info.failure : "none") << " output=" << output[0]
                          << ',' << output[1] << ',' << output[2] << ',' << output[3]
                          << " queued=" << stats.queued << " pending=" << stats.pending
                          << " published=" << stats.published << " stale=" << stats.stale
                          << " failed=" << stats.failed << " bytes=" << stats.bytes << '\n';
                gpu.errors();
            }
            require(recovered && output == std::array<float, 4>{22, 23, 24, 25},
                    "Stable one-shot constants never recovered from cold readback");
            require(s.read(target.Get(), 92, 1, output, info) && output == std::array<float, 4>{33, 0, 0, 0},
                    "Readback only healed the originally registered cell");
            require(s.shadow->stats().published == 1 && s.shadow->stats().pending == 0 &&
                        s.shadow->stats().queued == 1,
                    "Warm reads kept staging new copies");
            gpu.unbind();
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Both normal UpdateSubresource and Map change the resource revision.
        // A completed older ticket must never overwrite a newer CPU snapshot.
        for (unsigned dynamic = 0; dynamic < 2; ++dynamic) {
            auto initial = values(96, 1);
            auto target =
                buffer(gpu.device.Get(), 96, dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, &initial);
            Session s(gpu);
            std::array<float, 4> output{};
            ShadowReadInfo info;
            require(!s.read(target.Get(), 48, 4, output, info), "Expected a genuinely cold initial buffer");
            const auto replacement = values(96, 201);
            if (dynamic)
                write(gpu, target.Get(), replacement);
            else
                gpu.context->UpdateSubresource(target.Get(), 0, nullptr, replacement.data(), 0, 0);
            require(s.shadow->stats().pending == 1 && s.shadow->stats().queued == 1,
                    "A later write recycled an unretired ticket");
            s.collect();
            require(s.shadow->stats().stale == 1 && s.shadow->stats().published == 0,
                    "An earlier revision was published over a later write");
            require(s.read(target.Get(), 48, 4, output, info) &&
                        output == std::array<float, 4>{213, 214, 215, 216},
                    "Newly written constants were replaced by a stale GPU snapshot");
            gpu.unbind();
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // A valid cached snapshot and an in-flight snapshot each have an epoch.
        // Invalidating one must not accidentally validate the other on completion.
        {
            Session s(gpu);
            auto target = buffer(gpu.device.Get(), 96, D3D11_USAGE_DYNAMIC);
            write(gpu, target.Get(), values(96, 31));
            std::array<float, 4> output{};
            ShadowReadInfo info;
            require(s.read(target.Get(), 48, 4, output, info),
                    "Epoch fixture did not learn its initial data");
            s.shadow->invalidated(nullptr);
            output.fill(9);
            require(!s.read(target.Get(), 48, 4, output, info) && output == std::array<float, 4>{},
                    "Opaque epoch reused an older cached shadow");
            s.shadow->invalidated(nullptr);
            require(s.shadow->stats().pending == 1,
                    "Epoch invalidation discarded an unretired staging lease");
            s.collect();
            require(s.shadow->stats().stale == 1 && s.shadow->stats().published == 0,
                    "A pre-epoch ticket was published");
            require(!s.read(target.Get(), 48, 4, output, info), "A stale ticket fabricated a current epoch");
            s.collect();
            require(s.read(target.Get(), 48, 4, output, info) &&
                        output == std::array<float, 4>{43, 44, 45, 46},
                    "A fresh epoch could not recover stable cold data");
            gpu.unbind();
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Stop an owner with a ticket still logically pending, then adopt the
        // same actual resource. No previous-owner cache/ticket may authorize it.
        {
            auto initial = values(96, 51);
            auto target = buffer(gpu.device.Get(), 96, D3D11_USAGE_DEFAULT, &initial);
            Session first(gpu);
            std::array<float, 4> output{};
            ShadowReadInfo info;
            require(!first.read(target.Get(), 48, 4, output, info), "Owner fixture was not cold");
            {
                ConstantShadow competing(gpu.device.Get(), gpu.context.Get());
                require(!competing.created(target.Get(), initial.data()) && competing.stats().bytes == 0,
                        "An active owner's resource-private facts were replaced");
                require(competing.stop(), "Unused competing owner retained a ticket");
            }
            first.detach();
            first.shadow->stop();
            Session second(gpu);
            require(!second.read(target.Get(), 48, 4, output, info) && info.owned,
                    "New owner inherited old-owner data instead of adopting cold");
            const auto late = values(96, 901);
            require(!first.shadow->created(target.Get(), late.data()),
                    "Stopped adoption gate replaced the new owner's facts");
            first.shadow->beforeUpdate(gpu.context.Get(), target.Get(), 0, nullptr, late.data(), 0, 0, 0);
            output.fill(9);
            require(!first.read(target.Get(), 48, 4, output, info) && !info.owned &&
                        output == std::array<float, 4>{},
                    "Stopped owner still certified constants after handoff");
            first.collect();
            second.collect();
            require(first.shadow->stats().published == 0 && first.shadow->stats().bytes == 0,
                    "Retired owner published or retained its replaced facts");
            require(second.read(target.Get(), 48, 4, output, info) &&
                        output == std::array<float, 4>{63, 64, 65, 66},
                    "New owner failed to acquire its own current snapshot");
            gpu.unbind();
            target.Reset();
            released(second);
            first.close();
            second.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Unlike CPU readback, stop needs only the copy's GPU retirement. Hold
        // that copy behind an explicit gate, then complete it without ever
        // allowing active publication or requiring a CPU staging Map.
        {
            auto initial = values(96, 61);
            auto target = buffer(gpu.device.Get(), 96, D3D11_USAGE_DEFAULT, &initial);
            Session s(gpu);
            GpuGate gate(gpu);
            std::array<float, 4> output{};
            ShadowReadInfo info;
            require(!s.read(target.Get(), 48, 4, output, info), "Gated stop fixture was not cold");
            const auto charged = s.shadow->stats().bytes;
            s.detach();
            require(!s.shadow->stop(), "Stop claimed an explicitly blocked GPU copy was retired");
            require(s.shadow->stats().pending == 1 && s.shadow->stats().bytes == charged,
                    "Stop recycled or refunded an unretired copy");
            gpu.unbind();
            target.Reset();
            require(s.shadow->stats().liveBuffers == 1,
                    "Stopped copy lost its source lease before retirement");
            gate.open();
            gpu.complete();
            require(s.shadow->stop(), "GPU-retired stopped copy unnecessarily awaited CPU Map readiness");
            const auto stopped = s.shadow->stats();
            require(stopped.pending == 0 && stopped.published == 0 && stopped.copiedBytes == 0 &&
                        stopped.stale == 1 && stopped.failed == 0,
                    "Stopped retirement published/copied discarded constants or lost the ticket");
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Releasing the application's final resource reference cannot orphan
        // a ticket, free its staging allocation early, or form a permanent cycle.
        {
            auto initial = values(96, 71);
            auto target = buffer(gpu.device.Get(), 96, D3D11_USAGE_DEFAULT, &initial);
            Session s(gpu);
            std::array<float, 4> output{};
            ShadowReadInfo info;
            require(!s.read(target.Get(), 48, 4, output, info), "Release fixture was not cold");
            gpu.unbind();
            target.Reset();
            require(s.shadow->stats().pending == 1 && s.shadow->stats().liveBuffers == 1,
                    "Pending copy lost its resource lease");
            released(s);
            require(s.shadow->stats().pending == 0, "Released resource left a pending ticket");
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Fixed slot exhaustion is deterministic: completion is acknowledged
        // ONLY by collect(), never by an age or an unrelated write callback.
        {
            Session s(gpu);
            std::vector<ComPtr<ID3D11Buffer>> buffers;
            std::array<float, 4> output{};
            ShadowReadInfo info;
            const auto initial = values(96, 91);
            for (unsigned i = 0; i < ConstantShadow::maximumPending + 1; ++i) {
                buffers.push_back(buffer(gpu.device.Get(), 96, D3D11_USAGE_DEFAULT, &initial));
                require(!s.read(buffers.back().Get(), 48, 4, output, info),
                        "Uncollected cold read was reported warm");
            }
            require(s.shadow->stats().queued == ConstantShadow::maximumPending &&
                        s.shadow->stats().pending == ConstantShadow::maximumPending,
                    "Cold staging exceeded its fixed live-ticket limit");
            const auto replacement = values(96, 301);
            gpu.context->UpdateSubresource(buffers.front().Get(), 0, nullptr, replacement.data(), 0, 0);
            require(!s.read(buffers.back().Get(), 48, 4, output, info) &&
                        s.shadow->stats().queued == ConstantShadow::maximumPending,
                    "A stale but unretired ticket was reused as a free slot");
            gpu.unbind();
            buffers.clear();
            released(s);
            require(s.shadow->stats().stale == 1, "Slot fixture failed to discard the superseded snapshot");
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // A large pool keeps the old sparse route. Only the requested cell is
        // read from WC; no cold full-pool readback or full CPU scan is permitted.
        {
            Session s(gpu);
            auto target = buffer(gpu.device.Get(), 256 * 1024, D3D11_USAGE_DYNAMIC);
            std::array<float, 4> output{};
            ShadowReadInfo info;
            require(!s.shadow->read(target.Get(), 8192, 4096, 16, 4, output, info),
                    "Unobserved large-pool cell was trusted");
            require(s.shadow->stats().queued == 0, "Large cold allocation was copied wholesale");
            write(gpu, target.Get(), values(256 * 1024, 1));
            require(s.shadow->read(target.Get(), 8192, 4096, 16, 4, output, info) &&
                        output == std::array<float, 4>{32773, 32774, 32775, 32776},
                    "Sparse large-window cells lost their absolute allocation address");
            require(s.shadow->stats().wcBytes == 16 && s.shadow->stats().queued == 0,
                    "Large WC pool escaped the sparse-cell limit");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Use the same production accounting with a SMALLER isolated quota, not
        // thousands of allocations or a duplicated/mock budget implementation.
        {
            constexpr uint64_t limit = 32 * 1024;
            Session s(gpu, limit);
            std::vector<ComPtr<ID3D11Buffer>> buffers;
            const auto initial = values(4096, 1);
            bool denied = false;
            for (unsigned i = 0; i < 64; ++i) {
                buffers.push_back(buffer(gpu.device.Get(), 4096, D3D11_USAGE_DEFAULT, &initial));
                if (!s.shadow->created(buffers.back().Get(), initial.data())) {
                    denied = true;
                    break;
                }
            }
            require(denied && s.shadow->stats().bytes <= limit && s.shadow->stats().peakBytes <= limit,
                    "Shadow memory escaped its reserved live-byte quota");
            buffers.clear();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        {
            ComPtr<ID3D11Device> foreign;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL level{};
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &foreign, &level, &context),
                  "Create foreign device fixture");
            auto target = buffer(foreign.Get(), 96, D3D11_USAGE_DEFAULT);
            Session s(gpu);
            std::array<float, 4> output{9, 9, 9, 9};
            ShadowReadInfo info;
            // Never bind a foreign resource to the test context: adoption itself
            // must reject it before any GPU command can target the wrong device.
            require(!s.shadow->read(target.Get(), 0, 4096, 0, 4, output, info) && !info.owned &&
                        output == std::array<float, 4>{},
                    "Foreign-device constants were adopted");
            require(s.shadow->stats().bytes == 0 && s.shadow->stats().queued == 0,
                    "Foreign resource consumed this owner's shadow budget");
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        gpu.complete();
        require(gpu.errors() == 0, "Constant shadow caused D3D11 debug-layer errors");
        std::cout << "{\"constant_shadow_probe\":\"passed\",\"scenarios\":" << scenarios
                  << ",\"d3d11_errors\":0,\"window_created\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
