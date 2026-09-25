#include "present/channel.h"
#include "present/display-layout.h"
#include <cstdio>
#include <stdexcept>
namespace {
void require(bool value, const char* text) {
    if (!value)
        throw std::runtime_error(text);
}
std::unique_ptr<dspaa::PresentationSubmission> packet(uint64_t id) {
    auto result = std::make_unique<dspaa::PresentationSubmission>();
    result->metadata.applicationFrameId = id;
    return result;
}
} // namespace
int main() {
    try {
        // Catch recentering/auto-fitting an observed viewport: Unity's 1019x768
        // logical output actually occupies 1912 display pixels, not L/D's 1910/1911.
        require(
            dspaa::sameRect(dspaa::displayViewport(2560, 1440, 324, 0, 1912, 1440), RECT{324, 0, 2236, 1440}),
            "observed display viewport was recomputed from a guessed aspect ratio");
        bool rejectedViewport = false;
        try {
            (void)dspaa::displayViewport(2560, 1440, 320.5f, 0, 1920, 1440);
        } catch (const std::invalid_argument&) {
            rejectedViewport = true;
        }
        require(rejectedViewport,
                "fractional viewport was silently quantized into a different SDK rectangle");
        require(
            dspaa::sameRect(dspaa::displayRect(2560, 1440, RECT{8, 10, 2000, 1200}), RECT{8, 10, 2000, 1200}),
            "explicit canonical rectangle was recentered or stretched");
        bool rejectedRect = false;
        try {
            (void)dspaa::displayRect(2560, 1440, RECT{-1, 0, 2560, 1440});
        } catch (const std::invalid_argument&) {
            rejectedRect = true;
        }
        require(rejectedRect, "out-of-display content rectangle was accepted");
        auto channel = dspaa::registerPresentationChannel({}, nullptr);
        channel->surface(8, 1280, 720);
        DspAaPresentationBegin first{}, second{};
        require(channel->begin(first) && channel->begin(second), "before-input allocation failed");
        require(first.applicationFrameId && second.applicationFrameId > first.applicationFrameId &&
                    first.generation == 8,
                "application identities did not preserve order/surface");
        require(!channel->consume(), "BeginFrame fabricated an end-of-frame packet");
        channel->publish(packet(second.applicationFrameId + 1));
        require(!channel->consume(), "unallocated future frame reached Present");
        channel->publish(packet(first.applicationFrameId));
        auto observed = channel->consume();
        require(observed && observed->metadata.applicationFrameId == first.applicationFrameId,
                "newer simulation replaced an older render token");
        channel->publish(packet(first.applicationFrameId));
        require(!channel->consume(), "same rendered frame was published twice");
        DspAaPresentationConfiguration configuration{
            sizeof(configuration), DspAaPresentationAbiVersion, 1, 0, 1, 1};
        require(channel->configure(configuration), "fixed analytical FSR selection rejected");
        configuration.mode = 1;
        require(!channel->configure(configuration), "analytical FSR accepted unsupported Dynamic mode");
        DspAaPresentationInputs input{};
        input.size = sizeof(input);
        input.version = DspAaPresentationAbiVersion;
        input.applicationFrameId = second.applicationFrameId;
        input.generation = 8;
        auto token = dspaa::queuePresentationInputs(&input);
        require(token && !channel->consume(), "main-thread enqueue published pixels before render event");
        auto event = dspaa::presentationRenderEvent();
        event(2, token);
        observed = channel->consume();
        require(observed && observed->metadata.applicationFrameId == second.applicationFrameId,
                "render event lost the exact application token");
        event(2, token);
        require(!channel->consume(), "render event handle was replayed");
        DspAaPresentationBegin next{};
        require(channel->begin(next), "third input allocation failed");
        input.applicationFrameId = next.applicationFrameId;
        token = dspaa::queuePresentationInputs(&input);
        require(token != nullptr, "cancel packet allocation failed");
        dspaa::cancelPresentationInputs(token);
        event(2, token);
        require(!channel->consume(), "canceled command reached presentation");
        for (unsigned i = 0; i < 16; ++i)
            require(dspaa::queuePresentationInputs(&input) != nullptr, "bounded mailbox filled too early");
        require(!dspaa::queuePresentationInputs(&input), "bounded mailbox failed backpressure");
        channel->detach();
        require(!channel->begin(next), "detached surface allocated a new frame");
        auto replacement = dspaa::registerPresentationChannel({}, nullptr);
        replacement->surface(1, 640, 360);
        require(replacement->begin(next) && next.applicationFrameId > second.applicationFrameId,
                "recreated facade reused process application identities");
        input.applicationFrameId = next.applicationFrameId;
        input.generation = 1;
        token = dspaa::queuePresentationInputs(&input);
        require(token != nullptr, "abandoned old commands held new-surface capacity");
        event(2, token);
        observed = replacement->consume();
        require(observed && observed->metadata.applicationFrameId == next.applicationFrameId,
                "replacement mailbox did not receive its own frame");
        replacement->detach();
        std::puts("presentation channel: explicit IDs, render-order publication, replay/cancel, backpressure "
                  "and surface retirement passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
