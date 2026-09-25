#pragma once
#include "capture/hooks.h"
#include <array>
#include <cstdint>
#include <memory>

namespace dspaa::proof {
struct ShadowReadInfo {
    D3D11_BUFFER_DESC description{};
    bool owned = false;
    const char* failure = nullptr;
};
struct ShadowStats {
    uint64_t bytes = 0, peakBytes = 0, copiedBytes = 0, invalidations = 0;
    uint64_t wcBytes = 0, wcCopies = 0, wcNanoseconds = 0;
    uint64_t queued = 0, published = 0, stale = 0, failed = 0, pending = 0;
    uint64_t adopted = 0, attachmentFailures = 0, liveBuffers = 0;
    bool stopped = false, quarantined = false;
};
// Resource-private CPU shadows, observed writes, and cold readback have ONE
// lifecycle. Write callbacks never submit context commands. read/collect/stop
// are serialized render-thread operations under the caller's graphics lock;
// read requires the caller's no-active-query/private-copy permission. Neither
// read nor collect waits, flushes, or maps the original resource for reading.
class ConstantShadow final : public capture::WriteObserver {
  public:
    static constexpr uint64_t maximumBytes = 16u * 1024u * 1024u;
    static constexpr unsigned smallBytes = 4096, maximumCells = 1024, maximumPending = 8;
    // An isolated probe may narrow, but never enlarge, the production budget.
    ConstantShadow(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t byteLimit = maximumBytes);
    ~ConstantShadow();
    ConstantShadow(const ConstantShadow&) = delete;
    ConstantShadow& operator=(const ConstantShadow&) = delete;
    bool created(ID3D11Buffer* buffer, const void* initial = nullptr) noexcept;
    bool read(ID3D11Buffer* buffer, unsigned first, unsigned count, unsigned offset, unsigned components,
              std::array<float,4>& output, ShadowReadInfo& info) noexcept;
    void collect() noexcept;
    // Nonblocking. An unretired/failed ticket is not recycled. Destruction with
    // unknown retirement quarantines its bounded owner, rather than uncharging
    // live GPU work or retaining a mapped pointer. Normal shutdown drains first.
    bool stop() noexcept;
    ShadowStats stats() const noexcept;
    void mapped(ID3D11DeviceContext*, ID3D11Resource*, unsigned, D3D11_MAP, const D3D11_MAPPED_SUBRESOURCE&) noexcept override;
    void beforeUnmap(ID3D11DeviceContext*, ID3D11Resource*, unsigned) noexcept override;
    void beforeUpdate(ID3D11DeviceContext*, ID3D11Resource*, unsigned, const D3D11_BOX*, const void*, unsigned, unsigned, unsigned) noexcept override;
    void invalidated(ID3D11Resource*) noexcept override;
  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace dspaa::proof
