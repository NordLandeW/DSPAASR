#include "graphics/dx11-dx12.h"
#include "ui-proof/shadow.h"
#include <MinHook.h>
#include <array>
#include <chrono>
#include <cstring>
#include <d3d11sdklayers.h>
#include <iostream>
#include <limits>
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
ComPtr<ID3D11Buffer> byteBuffer(ID3D11Device* device, unsigned bytes, unsigned bindings, D3D11_USAGE usage,
                                const void* initial = nullptr) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.BindFlags = bindings;
    desc.Usage = usage;
    if (usage == D3D11_USAGE_DYNAMIC)
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initial;
    ComPtr<ID3D11Buffer> result;
    check(device->CreateBuffer(&desc, initial ? &data : nullptr, &result), "Create probe byte buffer");
    return result;
}
std::vector<unsigned char> byteValues(unsigned count, unsigned char salt) {
    std::vector<unsigned char> result(count);
    for (unsigned i = 0; i < count; ++i)
        result[i] = static_cast<unsigned char>(i ^ (i >> 8) ^ (i >> 16) ^ salt);
    return result;
}
void writeBytes(Gpu& gpu, ID3D11Buffer* target, const std::vector<unsigned char>& data) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(gpu.context->Map(target, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map probe IA bytes for writing");
    std::memcpy(mapped.pData, data.data(), data.size());
    gpu.context->Unmap(target, 0);
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
    require(stats.bytes == 0 && stats.liveBuffers == 0 && stats.iaResidentPages == 0,
            "Resource retirement leaked a shadow, IA page or COM cycle");
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
        // A real six-index ushort buffer has only 12 bytes: cold readback and
        // its final cell must not assume a physically present fourth uint32.
        {
            const std::array<uint16_t, 6> initial{0, 1, 2, 0x7FC1, 0xFFFF, 0x8000};
            auto target = byteBuffer(gpu.device.Get(), sizeof(initial), D3D11_BIND_INDEX_BUFFER,
                                     D3D11_USAGE_IMMUTABLE, initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<uint16_t, 6> output;
            output.fill(9);
            require(!s.shadow->readBytes(target.Get(), 0, sizeof(output), output.data(), info) &&
                        info.owned && output == std::array<uint16_t, 6>{},
                    "Short IB was trusted before its real cold copy completed");
            require(s.shadow->stats().queued == 1 && s.shadow->stats().pending == 1,
                    "Short IB did not reuse the common readback ticket owner");
            s.collect();
            require(s.shadow->readBytes(target.Get(), 0, sizeof(output), output.data(), info) &&
                        output == initial,
                    "12-byte IB tail lost exact ushort index bits");
            uint16_t last = 0;
            require(s.shadow->readBytes(target.Get(), sizeof(initial) - sizeof(last), sizeof(last), &last,
                                        info) &&
                        last == initial.back(),
                    "Short IB's last index was inaccessible");
            std::array<unsigned char, 4> invalid{9, 9, 9, 9};
            require(!s.shadow->readBytes(target.Get(), sizeof(initial) - 1, sizeof(invalid), invalid.data(),
                                         info) &&
                        invalid == std::array<unsigned char, 4>{},
                    "IA physical overrun exposed partial bytes");
            invalid.fill(9);
            require(!s.shadow->readBytes(target.Get(), std::numeric_limits<unsigned>::max() - 1,
                                         sizeof(invalid), invalid.data(), info) &&
                        invalid == std::array<unsigned char, 4>{},
                    "IA byte range wrapped at UINT_MAX");
            require(s.shadow->stats().queued == 1, "Invalid IA range scheduled another GPU copy");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Integer payloads cross cells and end at an odd physical tail. Some
        // words encode NaNs/infinities as floats; no float arithmetic is valid.
        {
            std::array<unsigned char, 31> initial{};
            const std::array<uint32_t, 5> bits{0x7FC00001u, 0x7FA12345u, 0xFF800000u, 0xFFFFFFFFu,
                                               0x80000000u};
            std::memcpy(initial.data() + 11, bits.data(), sizeof(bits));
            Session s(gpu);
            auto target = byteBuffer(gpu.device.Get(), sizeof(initial),
                                     D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
                                     D3D11_USAGE_IMMUTABLE, initial.data());
            require(s.shadow->created(target.Get(), initial.data()), "IA initial data was not adopted");
            ShadowReadInfo info;
            std::array<uint32_t, 5> output{};
            require(s.shadow->readBytes(target.Get(), 11, sizeof(output), output.data(), info) &&
                        output == bits,
                    "Cross-cell byte read changed uint32 index/NaN payload bits");
            unsigned char tail = 0;
            require(s.shadow->readBytes(target.Get(), sizeof(initial) - 1, 1, &tail, info) &&
                        tail == initial.back(),
                    "Non-16-byte vertex tail was rounded out of the allocation");
            std::array<float, 4> constants{9, 9, 9, 9};
            require(!s.shadow->read(target.Get(), 0, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, 0, 4, constants,
                                    info) &&
                        constants == std::array<float, 4>{},
                    "Adopted IA facts bypassed constant-buffer legality");
            require(s.shadow->stats().queued == 0, "Observed IA bytes unnecessarily queued readback");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // An in-flight byte ticket cannot overwrite a newer UpdateSubresource.
        // A later real GPU copy invalidates those CPU facts and must recover cold.
        {
            auto initial = byteValues(27, 0x21), replacement = byteValues(27, 0xA5),
                 copied = byteValues(27, 0x5A);
            auto target = byteBuffer(gpu.device.Get(), static_cast<unsigned>(initial.size()),
                                     D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 14> output{};
            require(!s.shadow->readBytes(target.Get(), 13, sizeof(output), output.data(), info),
                    "Revision fixture IA data was not genuinely cold");
            gpu.context->UpdateSubresource(target.Get(), 0, nullptr, replacement.data(), 0, 0);
            require(s.shadow->readBytes(target.Get(), 13, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), replacement.data() + 13, sizeof(output)) == 0,
                    "IA update failed to replace cached bytes before old ticket retirement");
            s.collect();
            require(s.shadow->stats().stale == 1 && s.shadow->stats().published == 0,
                    "Old IA ticket overwrote the new resource revision");
            const std::array<unsigned char, 3> patch{0xFF, 0x7F, 0x80};
            const D3D11_BOX tail{24, 0, 0, 27, 1, 1};
            gpu.context->UpdateSubresource(target.Get(), 0, &tail, patch.data(), 0, 0);
            std::array<unsigned char, 4> patched{};
            require(s.shadow->readBytes(target.Get(), 23, sizeof(patched), patched.data(), info) &&
                        patched ==
                            std::array<unsigned char, 4>{replacement[23], patch[0], patch[1], patch[2]},
                    "Partial IA tail update lost the unchanged bytes or its real byte range");
            auto source = byteBuffer(gpu.device.Get(), static_cast<unsigned>(copied.size()),
                                     D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, copied.data());
            gpu.context->CopyResource(target.Get(), source.Get());
            output.fill(9);
            require(!s.shadow->readBytes(target.Get(), 13, sizeof(output), output.data(), info) &&
                        output == std::array<unsigned char, 14>{},
                    "GPU-written IA bytes reused the old CPU snapshot");
            s.collect();
            require(s.shadow->readBytes(target.Get(), 13, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), copied.data() + 13, sizeof(output)) == 0,
                    "IA cold recovery did not observe the actual GPU copy");
            source.Reset();
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Far beyond the CB window, copy just three demanded cells (16+16+7
        // bytes), not a full 256-KiB geometry pool or a rounded-up tail.
        {
            constexpr unsigned bytes = 256 * 1024 + 7, offset = bytes - 25;
            const auto initial = byteValues(bytes, 0x81);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT,
                                     initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 25> output{};
            require(!s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info),
                    "Large cold IA pool was trusted without a byte-range ticket");
            require(!s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info) &&
                        s.shadow->stats().queued == 1,
                    "Large IA reads duplicated the pending resource ticket");
            s.collect();
            require(s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), initial.data() + offset, sizeof(output)) == 0,
                    "Large IA range inherited a CB window or wrong staging origin");
            const auto stats = s.shadow->stats();
            require(stats.copiedBytes == 39 && stats.peakBytes < bytes && stats.published == 1 &&
                        stats.wcBytes == 0,
                    "Large IA cold recovery copied or shadowed the whole allocation");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // A real dynamic WC upload refreshes the two demanded physical pages,
        // including the short tail. Cold GPU copies still cover only 39 bytes.
        // The obsolete partial staging lease remains owned until its fence retires.
        {
            constexpr unsigned bytes = 256 * 1024 + 7, offset = bytes - 25;
            const auto initial = byteValues(bytes, 0x13), replacement = byteValues(bytes, 0xB7);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC,
                                     initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 25> output{};
            require(!s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info),
                    "WC IA fixture did not queue its initial sparse cold read");
            writeBytes(gpu, target.Get(), replacement);
            require(s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), replacement.data() + offset, sizeof(output)) == 0,
                    "Sparse IA WC update failed to refresh its nonaligned tail");
            require(s.shadow->stats().wcBytes == ConstantShadow::iaPageBytes + 7 &&
                        s.shadow->stats().wcCopies == 1 &&
                        s.shadow->stats().iaWcBytes == ConstantShadow::iaPageBytes + 7 &&
                        s.shadow->stats().iaWcCopies == 1 && s.shadow->stats().iaResidentPages == 2 &&
                        s.shadow->stats().pending == 1 && s.shadow->stats().peakBytes < bytes,
                    "IA WC observation scanned the whole pool or freed a pending range");
            s.collect();
            require(s.shadow->stats().stale == 1 && s.shadow->stats().published == 0 &&
                        s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), replacement.data() + offset, sizeof(output)) == 0,
                    "Sparse cold data overwrote the newer IA WC revision");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Reject actual GPU-write-capable buffers before adoption. CB bytes may
        // not bypass their own binding-window contract through the IA entry point.
        {
            Session s(gpu);
            ShadowReadInfo info;
            const auto initial = byteValues(64, 0x11);
            const unsigned bindings[]{D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS,
                                      D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT,
                                      D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_CONSTANT_BUFFER};
            for (const auto bind : bindings) {
                auto target = byteBuffer(gpu.device.Get(), static_cast<unsigned>(initial.size()), bind,
                                         D3D11_USAGE_DEFAULT, initial.data());
                std::array<unsigned char, 4> output{9, 9, 9, 9};
                require(!s.shadow->readBytes(target.Get(), 0, sizeof(output), output.data(), info) &&
                            !info.owned && output == std::array<unsigned char, 4>{},
                        "IA read accepted a non-IA or GPU-write-capable buffer");
                if (bind != D3D11_BIND_CONSTANT_BUFFER)
                    require(!s.shadow->created(target.Get(), initial.data()),
                            "Unsafe IA flags were attached by creation");
            }
            require(s.shadow->stats().bytes == 0 && s.shadow->stats().queued == 0,
                    "Rejected IA buffer flags consumed the shared budget or queued a GPU copy");
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // A partial IA ticket has exactly the same no-early-release contract as
        // the original CB ticket, including stop without a CPU staging Map.
        {
            constexpr unsigned bytes = 256 * 1024 + 7, offset = bytes - 25;
            const auto initial = byteValues(bytes, 0x27);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT,
                                     initial.data());
            Session s(gpu);
            GpuGate gate(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 25> output{};
            require(!s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info),
                    "Gated IA range did not queue a cold ticket");
            const auto charged = s.shadow->stats().bytes;
            s.detach();
            require(!s.shadow->stop(), "Stop retired a GPU-blocked IA range");
            require(s.shadow->stats().pending == 1 && s.shadow->stats().bytes == charged,
                    "Stopped IA range lost its staging charge before GPU retirement");
            target.Reset();
            require(s.shadow->stats().liveBuffers == 1,
                    "Pending IA range lost its application-resource lease");
            gate.open();
            gpu.complete();
            require(s.shadow->stop(), "Completed IA range required CPU readback during stop");
            const auto stats = s.shadow->stats();
            require(stats.pending == 0 && stats.stale == 1 && stats.published == 0 &&
                        stats.copiedBytes == 0 && stats.failed == 0,
                    "Stopped IA retirement published discarded bytes or bypassed its ticket");
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // New offsets can be certified by bytes actually copied at Unmap, even
        // when that individual offset was never requested. A merely registered
        // lookahead page, however, must remain unknown until a real observation.
        {
            constexpr unsigned page = ConstantShadow::iaPageBytes, bytes = 3 * page + 7;
            const auto initial = byteValues(bytes, 0x18), replacement = byteValues(bytes, 0xB4);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DYNAMIC,
                                     initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 2> output{};
            require(!s.shadow->readBytes(target.Get(), 32, sizeof(output), output.data(), info),
                    "Forward IA fixture did not begin with genuinely cold bytes");
            writeBytes(gpu, target.Get(), replacement);
            require(s.shadow->readBytes(target.Get(), 96, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), replacement.data() + 96, sizeof(output)) == 0,
                    "New same-page IA offset was not learned from the actual WC mapping");
            require(s.shadow->readBytes(target.Get(), page + 16, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), replacement.data() + page + 16, sizeof(output)) == 0,
                    "Observed forward page was still forced through a stale cold ticket");
            output.fill(9);
            require(!s.shadow->readBytes(target.Get(), 2 * page + 16, sizeof(output), output.data(), info) &&
                        output == std::array<unsigned char, 2>{},
                    "Newly registered lookahead was trusted without observing its bytes");
            const auto stats = s.shadow->stats();
            require(stats.iaReadHits == 2 && stats.iaReadMisses == 2 && stats.iaWcBytes == 2 * page &&
                        stats.iaWcCopies == 1 && stats.queued == 1,
                    "IA accounting did not distinguish full reads from bounded WC page copies");
            s.collect();
            require(s.shadow->stats().stale == 1 && s.shadow->stats().published == 0,
                    "Old page ticket survived a later resource revision");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // A streaming cursor advances by a page every write. The next page is
        // captured before that new offset is read; demand older than the bounded
        // working set must be evicted rather than accumulating forever.
        {
            constexpr unsigned page = ConstantShadow::iaPageBytes;
            constexpr unsigned steps = 3 * ConstantShadow::maximumIaPages, bytes = (steps + 2) * page + 7;
            auto data = byteValues(bytes, 0x11);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC,
                                     data.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 2> output{};
            require(!s.shadow->readBytes(target.Get(), 16, sizeof(output), output.data(), info),
                    "Sliding IA fixture did not register its initial demand");
            for (unsigned i = 1; i <= steps; ++i) {
                data = byteValues(bytes, static_cast<unsigned char>(i));
                writeBytes(gpu, target.Get(), data);
                const auto offset = i * page + 16;
                require(s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info) &&
                            std::memcmp(output.data(), data.data() + offset, sizeof(output)) == 0,
                        "Sliding IA offset did not use this revision's already observed forward page");
                require(s.shadow->stats().iaResidentPages <= ConstantShadow::maximumIaPages,
                        "Sliding IA reads accumulated unbounded resident pages");
            }
            require(s.shadow->stats().iaReadHits == steps && s.shadow->stats().iaReadMisses == 1 &&
                        s.shadow->stats().iaEvictedPages > 0 && s.shadow->stats().peakBytes < bytes &&
                        s.shadow->stats().iaWcBytes <=
                            uint64_t(steps) * ConstantShadow::maximumIaPages * page,
                    "Streaming IA working set escaped its page or copied-byte policy");
            s.collect();
            output.fill(9);
            require(!s.shadow->readBytes(target.Get(), 16, sizeof(output), output.data(), info) &&
                        output == std::array<unsigned char, 2>{},
                    "Evicted IA data remained readable merely because its address was once seen");
            data = byteValues(bytes, 0xC9);
            writeBytes(gpu, target.Get(), data);
            require(s.shadow->readBytes(target.Get(), 16, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), data.data() + 16, sizeof(output)) == 0,
                    "A demanded evicted page could not recover from a later real write");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Partial UpdateSubresource establishes only its actual bytes, including
        // sub-cell, cross-cell and physical-tail updates. Known neighbors can be
        // merged, but a two-byte write must never certify a whole cell or page.
        {
            constexpr unsigned page = ConstantShadow::iaPageBytes, bytes = 3 * page + 7;
            const auto initial = byteValues(bytes, 0x31);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT,
                                     initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 4> wide{};
            require(!s.shadow->readBytes(target.Get(), page + 4, sizeof(wide), wide.data(), info),
                    "Partial IA fixture did not start unknown");
            const std::array<unsigned char, 2> patch{0xEF, 0x7F};
            const D3D11_BOX tiny{page + 5, 0, 0, page + 7, 1, 1};
            gpu.context->UpdateSubresource(target.Get(), 0, &tiny, patch.data(), 0, 0);
            std::array<unsigned char, 2> pair{};
            require(s.shadow->readBytes(target.Get(), page + 5, sizeof(pair), pair.data(), info) &&
                        pair == patch,
                    "A real two-byte IA update was lost because its cell was not fully observed");
            wide.fill(9);
            require(!s.shadow->readBytes(target.Get(), page + 4, sizeof(wide), wide.data(), info) &&
                        wide == std::array<unsigned char, 4>{},
                    "Two-byte IA update promoted unobserved bytes from the same cell");
            const std::array<unsigned char, 4> crossing{0x01, 0x7F, 0xFF, 0x80};
            const D3D11_BOX middle{page + 15, 0, 0, page + 19, 1, 1};
            gpu.context->UpdateSubresource(target.Get(), 0, &middle, crossing.data(), 0, 0);
            require(s.shadow->readBytes(target.Get(), page + 15, sizeof(wide), wide.data(), info) &&
                        wide == crossing,
                    "Cross-cell IA update changed exact raw bits");
            std::array<unsigned char, 8> unknown;
            unknown.fill(9);
            require(!s.shadow->readBytes(target.Get(), page + 13, sizeof(unknown), unknown.data(), info) &&
                        unknown == std::array<unsigned char, 8>{},
                    "Cross-cell update certified untouched neighboring bytes");
            require(!s.shadow->readBytes(target.Get(), bytes - 2, sizeof(pair), pair.data(), info),
                    "Unobserved physical IA tail was inferred from another page");
            const D3D11_BOX tail{bytes - 2, 0, 0, bytes, 1, 1};
            gpu.context->UpdateSubresource(target.Get(), 0, &tail, patch.data(), 0, 0);
            require(s.shadow->readBytes(target.Get(), bytes - 2, sizeof(pair), pair.data(), info) &&
                        pair == patch,
                    "Actual two-byte physical tail could not be read");
            std::array<unsigned char, 3> tailUnknown{9, 9, 9};
            require(!s.shadow->readBytes(target.Get(), bytes - 3, sizeof(tailUnknown), tailUnknown.data(),
                                         info) &&
                        tailUnknown == std::array<unsigned char, 3>{},
                    "Physical tail update promoted its untouched leading byte");
            const D3D11_BOX wholePage{page, 0, 0, 2 * page, 1, 1};
            gpu.context->UpdateSubresource(target.Get(), 0, &wholePage, initial.data() + page, 0, 0);
            gpu.context->UpdateSubresource(target.Get(), 0, &tiny, patch.data(), 0, 0);
            std::array<unsigned char, 8> expected{};
            std::memcpy(expected.data(), initial.data() + page + 3, expected.size());
            expected[2] = patch[0];
            expected[3] = patch[1];
            require(s.shadow->readBytes(target.Get(), page + 3, sizeof(unknown), unknown.data(), info) &&
                        unknown == expected,
                    "Partial IA update failed to preserve previously proved neighboring bytes");
            wide.fill(9);
            require(!s.shadow->readBytes(target.Get(), 2 * page - 2, sizeof(wide), wide.data(), info) &&
                        wide == std::array<unsigned char, 4>{},
                    "Cross-page miss exposed a partial prefix from the already proved page");
            const D3D11_BOX pageBoundary{2 * page - 2, 0, 0, 2 * page + 2, 1, 1};
            gpu.context->UpdateSubresource(target.Get(), 0, &pageBoundary, crossing.data(), 0, 0);
            require(s.shadow->readBytes(target.Get(), 2 * page - 2, sizeof(wide), wide.data(), info) &&
                        wide == crossing,
                    "Cross-page IA update used the wrong source offset or promoted the wrong bytes");
            s.collect();
            require(s.shadow->stats().queued == 1 && s.shadow->stats().stale == 1 &&
                        s.shadow->stats().published == 0 && s.shadow->stats().iaWcCopies == 0,
                    "Superseded cold data changed partial byte validity or WC accounting");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // A completed ticket cannot recreate a page evicted while its GPU lease
        // was pending. The following copy may publish only its own 16 real bytes,
        // not the rest of the allocated page or the registered lookahead page.
        {
            constexpr unsigned page = ConstantShadow::iaPageBytes;
            constexpr unsigned lastPage = ConstantShadow::maximumIaPages + 2,
                               bytes = (lastPage + 2) * page + 7;
            const auto initial = byteValues(bytes, 0x79);
            auto target = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT,
                                     initial.data());
            Session s(gpu);
            ShadowReadInfo info;
            std::array<unsigned char, 2> output{};
            require(!s.shadow->readBytes(target.Get(), 16, sizeof(output), output.data(), info),
                    "Eviction fixture did not queue its first actual GPU range");
            for (unsigned i = 2; i <= lastPage; ++i)
                require(
                    !s.shadow->readBytes(target.Get(), i * page + 16, sizeof(output), output.data(), info),
                    "Pending unrelated copy certified a new IA page");
            const auto before = s.shadow->stats();
            require(before.queued == 1 && before.pending == 1 && before.iaEvictedPages > 0 &&
                        before.iaResidentPages == ConstantShadow::maximumIaPages,
                    "IA eviction recycled a pending copy or escaped its resident limit");
            s.collect();
            require(s.shadow->stats().published == 0 && s.shadow->stats().stale == 1 &&
                        s.shadow->stats().iaResidentPages == before.iaResidentPages,
                    "Cold publication resurrected an already evicted IA demand page");
            const auto offset = lastPage * page + 16;
            require(!s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info),
                    "Uncopied resident page became valid after another page's retirement");
            s.collect();
            require(s.shadow->readBytes(target.Get(), offset, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), initial.data() + offset, sizeof(output)) == 0,
                    "Exact cold interval did not populate the resident IA page");
            output.fill(9);
            require(!s.shadow->readBytes(target.Get(), offset + 16, sizeof(output), output.data(), info) &&
                        output == std::array<unsigned char, 2>{},
                    "Cold ticket promoted uncopied bytes elsewhere in the same page");
            target.Reset();
            released(s);
            s.close();
            ++scenarios;
            std::cout << "scenario " << scenarios << " passed\n";
        }
        // Two data pages plus validity/node accounting fit this reduced shared
        // quota; another resource's demanded page does not. Denial clears output
        // and does not steal a live source/ticket or exceed the global budget.
        {
            constexpr unsigned page = ConstantShadow::iaPageBytes, bytes = 4 * page;
            constexpr uint64_t limit = 3 * page;
            const auto initial = byteValues(bytes, 0x41);
            auto first = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT,
                                    initial.data());
            auto second = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT,
                                     initial.data());
            Session s(gpu, limit);
            ShadowReadInfo info;
            std::array<unsigned char, 2> output{};
            require(!s.shadow->readBytes(first.Get(), 16, sizeof(output), output.data(), info),
                    "Budget fixture did not queue its bounded IA demand");
            output.fill(9);
            require(!s.shadow->readBytes(second.Get(), 16, sizeof(output), output.data(), info) &&
                        output == std::array<unsigned char, 2>{} && info.failure &&
                        std::strcmp(info.failure, "byte-budget") == 0,
                    "Exhausted IA page budget returned partial or uncharged bytes");
            const auto stats = s.shadow->stats();
            require(stats.bytes <= limit && stats.peakBytes <= limit && stats.iaResidentPages == 2 &&
                        stats.queued == 1 && stats.pending == 1,
                    "Page data/validity/node charge escaped the shared quota or recycled a ticket");
            s.collect();
            require(s.shadow->readBytes(first.Get(), 16, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), initial.data() + 16, sizeof(output)) == 0,
                    "Another resource's budget denial invalidated already owned IA data");
            first.Reset();
            second.Reset();
            released(s);
            s.close();
            // Optional forward selection is also charged, but inability to
            // allocate it must not turn a proved read into a cache miss.
            auto narrowTarget = byteBuffer(gpu.device.Get(), bytes, D3D11_BIND_VERTEX_BUFFER,
                                           D3D11_USAGE_DEFAULT, initial.data());
            Session narrow(gpu, 2 * page);
            require(!narrow.shadow->readBytes(narrowTarget.Get(), 16, sizeof(output), output.data(), info) &&
                        narrow.shadow->stats().iaResidentPages == 1 && narrow.shadow->stats().queued == 1,
                    "Optional lookahead consumed a charge that only the demanded IA page could afford");
            narrow.collect();
            require(narrow.shadow->readBytes(narrowTarget.Get(), 16, sizeof(output), output.data(), info) &&
                        std::memcmp(output.data(), initial.data() + 16, sizeof(output)) == 0 &&
                        narrow.shadow->stats().iaResidentPages == 1 &&
                        narrow.shadow->stats().peakBytes <= 2 * page,
                    "Optional lookahead budget denial rejected already proved IA bytes");
            narrowTarget.Reset();
            released(narrow);
            narrow.close();
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
