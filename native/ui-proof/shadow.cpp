#include "shadow.h"
#include "constants.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <wrl/client.h>

namespace dspaa::proof {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID bufferKey{0xa41ac69b, 0xc695, 0x498b, {0x92, 0x4f, 0x39, 0x04, 0x8c, 0x0c, 0x11, 0x23}};
constexpr unsigned cellBytes = D3D11_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS * sizeof(float);
using CellBytes = std::array<unsigned char, cellBytes>;
bool constantBuffer(const D3D11_BUFFER_DESC& desc) {
    return desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER && desc.ByteWidth && desc.ByteWidth % cellBytes == 0;
}
bool inputBuffer(const D3D11_BUFFER_DESC& desc) {
    constexpr unsigned input = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    constexpr unsigned allowed = input | D3D11_BIND_SHADER_RESOURCE;
    // Shared/tiled aliases can mutate without this owner's write observation.
    return desc.ByteWidth && (desc.BindFlags & input) && !(desc.BindFlags & ~allowed) && !desc.MiscFlags;
}
struct Budget {
    const uint64_t limit;
    std::atomic<uint64_t> bytes{0}, peak{0}, epoch{1}, copied{0}, invalidations{0};
    std::atomic<uint64_t> wcBytes{0}, wcCopies{0}, wcNanoseconds{0};
    std::atomic<uint64_t> queued{0}, published{0}, stale{0}, failed{0}, pending{0};
    std::atomic<uint64_t> adopted{0}, attachmentFailures{0}, liveBuffers{0};
    std::atomic<bool> active{true}, quarantined{false};
    explicit Budget(uint64_t maximum) : limit(maximum) {}
    bool reserve(uint64_t count) {
        auto used = bytes.load();
        do {
            if (used > limit || count > limit - used)
                return false;
        } while (!bytes.compare_exchange_weak(used, used + count));
        const auto next = used + count;
        auto previous = peak.load();
        while (previous < next && !peak.compare_exchange_weak(previous, next)) {
        }
        return true;
    }
};
struct Cell {
    CellBytes value{};
    uint64_t epoch = 0;
    bool valid = false;
};
// No reference back to the resource or manager: attaching this object cannot
// create a COM cycle. Pending tickets live separately in the bounded manager.
struct __declspec(uuid("FB267A8C-C3C1-4822-ACBD-F47FA124219A")) Facts final : IUnknown {
    std::atomic<ULONG> references{1};
    const std::shared_ptr<Budget> budget;
    const D3D11_BUFFER_DESC description;
    std::mutex mutex;
    std::unique_ptr<unsigned char[]> snapshot;
    std::map<unsigned, Cell> cells;
    const unsigned char* mapping = nullptr;
    uint64_t charged = sizeof(Facts), revision = 1, snapshotEpoch = 0, mappingEpoch = 0;
    bool snapshotValid = false;
    Facts(std::shared_ptr<Budget> owner, const D3D11_BUFFER_DESC& desc)
        : budget(std::move(owner)), description(desc) {
        ++budget->liveBuffers;
    }
    ~Facts() {
        budget->bytes.fetch_sub(charged);
        --budget->liveBuffers;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(Facts))
            return E_NOINTERFACE;
        *output = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references;
        if (!left)
            delete this;
        return left;
    }
    void invalidate() {
        ++revision;
        snapshotValid = false;
        for (auto& entry : cells)
            entry.second.valid = false;
        ++budget->invalidations;
    }
    bool ensureSnapshot() {
        if (snapshot)
            return true;
        const auto size = description.ByteWidth;
        if (size > ConstantShadow::smallBytes || !budget->reserve(size))
            return false;
        try {
            snapshot = std::make_unique<unsigned char[]>(size);
            charged += size;
        } catch (...) {
            budget->bytes.fetch_sub(size);
            return false;
        }
        return true;
    }
    // The caller owns mutex and validates the source lifetime. Complete small
    // snapshots are bounded; large WC pools copy ONLY previously requested cells.
    // Return bytes actually read from source, not subsequent cached-cell copies.
    uint64_t update(unsigned first, unsigned last, const void* source, bool rememberSmall, uint64_t epoch) {
        if (!source || first > last || last > description.ByteWidth) {
            invalidate();
            return 0;
        }
        uint64_t sourceBytes = 0;
        if (rememberSmall && first == 0 && last == description.ByteWidth && ensureSnapshot()) {
            std::memcpy(snapshot.get(), source, last);
            sourceBytes += last;
            budget->copied += last;
            snapshotEpoch = epoch;
            snapshotValid = true;
        } else if (rememberSmall && snapshotValid && snapshotEpoch == epoch) {
            std::memcpy(snapshot.get() + first, source, last - first);
            sourceBytes += last - first;
            budget->copied += last - first;
        } else
            snapshotValid = false;
        for (auto& [offset, cell] : cells) {
            const auto bytes = std::min(cellBytes, description.ByteWidth - offset);
            const uint64_t end = static_cast<uint64_t>(offset) + bytes;
            if (snapshotValid && snapshotEpoch == epoch) {
                std::memcpy(cell.value.data(), snapshot.get() + offset, bytes);
                cell.valid = true;
                cell.epoch = epoch;
                budget->copied += bytes;
            } else if (offset >= first && end <= last) {
                std::memcpy(cell.value.data(), static_cast<const unsigned char*>(source) + offset - first,
                            bytes);
                sourceBytes += bytes;
                cell.valid = true;
                cell.epoch = epoch;
                budget->copied += bytes;
            } else if (offset < last && end > first)
                cell.valid = false;
        }
        return sourceBytes;
    }
    bool readCell(unsigned offset, CellBytes& output, const char*& failure) {
        if (mapping) {
            failure = "buffer-mapped";
            return false;
        }
        if (offset >= description.ByteWidth || offset % cellBytes) {
            failure = "cell-physical-range";
            return false;
        }
        auto found = cells.find(offset);
        if (found == cells.end()) {
            if (cells.size() >= ConstantShadow::maximumCells) {
                failure = "cell-limit";
                return false;
            }
            constexpr auto cost = sizeof(Cell) + 64;
            if (!budget->reserve(cost)) {
                failure = "byte-budget";
                return false;
            }
            try {
                found = cells.emplace(offset, Cell{}).first;
                charged += cost;
            } catch (...) {
                budget->bytes.fetch_sub(cost);
                throw;
            }
        }
        auto& cell = found->second;
        const auto epoch = budget->epoch.load();
        // A newly received full snapshot also heals cells registered by the
        // earlier failed read; it is not restricted to newly inserted entries.
        if (snapshotValid && (description.Usage == D3D11_USAGE_IMMUTABLE || snapshotEpoch == epoch)) {
            std::memcpy(cell.value.data(), snapshot.get() + offset,
                        std::min(cellBytes, description.ByteWidth - offset));
            cell.valid = true;
            cell.epoch = epoch;
        }
        if (!cell.valid) {
            failure = "cell-unobserved";
            return false;
        }
        if (description.Usage != D3D11_USAGE_IMMUTABLE && cell.epoch != epoch) {
            failure = "cell-epoch";
            return false;
        }
        output = cell.value;
        return true;
    }
};
ComPtr<Facts> facts(ID3D11Resource* resource) {
    ComPtr<IUnknown> stored;
    UINT size = sizeof(IUnknown*);
    if (!resource || FAILED(resource->GetPrivateData(bufferKey, &size, stored.GetAddressOf())) ||
        size != sizeof(IUnknown*))
        return {};
    ComPtr<Facts> result;
    if (stored)
        stored.As(&result);
    return result;
}
ComPtr<IUnknown> identity(IUnknown* object) {
    ComPtr<IUnknown> result;
    if (object)
        object->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
const char* rangeFailure(BoundReadFailure failure) {
    switch (failure) {
    case BoundReadFailure::Components:
        return "pin-components";
    case BoundReadFailure::Alignment:
        return "pin-alignment";
    case BoundReadFailure::Window:
        return "window-count";
    case BoundReadFailure::WindowRange:
        return "shader-window-range";
    case BoundReadFailure::AllocationRange:
        return "physical-allocation-range";
    default:
        return "bound-read-failure";
    }
}
} // namespace
struct ConstantShadow::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> timeline;
    ComPtr<ID3D11Fence> completion;
    uint64_t nextCompletion = 0;
    ComPtr<IUnknown> deviceIdentity;
    const std::shared_ptr<Budget> budget;
    std::mutex adoptionMutex;
    struct Ticket {
        ComPtr<ID3D11Buffer> source, staging;
        ComPtr<IUnknown> sourceIdentity;
        ComPtr<Facts> value;
        uint64_t revision = 0, epoch = 0, charged = 0, completionValue = 0;
        unsigned first = 0, last = 0; // Exact copied source-byte interval, staging starts at zero.
        bool failed = false;
    };
    std::array<Ticket, maximumPending> tickets;
    std::shared_ptr<Impl> quarantine;
    Impl(ID3D11Device* d, ID3D11DeviceContext* c, uint64_t limit)
        : device(d), context(c), deviceIdentity(identity(d)), budget(std::make_shared<Budget>(limit)) {
        if (!d || !c || !limit || limit > maximumBytes || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
            throw std::invalid_argument("Constant shadow requires a bounded immediate-context owner");
        ComPtr<ID3D11Device> actual;
        c->GetDevice(&actual);
        if (identity(actual.Get()).Get() != deviceIdentity.Get())
            throw std::invalid_argument("Constant shadow context belongs to another device");
        ComPtr<ID3D11Device5> newer;
        if (FAILED(device.As(&newer)) || FAILED(context.As(&timeline)) ||
            FAILED(newer->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion))))
            throw std::runtime_error("Constant shadow requires its own GPU retirement fence");
    }
    ComPtr<Facts> adopt(ID3D11Resource* resource, const void* initial, const char*& failure) {
        failure = nullptr;
        if (!budget->active) {
            failure = "shadow-stopped";
            return {};
        }
        if (!resource) {
            failure = "no-buffer";
            return {};
        }
        std::lock_guard lock(adoptionMutex);
        // stop() closes this same gate before another owner may replace facts.
        // A callback that entered before shutdown must not attach afterwards.
        if (!budget->active) {
            failure = "shadow-stopped";
            return {};
        }
        auto value = facts(resource);
        if (value && value->budget == budget)
            return value;
        if (value && value->budget->active) {
            failure = "foreign-active-owner";
            return {};
        }
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
            failure = "not-buffer";
            return {};
        }
        ComPtr<ID3D11Device> sourceDevice;
        buffer->GetDevice(&sourceDevice);
        if (identity(sourceDevice.Get()).Get() != deviceIdentity.Get()) {
            failure = "foreign-device";
            return {};
        }
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        if (!constantBuffer(desc) && !inputBuffer(desc)) {
            failure = "unsupported-buffer";
            return {};
        }
        if (!budget->reserve(sizeof(Facts))) {
            failure = "byte-budget";
            return {};
        }
        ComPtr<Facts> next;
        try {
            next.Attach(new Facts(budget, desc));
        } catch (...) {
            budget->bytes.fetch_sub(sizeof(Facts));
            throw;
        }
        if (initial)
            next->update(0, desc.ByteWidth, initial, desc.ByteWidth <= smallBytes, budget->epoch.load());
        const auto attached = buffer->SetPrivateDataInterface(bufferKey, next.Get());
        if (FAILED(attached)) {
            ++budget->attachmentFailures;
            failure = "attach-facts-failed";
            return {};
        }
        ++budget->adopted;
        return next;
    }
    ComPtr<Facts> owned(ID3D11Resource* resource) {
        auto value = facts(resource);
        return value && value->budget == budget ? value : ComPtr<Facts>{};
    }
    // Called with Facts::mutex held, on the serialized render thread only.
    const char* request(ID3D11Buffer* source, const ComPtr<Facts>& value, unsigned first, unsigned last) {
        if (value->mapping)
            return "buffer-mapped";
        const auto width = value->description.ByteWidth;
        // The CB route retains its existing large-pool sparse-write contract.
        // IA may read back only its requested cells, never the whole large pool.
        if (constantBuffer(value->description) && width > smallBytes)
            return "cold-large-buffer";
        if (width <= smallBytes) {
            first = 0;
            last = width;
        }
        if (first >= last || last > width || first % cellBytes || (last != width && last % cellBytes) ||
            static_cast<uint64_t>(last) - first > static_cast<uint64_t>(maximumCells) * cellBytes)
            return "cold-readback-range";
        auto sourceIdentity = identity(source);
        if (!sourceIdentity)
            return "resource-identity-unavailable";
        Ticket* empty = nullptr;
        for (auto& ticket : tickets) {
            if (ticket.sourceIdentity.Get() == sourceIdentity.Get())
                return ticket.failed ? "cold-readback-failed" : "cold-readback-pending";
            if (!ticket.staging && !empty)
                empty = &ticket;
        }
        if (!empty)
            return "cold-readback-slots-full";
        if (nextCompletion == UINT64_MAX - 1)
            return "cold-fence-exhausted";
        ComPtr<ID3D11Predicate> predicate;
        BOOL enabled = FALSE;
        context->GetPredication(&predicate, &enabled);
        if (predicate)
            return "cold-readback-predicated";
        const auto bytes = last - first;
        if (!budget->reserve(bytes))
            return "byte-budget";
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &staging))) {
            budget->bytes.fetch_sub(bytes);
            ++budget->failed;
            return "cold-staging-create-failed";
        }
        // Publish ticket ownership before issuing the asynchronous copy. A
        // source mutation only changes its revision; it NEVER frees this slot.
        empty->source = source;
        empty->sourceIdentity = std::move(sourceIdentity);
        empty->staging = std::move(staging);
        empty->value = value;
        empty->first = first;
        empty->last = last;
        empty->revision = value->revision;
        empty->epoch = budget->epoch.load();
        empty->charged = bytes;
        ++budget->pending;
        ++budget->queued;
        capture::Bypass bypass;
        if (first == 0 && last == width)
            context->CopyResource(empty->staging.Get(), source);
        else {
            const D3D11_BOX box{first, 0, 0, last, 1, 1};
            context->CopySubresourceRegion(empty->staging.Get(), 0, 0, 0, 0, source, 0, &box);
        }
        const auto valueCompleted = ++nextCompletion;
        if (FAILED(timeline->Signal(completion.Get(), valueCompleted))) {
            // The copy was already submitted. No successful signal means no
            // retirement receipt, even if a later ticket's signal succeeds.
            empty->failed = true;
            ++budget->failed;
            return "cold-signal-failed";
        }
        empty->completionValue = valueCompleted;
        return "cold-readback-queued";
    }
    void retire(Ticket& ticket) noexcept {
        const auto charge = ticket.charged;
        ticket = {};
        budget->bytes.fetch_sub(charge);
        --budget->pending;
    }
    void collect() noexcept {
        capture::Bypass bypass;
        const auto completed = completion->GetCompletedValue();
        // UINT64_MAX denotes device removal, not completion of every ticket.
        if (completed == UINT64_MAX)
            return;
        for (auto& ticket : tickets) {
            if (!ticket.staging || !ticket.completionValue || completed < ticket.completionValue)
                continue;
            // Stopping discards data, not GPU ownership. Once this exact copy's
            // fence has retired, discarding needs no CPU staging Map at all.
            if (!budget->active || ticket.failed) {
                if (!ticket.failed)
                    ++budget->stale;
                retire(ticket);
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto result =
                context->Map(ticket.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (result == DXGI_ERROR_WAS_STILL_DRAWING)
                continue;
            if (FAILED(result)) {
                ticket.failed = true;
                ++budget->failed;
                continue;
            }
            // GPU retirement alone cannot authorize CPU data. Publication still
            // requires a successful nonblocking Map and the complete identity /
            // owner / revision / epoch check below.
            try {
                auto current = facts(ticket.source.Get());
                auto& value = *ticket.value.Get();
                std::lock_guard lock(value.mutex);
                if (mapped.pData && budget->active && current.Get() == ticket.value.Get() &&
                    value.budget == budget && ticket.revision == value.revision &&
                    ticket.epoch == budget->epoch.load() && !value.mapping) {
                    const auto epoch = ticket.epoch;
                    const bool smallSnapshot = value.description.ByteWidth <= smallBytes;
                    if (!smallSnapshot || value.ensureSnapshot()) {
                        value.update(ticket.first, ticket.last, mapped.pData, smallSnapshot, epoch);
                        if (epoch == budget->epoch.load() && budget->active)
                            ++budget->published;
                        else {
                            value.invalidate();
                            ++budget->stale;
                        }
                    }
                } else
                    ++budget->stale;
            } catch (...) {
                ++budget->failed;
            }
            context->Unmap(ticket.staging.Get(), 0);
            retire(ticket);
        }
    }
};
ConstantShadow::ConstantShadow(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t byteLimit)
    : impl_(std::make_shared<Impl>(device, context, byteLimit)) {}
ConstantShadow::~ConstantShadow() {
    // The last callback lease can die on an application worker, without the
    // render-thread graphics lock. Destruction must never call Map/Unmap.
    // Explicit stop()/collect() is the only retirement path; unknown GPU work
    // keeps its bounded lease and charge rather than being released speculatively.
    auto& s = *impl_;
    if (s.budget->active.exchange(false))
        ++s.budget->epoch;
    if (s.budget->pending) {
        s.budget->quarantined = true;
        s.quarantine = impl_;
    }
}
bool ConstantShadow::created(ID3D11Buffer* buffer, const void* initial) noexcept {
    try {
        const char* failure = nullptr;
        return impl_->adopt(buffer, initial, failure).Get() != nullptr;
    } catch (...) {
        return false;
    }
}
bool ConstantShadow::read(ID3D11Buffer* buffer, unsigned first, unsigned count, unsigned offset,
                          unsigned components, std::array<float, 4>& output, ShadowReadInfo& info) noexcept {
    output = {};
    info = {};
    try {
        auto& s = *impl_;
        if (buffer)
            buffer->GetDesc(&info.description);
        if (buffer && !constantBuffer(info.description)) {
            info.failure = "invalid-constant-buffer";
            return false;
        }
        auto value = s.adopt(buffer, nullptr, info.failure);
        if (!value)
            return false;
        info.owned = true;
        std::lock_guard lock(value->mutex);
        const auto epoch = s.budget->epoch.load();
        BoundReadFailure failure = BoundReadFailure::None;
        if (!readBoundFloats(
                info.description.ByteWidth, first, count, offset, components,
                [&](unsigned position, std::array<float, 4>& cell) {
                    CellBytes bytes{};
                    if (!value->readCell(position, bytes, info.failure))
                        return false;
                    std::memcpy(cell.data(), bytes.data(), bytes.size());
                    return true;
                },
                output, &failure)) {
            if (failure != BoundReadFailure::Cell)
                info.failure = rangeFailure(failure);
            else if (info.failure && (std::strcmp(info.failure, "cell-unobserved") == 0 ||
                                      std::strcmp(info.failure, "cell-epoch") == 0))
                info.failure = s.request(buffer, value, 0, info.description.ByteWidth);
            return false;
        }
        if (!s.budget->active ||
            (value->description.Usage != D3D11_USAGE_IMMUTABLE && epoch != s.budget->epoch.load())) {
            output = {};
            info.failure = "read-epoch-changed";
            return false;
        }
        info.failure = nullptr;
        return true;
    } catch (...) {
        output = {};
        info.failure = "shadow-read-exception";
        return false;
    }
}
bool ConstantShadow::readBytes(ID3D11Buffer* buffer, unsigned byteOffset, unsigned byteCount, void* output,
                               ShadowReadInfo& info) noexcept {
    info = {};
    const auto fail = [&](const char* reason) {
        if (output && byteCount)
            std::memset(output, 0, byteCount);
        info.failure = reason;
        return false;
    };
    if (output && byteCount)
        std::memset(output, 0, byteCount);
    try {
        auto& s = *impl_;
        if (buffer)
            buffer->GetDesc(&info.description);
        if (!output || !byteCount)
            return fail("byte-read-output");
        if (!buffer)
            return fail("no-buffer");
        if (!inputBuffer(info.description))
            return fail("invalid-input-buffer");
        const uint64_t end = static_cast<uint64_t>(byteOffset) + byteCount;
        if (end > info.description.ByteWidth)
            return fail("physical-allocation-range");
        const uint64_t first = byteOffset - byteOffset % cellBytes;
        const uint64_t last =
            std::min<uint64_t>((end + cellBytes - 1) / cellBytes * cellBytes, info.description.ByteWidth);
        if ((end - 1) / cellBytes - first / cellBytes + 1 > maximumCells)
            return fail("cell-limit");
        auto value = s.adopt(buffer, nullptr, info.failure);
        if (!value)
            return false;
        info.owned = true;
        std::lock_guard lock(value->mutex);
        const auto epoch = s.budget->epoch.load();
        bool cold = false;
        for (uint64_t position = first; position < end; position += cellBytes) {
            CellBytes cell{};
            if (!value->readCell(static_cast<unsigned>(position), cell, info.failure)) {
                if (info.failure && (std::strcmp(info.failure, "cell-unobserved") == 0 ||
                                     std::strcmp(info.failure, "cell-epoch") == 0))
                    cold = true; // Register every requested cell before queuing one bounded copy.
                else
                    return fail(info.failure);
            } else {
                const auto begin = std::max<uint64_t>(position, byteOffset);
                const auto finish = std::min<uint64_t>(position + cellBytes, end);
                std::memcpy(static_cast<unsigned char*>(output) + (begin - byteOffset),
                            cell.data() + (begin - position), static_cast<size_t>(finish - begin));
            }
        }
        if (cold)
            return fail(s.request(buffer, value, static_cast<unsigned>(first), static_cast<unsigned>(last)));
        if (!s.budget->active ||
            (value->description.Usage != D3D11_USAGE_IMMUTABLE && epoch != s.budget->epoch.load()))
            return fail("read-epoch-changed");
        info.failure = nullptr;
        return true;
    } catch (...) {
        return fail("shadow-byte-read-exception");
    }
}
void ConstantShadow::collect() noexcept {
    impl_->collect();
}
bool ConstantShadow::stop() noexcept {
    auto& s = *impl_;
    {
        std::lock_guard lock(s.adoptionMutex);
        if (s.budget->active.exchange(false))
            ++s.budget->epoch;
    }
    // Do not hold adoptionMutex across driver calls: device hooks may reenter
    // created(), and Facts locks are acquired by both collection and callbacks.
    s.collect();
    return s.budget->pending == 0;
}
void ConstantShadow::mapped(ID3D11DeviceContext* context, ID3D11Resource* resource, unsigned sub,
                            D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE& mapping) noexcept {
    if (sub || type == D3D11_MAP_READ)
        return;
    try {
        auto& s = *impl_;
        const char* failure = nullptr;
        auto value = s.adopt(resource, nullptr, failure);
        if (!value)
            return;
        std::lock_guard lock(value->mutex);
        value->invalidate();
        value->mapping = nullptr;
        if (context == s.context.Get() && mapping.pData) {
            value->mapping = static_cast<const unsigned char*>(mapping.pData);
            value->mappingEpoch = s.budget->epoch.load();
        }
    } catch (...) {
    }
}
void ConstantShadow::beforeUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                                 unsigned sub) noexcept {
    if (sub)
        return;
    try {
        auto& s = *impl_;
        auto value = s.owned(resource);
        if (!value)
            return;
        std::lock_guard lock(value->mutex);
        // Clear the retained pointer even when allocation/copy bookkeeping fails.
        const auto* source = value->mapping;
        value->mapping = nullptr;
        if (!s.budget->active || !source || context != s.context.Get())
            return;
        const auto epoch = s.budget->epoch.load();
        if (value->mappingEpoch != epoch) {
            value->invalidate();
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const auto copied = value->update(0, value->description.ByteWidth, source,
                                          value->description.ByteWidth <= smallBytes, epoch);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                .count();
        if (copied) {
            s.budget->wcBytes += copied;
            ++s.budget->wcCopies;
            s.budget->wcNanoseconds += static_cast<uint64_t>(elapsed);
        }
        if (epoch != s.budget->epoch.load())
            value->invalidate();
    } catch (...) {
        invalidated(resource);
    }
}
void ConstantShadow::beforeUpdate(ID3D11DeviceContext* context, ID3D11Resource* resource, unsigned sub,
                                  const D3D11_BOX* box, const void* source, unsigned, unsigned,
                                  unsigned flags) noexcept {
    if (sub)
        return;
    try {
        auto& s = *impl_;
        const char* failure = nullptr;
        auto value = s.adopt(resource, nullptr, failure);
        if (!value)
            return;
        std::lock_guard lock(value->mutex);
        ++value->revision;
        if (context != s.context.Get() || (flags & ~(D3D11_COPY_DISCARD | D3D11_COPY_NO_OVERWRITE)) ||
            (box && (box->top || box->front || box->bottom != 1 || box->back != 1))) {
            value->invalidate();
            return;
        }
        // UpdateSubresource is predicated. Observing its CPU argument is not
        // proof the GPU accepted it when a predicate can skip the real update.
        ComPtr<ID3D11Predicate> predicate;
        BOOL enabled = FALSE;
        context->GetPredication(&predicate, &enabled);
        if (predicate) {
            value->invalidate();
            return;
        }
        if (flags & D3D11_COPY_DISCARD)
            value->invalidate();
        const auto epoch = s.budget->epoch.load();
        value->update(box ? box->left : 0, box ? box->right : value->description.ByteWidth, source, true,
                      epoch);
        if (epoch != s.budget->epoch.load())
            value->invalidate();
    } catch (...) {
        invalidated(resource);
    }
}
void ConstantShadow::invalidated(ID3D11Resource* resource) noexcept {
    try {
        auto& s = *impl_;
        if (!s.budget->active)
            return;
        if (!resource) {
            ++s.budget->epoch;
            ++s.budget->invalidations;
            return;
        }
        if (auto value = s.owned(resource)) {
            std::lock_guard lock(value->mutex);
            value->invalidate();
        }
    } catch (...) {
    }
}
ShadowStats ConstantShadow::stats() const noexcept {
    const auto& b = *impl_->budget;
    ShadowStats result;
    result.bytes = b.bytes;
    result.peakBytes = b.peak;
    result.copiedBytes = b.copied;
    result.invalidations = b.invalidations;
    result.wcBytes = b.wcBytes;
    result.wcCopies = b.wcCopies;
    result.wcNanoseconds = b.wcNanoseconds;
    result.queued = b.queued;
    result.published = b.published;
    result.stale = b.stale;
    result.failed = b.failed;
    result.pending = b.pending;
    result.adopted = b.adopted;
    result.attachmentFailures = b.attachmentFailures;
    result.liveBuffers = b.liveBuffers;
    result.stopped = !b.active;
    result.quarantined = b.quarantined;
    return result;
}
} // namespace dspaa::proof
