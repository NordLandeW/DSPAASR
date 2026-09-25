#include "present/latency.h"
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
HANDLE duplicate(HANDLE input) {
    HANDLE result = nullptr;
    require(DuplicateHandle(GetCurrentProcess(), input, GetCurrentProcess(), &result, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE,
            "duplicate backend handle");
    return result;
}
void take(HANDLE handle) {
    require(WaitForSingleObject(handle, 10000) == WAIT_OBJECT_0, "semantic latency grant did not arrive");
}
}
int main() {
    try {
        dspaa::PresentationLatency relay;
        require(!relay.active() && relay.pause(), "unbound latency state");
        require(!relay.bind(nullptr), "null backend accepted");
        Handle first{CreateSemaphoreW(nullptr, 0, 16, nullptr)};
        Handle second{CreateSemaphoreW(nullptr, 0, 16, nullptr)};
        require(first.value && second.value, "create synthetic backend semaphores");
        require(relay.bind(duplicate(first.value)) && relay.active(), "bind initial backend");
        Handle host{relay.duplicateHostHandle()};
        Handle otherHost{relay.duplicateHostHandle()};
        require(host.value && otherHost.value && host.value != otherHost.value, "independently owned host duplicates");
        require(WaitForSingleObject(host.value, 0) == WAIT_TIMEOUT, "host grant arrived without backend readiness");
        require(ReleaseSemaphore(first.value, 1, nullptr) != FALSE, "signal initial backend");
        take(host.value);
        require(ReleaseSemaphore(first.value, 1, nullptr) != FALSE, "signal unrequested readiness");
        require(WaitForSingleObject(first.value, 0) == WAIT_OBJECT_0, "relay consumed readiness without application demand");
        require(WaitForSingleObject(host.value, 0) == WAIT_TIMEOUT, "relay fabricated another frame grant");
        relay.presented();
        require(ReleaseSemaphore(first.value, 1, nullptr) != FALSE, "complete first application present");
        take(otherHost.value);
        require(relay.pause() && !relay.active(), "stop first relay worker");
        require(relay.bind(duplicate(second.value)) && relay.active(), "bind replacement backend");
        relay.presented();
        require(ReleaseSemaphore(second.value, 1, nullptr) != FALSE, "complete replacement application present");
        take(host.value); // This handle predates replacement and must still work.
        CloseHandle(otherHost.value); otherHost.value = nullptr;
        relay.presented();
        require(ReleaseSemaphore(second.value, 1, nullptr) != FALSE, "complete after other host duplicate closed");
        take(host.value);
        require(relay.pause() && relay.pause(), "idempotent relay retirement");
        require(ReleaseSemaphore(first.value, 1, nullptr) && ReleaseSemaphore(second.value, 1, nullptr), "caller backend handles were incorrectly closed");
        require(WaitForSingleObject(host.value, 0) == WAIT_TIMEOUT, "retired backend still issued host grants");
        std::puts("presentation latency: readiness/demand, stable host identity, independent ownership and retirement passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
