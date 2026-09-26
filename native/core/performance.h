#pragma once

// Explicit diagnostic builds only. Normal builds compile every scope away.
#if defined(DSPAA_ENABLE_PERFORMANCE_METRICS)
#include <array>
#include <atomic>
#include <cstdint>
#include <windows.h>

namespace dspaa::performance {
enum class Metric : uint32_t {
    ReflexSleep,
    SlPresent,
    SlCopiedWait,
    SlConsumedWait,
    TransportCapture,
    TransportSlotWait,
    ProducerFlush,
    FsrPresent,
    FsrSlotWait,
    FacadePresent,
    FacadeLockWait,
    SlCopySubmit,
    SlTag,
    SlQuery,
    FsrUpload,
    FsrDispatch,
    WorldCapture,
    NativePresent,
    Count
};
struct Counter {
    std::atomic<uint64_t> count{0}, ticks{0}, maximum{0};
};
inline std::array<Counter, static_cast<size_t>(Metric::Count)> counters;
inline void record(Metric metric, uint64_t ticks) noexcept {
    auto& counter = counters[static_cast<size_t>(metric)];
    counter.ticks.fetch_add(ticks, std::memory_order_relaxed);
    auto maximum = counter.maximum.load(std::memory_order_relaxed);
    while (maximum < ticks &&
           !counter.maximum.compare_exchange_weak(maximum, ticks, std::memory_order_relaxed)) {
    }
    counter.count.fetch_add(1, std::memory_order_release);
}
class Scope {
    Metric metric_;
    LARGE_INTEGER start_{};

  public:
    explicit Scope(Metric metric) noexcept : metric_(metric) {
        QueryPerformanceCounter(&start_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        LARGE_INTEGER end{};
        if (QueryPerformanceCounter(&end) && start_.QuadPart > 0 && end.QuadPart >= start_.QuadPart)
            record(metric_, static_cast<uint64_t>(end.QuadPart - start_.QuadPart));
    }
};
} // namespace dspaa::performance
#define DSPAA_PERF_JOIN_INNER(a, b) a##b
#define DSPAA_PERF_JOIN(a, b) DSPAA_PERF_JOIN_INNER(a, b)
#define DSPAA_PERF_SCOPE(metric)                                                                             \
    ::dspaa::performance::Scope DSPAA_PERF_JOIN(dspaaPerformanceScope,                                       \
                                                __LINE__)(::dspaa::performance::Metric::metric)
#else
#define DSPAA_PERF_SCOPE(metric) ((void)0)
#endif
