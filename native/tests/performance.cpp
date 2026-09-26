#include "core/performance.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>

int main() {
    try {
        using namespace dspaa::performance;
        std::array<std::thread, 4> workers;
        for (unsigned i = 0; i < workers.size(); ++i)
            workers[i] = std::thread([i] {
                for (unsigned n = 0; n < 1000; ++n)
                    record(Metric::TransportCapture, i + 1);
            });
        for (auto& worker : workers)
            worker.join();
        const auto& result = counters[static_cast<size_t>(Metric::TransportCapture)];
        if (result.count.load() != 4000 || result.ticks.load() != 10000 || result.maximum.load() != 4)
            throw std::runtime_error("Concurrent timing samples were lost or aggregated incorrectly");
        const auto& untouched = counters[static_cast<size_t>(Metric::ReflexSleep)];
        if (untouched.count.load() || untouched.ticks.load() || untouched.maximum.load())
            throw std::runtime_error("Independent timing categories were mixed");
        {
            DSPAA_PERF_SCOPE(WorldCapture);
        }
        if (counters[static_cast<size_t>(Metric::WorldCapture)].count.load() != 1)
            throw std::runtime_error("Scope did not record its completed CPU interval");
        std::puts(
            "performance counters: concurrent accumulation, category isolation and scoped recording passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
