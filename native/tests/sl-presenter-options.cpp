// Compile the production implementation into this fixture so SDK calls can be
// replaced without loading a vendor DLL or initializing a window/graphics device.
#include "sl/presenter.cpp"
#include <cstdio>

namespace dspaa {
struct SlPresenterTests {
    struct Submission {
        sl::DLSSGMode mode;
        unsigned frames;
        float target;
    };
    inline static SlPresenterTests* current = nullptr;
    std::vector<Submission> submissions;
    bool failOptions = false;
    sl::DLSSGStatus sdkStatus = sl::DLSSGStatus::eOk;
    bool unloaded = false;
    std::shared_ptr<detail::SlRuntimeState> runtime =
        std::make_shared<detail::SlRuntimeState>(SlRuntimeCreateInfo{});
    SlPresenter::Impl presenter{SlPresenterCreateInfo{}, runtime};

    SlPresenterTests() {
        current = this;
        runtime->api.dlssOptions = [](const sl::ViewportHandle&, const sl::DLSSGOptions& value) {
            current->submissions.push_back(
                {value.mode, value.numFramesToGenerate, value.dynamicTargetFrameRate});
            return current->failOptions ? sl::Result::eErrorInvalidState : sl::Result::eOk;
        };
        runtime->api.dlssState = [](const sl::ViewportHandle&, sl::DLSSGState& state,
                                    const sl::DLSSGOptions*) {
            state.status = current->sdkStatus;
            return sl::Result::eOk;
        };
        runtime->api.reflexOptions = [](const sl::ReflexOptions&) { return sl::Result::eOk; };
        runtime->api.load = [](sl::Feature feature, bool enabled) {
            require(feature == sl::kFeatureDLSS_G && !enabled, "Unexpected feature transition");
            require(!current->submissions.empty() && current->submissions.back().mode == sl::DLSSGMode::eOff,
                    "Presenter released SDK ownership before submitting Off");
            current->unloaded = true;
            return sl::Result::eOk;
        };
    }
    ~SlPresenterTests() {
        current = nullptr;
    }
    static void require(bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    }
    void reportingDoesNotConfigure(SlGenerationMode mode) {
        SlGenerationRequest request{mode, 1, 0};
        presenter.options(mode, request);
        require(submissions.size() == 1 && submissions.back().mode != sl::DLSSGMode::eOff,
                "Fixture did not submit SDK generation mode");
        // An observed failure is not an SDK configuration transaction.
        presenter.observation.active = SlGenerationMode::Off;
        presenter.pause();
        require(submissions.size() == 2 && submissions.back().mode == sl::DLSSGMode::eOff,
                "Reporting Off suppressed the required SDK Off transition");
        require(presenter.forceReset, "Pause retained temporal history");
        presenter.pause();
        require(submissions.size() == 2, "Repeated pause resubmitted identical SDK options");
    }
    void finish(bool generate, sl::DLSSGStatus status, HRESULT result = S_OK) {
        sdkStatus = status;
        const auto state = presenter.query();
        const ContentView view{{0, 0, 1280, 720}, 1280.0f / 720};
        presenter.finishPresent(generate, result, state, view);
    }
    void recovery(SlGenerationMode mode) {
        const SlGenerationRequest request{mode, 1, 0};
        const auto sdkMode = mode == SlGenerationMode::Fixed ? sl::DLSSGMode::eOn : sl::DLSSGMode::eDynamic;
        ContentView unused;
        require(!presenter.eligible(PresentationFrame{}, PresentArguments{}, request, {}, unused),
                "Missing frame inputs were accepted before recovery");
        const auto inputReason = presenter.observation.reason;
        presenter.options(mode, request);
        finish(true, sl::DLSSGStatus::eOk);
        require(presenter.observation.active == mode && !presenter.forceReset,
                "Healthy generation was not reported active");
        // Even a cache hit must not leave an old observed failure latched forever.
        presenter.observation.active = SlGenerationMode::Off;
        presenter.options(mode, request);
        finish(true, sl::DLSSGStatus::eOk);
        require(submissions.size() == 1 && presenter.observation.active == mode,
                "Healthy cached generation did not restore the observed active mode");

        const auto failure =
            sl::DLSSGStatus::eFailCommonConstantsInvalid | sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime;
        finish(true, failure);
        require(submissions.size() == 2 && submissions.back().mode == sl::DLSSGMode::eOff &&
                    presenter.observation.active == SlGenerationMode::Off && presenter.forceReset &&
                    presenter.observation.sdkStatus == static_cast<uint32_t>(failure),
                "Runtime failure did not submit Off, report the failure and reset temporal history");
        require(!presenter.eligible(PresentationFrame{}, PresentArguments{}, request, {}, unused) &&
                    presenter.observation.reason != inputReason,
                "The pending SDK error did not gate generation before frame-input validation");
        const auto recoveryReason = presenter.observation.reason;
        presenter.pause();
        finish(false, sl::DLSSGStatus::eOk, DXGI_ERROR_WAS_STILL_DRAWING);
        require(!presenter.eligible(PresentationFrame{}, PresentArguments{}, request, {}, unused) &&
                    presenter.observation.reason == recoveryReason,
                "An unsuccessful disabled Present prematurely permitted generation retry");
        // The SDK can retain the previous error while disabled. A successful Off
        // Present must allow the next frame to be validated again in that case.
        finish(false, failure);
        require(submissions.size() == 2 && presenter.observation.active == SlGenerationMode::Off &&
                    presenter.forceReset,
                "Disabled recovery frames resubmitted SDK options or accepted temporal history");
        require(!presenter.eligible(PresentationFrame{}, PresentArguments{}, request, {}, unused) &&
                    presenter.observation.reason == inputReason,
                "Completed disabled Present did not return to ordinary frame-input validation");
        presenter.options(mode, request);
        finish(true, sl::DLSSGStatus::eOk);
        require(submissions.size() == 3 && submissions.back().mode == sdkMode &&
                    presenter.observation.active == mode && !presenter.forceReset,
                "Generation did not re-enable after completed disabled-frame recovery");
        presenter.options(mode, request);
        require(submissions.size() == 3, "Steady generation repeatedly resubmitted SDK options");
    }
    void failedSubmission() {
        presenter.options(SlGenerationMode::Fixed);
        failOptions = true;
        bool failed = false;
        try {
            finish(true, sl::DLSSGStatus::eFailCommonConstantsInvalid);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed && submissions.size() == 2 && presenter.forceReset &&
                    presenter.observation.active == SlGenerationMode::Off,
                "Failed SDK Off submission was swallowed or reported active");
        failOptions = false;
        presenter.pause();
        require(submissions.size() == 3 && submissions.back().mode == sl::DLSSGMode::eOff,
                "Failed SDK Off submission incorrectly committed the configuration cache");
        presenter.pause();
        require(submissions.size() == 3, "Successful Off retry was not cached");
        failOptions = true;
        failed = false;
        try {
            presenter.options(SlGenerationMode::Dynamic);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed && presenter.observation.active == SlGenerationMode::Off,
                "Failed enable submission was reported as active");
        failOptions = false;
        presenter.options(SlGenerationMode::Dynamic);
        require(submissions.size() == 5 && submissions.back().mode == sl::DLSSGMode::eDynamic,
                "Failed enable submission incorrectly committed the configuration cache");
    }
    void changedParameters() {
        SlGenerationRequest request{SlGenerationMode::Fixed, 1, 0};
        presenter.options(request.mode, request);
        request.generatedFrames = 3;
        presenter.options(request.mode, request);
        require(submissions.size() == 2 && submissions.back().frames == 3,
                "Fixed multiplier change was dropped by the cache");
        presenter.options(request.mode, request);
        require(submissions.size() == 2, "Unchanged fixed multiplier was resubmitted");
        request.mode = SlGenerationMode::Dynamic;
        request.dynamicTargetFrameRate = 120;
        presenter.options(request.mode, request);
        request.dynamicTargetFrameRate = 144;
        presenter.options(request.mode, request);
        require(submissions.size() == 4 && submissions.back().mode == sl::DLSSGMode::eDynamic &&
                    submissions.back().target == 144,
                "Dynamic target change was dropped by the cache");
        request.generatedFrames = 2; // Not an effective Dynamic option.
        presenter.options(request.mode, request);
        require(submissions.size() == 4, "Irrelevant fixed multiplier changed Dynamic options");
    }
    void retirement(bool fail) {
        presenter.options(SlGenerationMode::Fixed);
        presenter.observation.active = SlGenerationMode::Off;
        runtime->presenterOwner = &presenter;
        failOptions = fail;
        const auto result = presenter.stop();
        require(submissions.size() == 2 && submissions.back().mode == sl::DLSSGMode::eOff,
                "Retirement skipped SDK Off because observation already reported Off");
        if (fail) {
            require(result == PresentRetirement::Quarantined && runtime->quarantined && !unloaded &&
                        runtime->presenterOwner == &presenter,
                    "Failed SDK Off released ownership instead of quarantining");
        } else {
            require(result == PresentRetirement::Drained && unloaded && !runtime->presenterOwner,
                    "Successful retirement did not release SDK ownership");
            require(presenter.stop() == PresentRetirement::Drained && submissions.size() == 2,
                    "Repeated retirement submitted more SDK options");
        }
    }
    static void run() {
        for (const auto mode : {SlGenerationMode::Fixed, SlGenerationMode::Dynamic}) {
            SlPresenterTests fixture;
            fixture.reportingDoesNotConfigure(mode);
        }
        for (const auto mode : {SlGenerationMode::Fixed, SlGenerationMode::Dynamic}) {
            SlPresenterTests fixture;
            fixture.recovery(mode);
        }
        {
            SlPresenterTests fixture;
            fixture.failedSubmission();
        }
        {
            SlPresenterTests fixture;
            fixture.changedParameters();
        }
        for (const bool fail : {false, true}) {
            SlPresenterTests fixture;
            fixture.retirement(fail);
        }
        std::puts("SL option-cache, runtime-error recovery, retry and retirement checks passed "
                  "(production methods, SDK stubs).");
    }
};
} // namespace dspaa

int main() {
    try {
        dspaa::SlPresenterTests::run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
