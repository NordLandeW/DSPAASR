#include "channel.h"
#include "world-color.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace dspaa {
namespace {
struct Broker {
    std::mutex mutex;
    bool initialized = false;
    std::shared_ptr<SlRuntime> runtime;
    std::string failure;
    std::weak_ptr<PresentationChannel> channel;
    struct Pending {
        std::shared_ptr<PresentationChannel> channel;
        std::unique_ptr<PresentationSubmission> frame;
    };
    std::unordered_map<uintptr_t, Pending> commands;
    uintptr_t nextToken = 1;
    std::atomic<uint64_t> nextApplicationFrame{0};
};
Broker& broker() {
    static auto* instance = new Broker;
    return *instance;
}
void message(DspAaPresentationStatus& status, const char* text) {
    strncpy_s(status.message, text ? text : "", _TRUNCATE);
}
void __stdcall renderEvent(int event, void* token) noexcept {
    try {
        if (!token)
            return;
        if (event == 1) {
            if (auto channel = presentationChannel()) {
                const auto id = reinterpret_cast<uintptr_t>(token);
                channel->marker(id, SlMarker::RenderSubmitStart);
            }
        } else if (event == 2) {
            Broker::Pending pending;
            auto& b = broker();
            {
                std::lock_guard lock(b.mutex);
                const auto found = b.commands.find(reinterpret_cast<uintptr_t>(token));
                if (found == b.commands.end())
                    return;
                pending = std::move(found->second);
                b.commands.erase(found);
            }
            if (auto world = pending.channel->worldColor())
                world->attach(*pending.frame);
            pending.channel->publish(std::move(pending.frame));
        } else if (event == 3) {
            if (auto channel = presentationChannel())
                if (auto world = channel->worldColor())
                    world->capture(reinterpret_cast<uintptr_t>(token));
        }
    } catch (...) {
    } // No exception crosses a Unity render-event boundary.
}
} // namespace
void initializePresentationRuntime(const std::filesystem::path& runtime, const std::filesystem::path& logs,
                                   PresentationLog log) noexcept {
    auto& b = broker();
    std::lock_guard lock(b.mutex);
    if (b.initialized)
        return;
    b.initialized = true;
    try {
        SlRuntimeCreateInfo info;
        info.runtimeDirectory = runtime;
        info.logDirectory = logs;
        b.runtime = std::make_shared<SlRuntime>(info);
    } catch (const std::exception& error) {
        b.failure = error.what();
    } catch (...) {
        b.failure = "Unknown early Streamline initialization failure";
    }
    if (log && !b.failure.empty())
        log(b.failure.c_str());
}
std::shared_ptr<SlRuntime> earlyPresentationRuntime() {
    auto& b = broker();
    std::lock_guard lock(b.mutex);
    return b.runtime;
}
std::string presentationRuntimeFailure() {
    auto& b = broker();
    std::lock_guard lock(b.mutex);
    return b.failure;
}
std::shared_ptr<PresentationChannel> registerPresentationChannel(const std::filesystem::path& runtime,
                                                                 PresentationLog log) {
    auto& b = broker();
    std::lock_guard lock(b.mutex);
    if (auto prior = b.channel.lock(); prior && (prior->status().flags & 1u))
        throw std::runtime_error("Another live facade owns the presentation mailbox");
    auto channel = std::make_shared<PresentationChannel>(
        b.runtime, std::filesystem::is_regular_file(runtime / L"amd_fidelityfx_framegeneration_dx12.dll"),
        log);
    b.channel = channel;
    return channel;
}
std::shared_ptr<PresentationChannel> presentationChannel() {
    auto& b = broker();
    std::lock_guard lock(b.mutex);
    return b.channel.lock();
}
PresentationChannel::PresentationChannel(std::shared_ptr<SlRuntime> runtime, bool fsr, PresentationLog log)
    : runtime_(std::move(runtime)), log_(log) {
    status_.size = sizeof(status_);
    status_.version = DspAaPresentationAbiVersion;
    status_.flags = 1u | (fsr ? 2u : 0u);
    message(status_, "Native presentation; frame generation is off");
}
bool PresentationChannel::configure(const DspAaPresentationConfiguration& value) {
    if (value.backend > 2 || value.mode > 1 || value.reflex > 2 || !value.generatedFrames ||
        !std::isfinite(value.dynamicTargetFrameRate) || value.dynamicTargetFrameRate < 0 ||
        (value.backend == 1 && (value.mode || value.generatedFrames != 1)))
        return false;
    std::lock_guard lock(mutex_);
    if (!attached_)
        return false;
    configuration_.backend = value.backend;
    configuration_.mode = value.mode;
    configuration_.generatedFrames = value.generatedFrames;
    configuration_.reflex = value.reflex;
    configuration_.dynamicTargetFrameRate = value.dynamicTargetFrameRate;
    configuration_.frameLimitMicroseconds = value.frameLimitMicroseconds;
    ++configuration_.revision;
    status_.requestedBackend = value.backend;
    return true;
}
PresentationConfiguration PresentationChannel::configuration() const {
    std::lock_guard lock(mutex_);
    return configuration_;
}
bool PresentationChannel::begin(DspAaPresentationBegin& frame) {
    {
        std::lock_guard lock(mutex_);
        if (!attached_)
            return false;
        nextFrame_ = ++broker().nextApplicationFrame;
        frame.applicationFrameId = nextFrame_;
        frame.generation = status_.generation;
        status_.lastApplicationFrameId = frame.applicationFrameId;
    }
    if (runtime_ && runtime_->beginFrame(frame.applicationFrameId))
        runtime_->marker(frame.applicationFrameId, SlMarker::SimulationStart);
    return true;
}
bool PresentationChannel::marker(uint64_t id, SlMarker phase) noexcept {
    return runtime_ && runtime_->marker(id, phase);
}
void PresentationChannel::bindWorldColor(const std::shared_ptr<WorldColor>& world) {
    std::lock_guard lock(mutex_);
    world_ = world;
}
std::shared_ptr<WorldColor> PresentationChannel::worldColor() const {
    std::lock_guard lock(mutex_);
    return attached_ ? world_.lock() : nullptr;
}
void PresentationChannel::publish(std::unique_ptr<PresentationSubmission> frame) {
    std::lock_guard lock(mutex_);
    if (!attached_ || !frame || !frame->metadata.applicationFrameId ||
        frame->metadata.applicationFrameId > nextFrame_ ||
        frame->metadata.applicationFrameId <= lastPublished_)
        return;
    // Publication is a render-thread event; the main thread cannot select a newer
    // simulation token for an older Present. Skipped envelopes are not replayed.
    lastPublished_ = frame->metadata.applicationFrameId;
    pending_ = std::move(frame);
}
std::unique_ptr<PresentationSubmission> PresentationChannel::consume() {
    std::lock_guard lock(mutex_);
    return std::move(pending_);
}
void PresentationChannel::surface(uint64_t generation, uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);
    status_.generation = generation;
    status_.width = width;
    status_.height = height;
}
void PresentationChannel::observe(uint32_t backend, uint64_t frame, const BackendObservation& value) {
    std::lock_guard lock(mutex_);
    status_.activeBackend = backend;
    status_.activeMode = value.mode;
    status_.lastPresentedFrameId = frame;
    status_.flags = (status_.flags & 3u) | (value.dynamic ? 16u : 0u) | (value.vsync ? 32u : 0u) |
                    (value.generating ? 64u : 0u) | (value.quarantined ? 128u : 0u);
    status_.maximumGeneratedFrames = value.maximumGeneratedFrames;
    status_.sdkStatus = value.sdkStatus;
    ++status_.applicationPresents;
    status_.generatedSubmissions = value.generatedSubmissions;
    status_.sdkReportedPresents = value.sdkReportedPresents;
    if (!value.reason.empty())
        message(status_, value.reason.c_str());
    else
        message(
            status_,
            value.generating
                ? "Frame generation enabled; SDK may skip generated presents (for example, while unfocused)"
                : "Frame generation paused or off");
}
void PresentationChannel::reason(const char* text, bool quarantined) {
    std::lock_guard lock(mutex_);
    message(status_, text);
    if (quarantined)
        status_.flags |= 128u;
    if (log_)
        log_(text);
}
void PresentationChannel::detach() noexcept {
    {
        std::lock_guard lock(mutex_);
        attached_ = false;
        pending_.reset();
        status_.flags &= ~1u;
    }
    // Event tokens are handles, not pointers: already queued Unity callbacks can
    // safely observe a miss. Do not let abandoned events retain a dead facade or
    // consume the next surface's bounded command capacity indefinitely.
    auto& b = broker();
    std::lock_guard lock(b.mutex);
    for (auto it = b.commands.begin(); it != b.commands.end();)
        if (it->second.channel.get() == this)
            it = b.commands.erase(it);
        else
            ++it;
}
DspAaPresentationStatus PresentationChannel::status() const {
    DspAaPresentationStatus result;
    {
        std::lock_guard lock(mutex_);
        result = status_;
    }
    if (runtime_) {
        const auto sl = runtime_->status();
        if (sl.dlssSupported)
            result.flags |= 4u;
        if (sl.reflexSupported)
            result.flags |= 8u;
        if (sl.quarantined)
            result.flags |= 128u;
    }
    return result;
}
void* queuePresentationInputs(const DspAaPresentationInputs* value) {
    try {
        if (!value || value->size != sizeof(*value) || value->version != DspAaPresentationAbiVersion ||
            !value->applicationFrameId || value->reserved || (value->flags & ~63u))
            return nullptr;
        auto channel = dspaa::presentationChannel();
        if (!channel)
            return nullptr;
        auto submission = std::make_unique<dspaa::PresentationSubmission>();
        submission->metadata = *value;
        void* resources[] = {value->hudless, value->depth, value->motion, value->fsrDistortion,
                             value->slDistortion};
        for (size_t i = 0; i < std::size(resources); ++i)
            submission->resources[i] = static_cast<ID3D11Resource*>(resources[i]);
        auto& b = dspaa::broker();
        std::lock_guard lock(b.mutex);
        if (b.commands.size() >= 16)
            return nullptr;
        auto id = b.nextToken++;
        while (!id || b.commands.contains(id))
            id = b.nextToken++;
        b.commands.emplace(id, dspaa::Broker::Pending{std::move(channel), std::move(submission)});
        return reinterpret_cast<void*>(id);
    } catch (...) {
        return nullptr;
    }
}
void cancelPresentationInputs(void* token) {
    try {
        auto& b = dspaa::broker();
        std::lock_guard lock(b.mutex);
        b.commands.erase(reinterpret_cast<uintptr_t>(token));
    } catch (...) {
    }
}
DspAaRenderEvent presentationRenderEvent() {
    return renderEvent;
}
} // namespace dspaa
