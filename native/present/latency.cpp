#include "latency.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <process.h>

namespace dspaa {
struct PresentationLatency::Shared {
    // The stable bridge permits at most one unconsumed host grant. In particular,
    // a client that did not wait previously cannot later run through 64 stale grants.
    // This is a conservative upper bound, even if the backend permits more in flight.
    HANDLE host = CreateSemaphoreW(nullptr, 0, 1, nullptr);
    HANDLE requests = CreateSemaphoreW(nullptr, 0, 64, nullptr);
    std::atomic<bool> initialized{false};
    ~Shared() { if (host) CloseHandle(host); if (requests) CloseHandle(requests); }
};
struct PresentationLatency::Run {
    std::shared_ptr<Shared> shared;
    HANDLE backend = nullptr;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> failed{false};
    ~Run() { if (backend) CloseHandle(backend); if (stop) CloseHandle(stop); if (done) CloseHandle(done); }
};
PresentationLatency::PresentationLatency() : shared_(std::make_shared<Shared>()) {
    if (!shared_->host || !shared_->requests) throw std::runtime_error("Cannot allocate facade latency semaphores");
}
PresentationLatency::~PresentationLatency() { (void)pause(); }
bool PresentationLatency::pause() noexcept {
    if (!run_) return true;
    if (!SetEvent(run_->stop) || WaitForSingleObject(run_->done, 30000) != WAIT_OBJECT_0) return false;
    run_.reset();
    return true;
}
bool PresentationLatency::bind(HANDLE ownedBackendHandle) noexcept {
    struct HandleCloser { void operator()(void* handle) const noexcept { if (handle) CloseHandle(handle); } };
    std::unique_ptr<void, HandleCloser> incoming(ownedBackendHandle);
    try {
        if (!incoming || !pause()) return false;
        auto next = std::make_shared<Run>();
        next->backend = incoming.release();
        next->shared = shared_;
        if (!next->stop || !next->done) return false;
        auto context = std::make_unique<std::shared_ptr<Run>>(next);
        const auto thread = _beginthreadex(nullptr, 0, [](void* argument) -> unsigned {
            std::unique_ptr<std::shared_ptr<Run>> owner(static_cast<std::shared_ptr<Run>*>(argument));
            const auto next = *owner;
            bool heldRequest = false;
            for (;;) {
                HANDLE demand[] = {next->stop, next->shared->requests};
                const auto requested = WaitForMultipleObjects(2, demand, FALSE, INFINITE);
                if (requested == WAIT_OBJECT_0) break;
                if (requested != WAIT_OBJECT_0 + 1) { next->failed = true; break; }
                heldRequest = true;
                HANDLE ready[] = {next->stop, next->backend};
                const auto available = WaitForMultipleObjects(2, ready, FALSE, INFINITE);
                if (available == WAIT_OBJECT_0) break;
                if (available != WAIT_OBJECT_0 + 1) { next->failed = true; break; }
                // A client which requests but never waits cannot grow an unbounded
                // relay queue. Its already queued grants remain usable.
                if (!ReleaseSemaphore(next->shared->host, 1, nullptr) && GetLastError() != ERROR_TOO_MANY_POSTS)
                    next->failed = true;
                heldRequest = false;
                if (next->failed) break;
            }
            if (heldRequest) ReleaseSemaphore(next->shared->requests, 1, nullptr);
            if (next->failed) ReleaseSemaphore(next->shared->host, 1, nullptr); // Let the host observe the latched device error.
            SetEvent(next->done);
            return 0;
        }, context.get(), 0, nullptr);
        if (!thread) return false;
        (void)context.release(); // The successfully created worker now owns its argument.
        CloseHandle(reinterpret_cast<HANDLE>(thread));
        run_ = std::move(next);
        if (!shared_->initialized.exchange(true))
            ReleaseSemaphore(shared_->requests, 1, nullptr);
        return true;
    } catch (...) { return false; }
}
HANDLE PresentationLatency::duplicateHostHandle() const noexcept {
    HANDLE result = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), shared_->host, GetCurrentProcess(), &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr;
    return result;
}
void PresentationLatency::presented() noexcept {
    if (run_) ReleaseSemaphore(shared_->requests, 1, nullptr);
}
bool PresentationLatency::active() const noexcept { return run_ && !run_->failed; }
} // namespace dspaa
