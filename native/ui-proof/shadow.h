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
// CB constants and ordinary IA vertex/index bytes share ONE resource-private
// owner, write observation, budget, cold readback and retirement lifecycle.
// Write callbacks never submit context commands. Reads/collect/stop are serialized
// render-thread operations under the caller's graphics lock; reads require its
// no-active-query/private-copy permission. Reads and collect never wait, flush,
// or map the original resource for reading.
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
              std::array<float, 4>& output, ShadowReadInfo& info) noexcept;
    // IA only: VB/IB, optionally SRV, with no GPU-write bind or miscellaneous
    // sharing/tiled-resource contract. Uses the actual ByteWidth, not a CB window.
    // The caller provides byteCount writable output bytes; failure clears them.
    bool readBytes(ID3D11Buffer* buffer, unsigned byteOffset, unsigned byteCount, void* output,
                   ShadowReadInfo& info) noexcept;
    void collect() noexcept;
    // Nonblocking. Stops adoption/publication, then discards tickets only after
    // their own copy-completion fence retires; this never requires a CPU Map.
    // Failed signals, device removal and incomplete copies keep their leases and
    // charges. Normal shutdown drains the same graphics context, then calls stop
    // again. Destruction issues no context commands and quarantines unknown work.
    bool stop() noexcept;
    ShadowStats stats() const noexcept;
    void mapped(ID3D11DeviceContext*, ID3D11Resource*, unsigned, D3D11_MAP,
                const D3D11_MAPPED_SUBRESOURCE&) noexcept override;
    void beforeUnmap(ID3D11DeviceContext*, ID3D11Resource*, unsigned) noexcept override;
    void beforeUpdate(ID3D11DeviceContext*, ID3D11Resource*, unsigned, const D3D11_BOX*, const void*,
                      unsigned, unsigned, unsigned) noexcept override;
    void invalidated(ID3D11Resource*) noexcept override;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace dspaa::proof
