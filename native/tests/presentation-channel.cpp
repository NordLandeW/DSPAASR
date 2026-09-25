#include "present/channel.h"
#include "present/capture.h"
#include <cstdio>
#include <stdexcept>
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::unique_ptr<dspaa::PresentationSubmission> packet(uint64_t id) {
    auto result = std::make_unique<dspaa::PresentationSubmission>(); result->metadata.applicationFrameId = id; return result;
}
void captureCommands() {
    DspAaCaptureCommand input{}; input.size=sizeof(input); input.version=1;
    input.applicationFrameId=1; input.generation=2; input.passId=3;
    input.operation=4; input.scope=static_cast<uint32_t>(dspaa::CaptureScopeKind::DualColor);
    input.flags=10; input.source.epoch=77; input.occlusionSlot=-1;
    input.occlusionDomain.matrix[0]=input.occlusionDomain.matrix[3]=1;
    auto copied=dspaa::copyCaptureCommand(&input,nullptr,nullptr,"Owned whole-output Blit");
    require(copied && copied->scope.fullOverwrite && copied->scope.implicitOutputEpoch==77,
            "Capture command lost its explicit actual-draw output epoch");
    input.source.epoch=88;
    require(copied->scope.implicitOutputEpoch==77,"Capture output metadata was borrowed instead of copied");
    input.flags=8;
    require(!dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr),"Implicit target accepted an unproven partial write");
    input.flags=10; input.scope=static_cast<uint32_t>(dspaa::CaptureScopeKind::FullOnlyUiCoverage);
    require(!dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr),"Implicit target silently reseeded UI geometry");
    input.operation=7; input.flags=8; input.source.epoch=0;
    require(dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr)!=nullptr,"Actual-output remember command rejected");
    input.flags=9;
    require(!dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr),"Remember ambiguously accepted both current RTV and actual scope output");
    input.operation=3; input.flags=8;
    require(!dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr),"Actual-output flag escaped its operation contract");
    input.operation=10; input.flags=10; input.source.epoch=91;
    input.scope=static_cast<uint32_t>(dspaa::CaptureScopeKind::DualColor);
    auto tail=dspaa::copyCaptureCommand(&input,nullptr,nullptr,"Owned camera color tail");
    require(tail && tail->scope.implicitOutputEpoch==91 && tail->scope.fullOverwrite,"Camera tail lost its full-output draw contract");
    input.flags=2;
    require(!dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr),"Camera tail accepted a guessed output identity");
    input.operation=11; input.flags=0; input.source.epoch=0;
    require(dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr)!=nullptr,"Optional camera-tail end rejected");
    input.operation=3; input.flags=21; input.source.epoch=92;
    require(dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr)!=nullptr,"Observed HDR camera-input handoff rejected");
    input.flags=16;
    require(!dspaa::copyCaptureCommand(&input,nullptr,nullptr,nullptr),"HDR handoff accepted an ambiguous current target");
}
}
int main() {
    try {
        captureCommands();
        auto channel = dspaa::registerPresentationChannel({}, nullptr);
        channel->surface(8, 1280, 720);
        DspAaPresentationBegin first{}, second{};
        require(channel->begin(first) && channel->begin(second), "before-input allocation failed");
        require(first.applicationFrameId && second.applicationFrameId > first.applicationFrameId && first.generation == 8,
                "application identities did not preserve order/surface");
        require(!channel->consume(), "BeginFrame fabricated an end-of-frame packet");
        channel->publish(packet(second.applicationFrameId + 1));
        require(!channel->consume(), "unallocated future frame reached Present");
        channel->publish(packet(first.applicationFrameId));
        auto observed = channel->consume();
        require(observed && observed->metadata.applicationFrameId == first.applicationFrameId, "newer simulation replaced an older render token");
        channel->publish(packet(first.applicationFrameId));
        require(!channel->consume(), "same rendered frame was published twice");
        DspAaPresentationConfiguration configuration{sizeof(configuration), 1, 1, 0, 1, 1};
        require(channel->configure(configuration), "fixed analytical FSR selection rejected");
        configuration.mode = 1;
        require(!channel->configure(configuration), "analytical FSR accepted unsupported Dynamic mode");
        DspAaPresentationInputs input{}; input.size = sizeof(input); input.version = 1;
        input.applicationFrameId = second.applicationFrameId; input.generation = 8;
        auto token = dspaa::queuePresentationInputs(&input);
        require(token && !channel->consume(), "main-thread enqueue published pixels before render event");
        auto event = dspaa::presentationRenderEvent(); event(2, token);
        observed = channel->consume(); require(observed && observed->metadata.applicationFrameId == second.applicationFrameId,
                                               "render event lost the exact application token");
        event(2, token); require(!channel->consume(), "render event handle was replayed");
        DspAaPresentationBegin next{}; require(channel->begin(next), "third input allocation failed");
        input.applicationFrameId = next.applicationFrameId;
        token = dspaa::queuePresentationInputs(&input); require(token != nullptr, "cancel packet allocation failed");
        dspaa::cancelPresentationInputs(token); event(2, token); require(!channel->consume(), "canceled command reached presentation");
        for (unsigned i = 0; i < 16; ++i) require(dspaa::queuePresentationInputs(&input) != nullptr, "bounded mailbox filled too early");
        require(!dspaa::queuePresentationInputs(&input), "bounded mailbox failed backpressure");
        channel->detach(); require(!channel->begin(next), "detached surface allocated a new frame");
        auto replacement = dspaa::registerPresentationChannel({}, nullptr);
        replacement->surface(1, 640, 360); require(replacement->begin(next) && next.applicationFrameId > second.applicationFrameId,
                                                "recreated facade reused process application identities");
        input.applicationFrameId = next.applicationFrameId; input.generation = 1;
        token = dspaa::queuePresentationInputs(&input); require(token != nullptr, "abandoned old commands held new-surface capacity");
        event(2, token); observed = replacement->consume();
        require(observed && observed->metadata.applicationFrameId == next.applicationFrameId, "replacement mailbox did not receive its own frame");
        replacement->detach();
        std::puts("presentation channel: explicit IDs, render-order publication, replay/cancel, backpressure and surface retirement passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
