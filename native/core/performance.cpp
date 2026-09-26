#include "performance.h"

// This file is linked only into an explicitly instrumented diagnostic DLL.
// CPU wall time includes waits and scheduling. Nested scopes overlap; their
// totals must not be added. Counters are cumulative and are never reset while
// producers run. Concurrent snapshots are approximate, not an atomic frame cut.
extern "C" __declspec(dllexport) int __cdecl DspAaReadPerformanceCounter(uint32_t metric, uint64_t* count,
                                                                         uint64_t* ticks, uint64_t* maximum,
                                                                         uint64_t* frequency) noexcept {
    if (metric >= static_cast<uint32_t>(dspaa::performance::Metric::Count) || !count || !ticks || !maximum ||
        !frequency)
        return 0;
    LARGE_INTEGER clock{};
    if (!QueryPerformanceFrequency(&clock) || clock.QuadPart <= 0)
        return 0;
    const auto& counter = dspaa::performance::counters[metric];
    *count = counter.count.load(std::memory_order_acquire);
    *ticks = counter.ticks.load(std::memory_order_relaxed);
    *maximum = counter.maximum.load(std::memory_order_relaxed);
    *frequency = static_cast<uint64_t>(clock.QuadPart);
    return 1;
}
