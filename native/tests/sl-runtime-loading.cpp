#include "sl/internal.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace {
namespace fs = std::filesystem;
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct FixtureDirectory {
    fs::path path;
    ~FixtureDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void reachesExportCheck(const fs::path& directory) {
    dspaa::SlRuntimeCreateInfo info;
    info.runtimeDirectory = directory;
    std::string failure;
    try {
        dspaa::SlRuntime runtime(info);
    } catch (const std::exception& error) {
        failure = error.what();
    }
    // The unsigned project fixture is a real loadable DLL, but deliberately
    // exports no Streamline API. Reaching this error proves that file contents
    // were not rejected by a release pin or custom signature check. No vendor
    // code, SDK initialization, window or graphics device is involved.
    require(failure == "Missing Streamline export: slInit",
            "Replacement DLL was not rejected at the required-export boundary");
    require(!GetModuleHandleW(L"sl.interposer.dll"), "Failed runtime retained its inactive fixture module");
}
// Exercise the real runtime's ownership/gating with only its external SDK calls
// replaced. No DLL, driver state, window or GPU work participates in this test.
struct ReflexFixture {
    struct Token final : sl::FrameToken {
        uint32_t id;
        explicit Token(uint32_t value) : id(value) {}
        operator uint32_t() const override {
            return id;
        }
    };
    struct Policy {
        sl::ReflexMode mode;
        uint32_t limit;
    };
    inline static ReflexFixture* current = nullptr;
    std::vector<Policy> policies;
    std::vector<std::unique_ptr<Token>> tokens;
    unsigned sleeps = 0, markers = 0, unloads = 0;
    bool failNeutral = false;
    dspaa::detail::SlRuntimeState state{dspaa::SlRuntimeCreateInfo{}};
    ReflexFixture() {
        current = this;
        state.attached = state.reflexSupported = true;
        state.api.reflexOptions = [](const sl::ReflexOptions& options) {
            current->policies.push_back({options.mode, options.frameLimitUs});
            return current->failNeutral && options.mode == sl::ReflexMode::eOff && !options.frameLimitUs
                       ? sl::Result::eErrorInvalidState
                       : sl::Result::eOk;
        };
        state.api.load = [](sl::Feature feature, bool enabled) {
            require(feature == sl::kFeatureDLSS_G && !enabled, "Unexpected feature ownership change");
            require(!current->policies.empty() && current->policies.back().mode == sl::ReflexMode::eOff &&
                        current->policies.back().limit == 0,
                    "SL hooks were released before neutralizing Reflex/limiter");
            ++current->unloads;
            return sl::Result::eOk;
        };
        state.api.newFrame = [](sl::FrameToken*& output, const uint32_t* id) {
            auto token = std::make_unique<Token>(*id);
            output = token.get();
            current->tokens.push_back(std::move(token));
            return sl::Result::eOk;
        };
        state.api.sleep = [](const sl::FrameToken&) {
            ++current->sleeps;
            return sl::Result::eOk;
        };
        state.api.marker = [](sl::PCLMarker, const sl::FrameToken&) {
            ++current->markers;
            return sl::Result::eOk;
        };
    }
    ~ReflexFixture() {
        current = nullptr;
    }
    bool policy(sl::ReflexMode mode, uint32_t limit) const {
        return !policies.empty() && policies.back().mode == mode && policies.back().limit == limit;
    }
};
void reflexOwnership() {
    ReflexFixture fixture;
    auto& state = fixture.state;
    {
        std::lock_guard lock(state.sdkMutex);
        state.applyReflex(dspaa::SlReflexMode::Off, 0);
    }
    require(fixture.policy(sl::ReflexMode::eOff, 0) && !state.reflexConfigured,
            "Neutral initialization opened the frame configuration gate");
    require(!state.setReflex(dspaa::SlReflexMode::On, 5000) && fixture.policies.size() == 1,
            "Inactive runtime applied an unowned driver policy");
    state.presenterOwner = &fixture;
    state.active = true; // The presenter has created its actual chain.
    require(!state.begin(1) && fixture.tokens.empty(), "Unconfigured owner allocated a frame token");
    require(state.setReflex(dspaa::SlReflexMode::On, 0) && state.begin(1) && fixture.sleeps == 1,
            "First configured SL frame failed to call Sleep");
    require(state.mark(1, dspaa::SlMarker::SimulationStart), "Configured SL marker rejected");
    require(state.setReflex(dspaa::SlReflexMode::OnWithBoost, 1000) &&
                fixture.policy(sl::ReflexMode::eLowLatencyWithBoost, 1000) && state.begin(2),
            "SL-owned Boost/limiter policy was not applied");
    state.deactivate(&fixture);
    require(fixture.policy(sl::ReflexMode::eOff, 0) && fixture.unloads == 1 && !state.presenterOwner &&
                !state.active && !state.reflexConfigured && state.reflexMode == dspaa::SlReflexMode::Off,
            "Retired owner retained Reflex Boost/limiter or admitted frames");
    const auto calls = fixture.policies.size();
    const auto sleeps = fixture.sleeps, markers = fixture.markers;
    require(!state.begin(3) && !state.mark(2, dspaa::SlMarker::SimulationEnd) &&
                !state.setReflex(dspaa::SlReflexMode::On, 5000) && fixture.policies.size() == calls &&
                fixture.sleeps == sleeps && fixture.markers == markers,
            "Inactive runtime kept issuing Reflex calls");
    state.presenterOwner = &fixture;
    state.active = true;
    require(!state.begin(3), "Reentered owner inherited the old configuration gate");
    require(state.setReflex(dspaa::SlReflexMode::Off, 5000) && state.begin(3) &&
                state.mark(3, dspaa::SlMarker::SimulationStart) && fixture.sleeps == sleeps + 1 &&
                fixture.policy(sl::ReflexMode::eOff, 5000),
            "An active owner's Reflex Off mode stopped Sleep or lost its independent limiter");
    require(state.setReflex(dspaa::SlReflexMode::OnWithBoost, 1000), "Reentered SL policy rejected");
    fixture.failNeutral = true;
    bool rejected = false;
    try {
        state.deactivate(&fixture);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected && state.quarantined && state.presenterOwner == &fixture && !state.active &&
                fixture.unloads == 1 && state.stop() == dspaa::PresentRetirement::Quarantined,
            "Failed Reflex neutralization released ownership instead of quarantining");
    std::puts("Reflex ownership: neutral attach policy, configured first token, Boost/limiter release, "
              "reentry, active-Off Sleep and failed-neutralization quarantine passed (SDK stubs).");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3, "Pass the harmless fixture DLL and test output directory");
        reflexOwnership();
        const auto directory =
            fs::path(argv[2]) / (L"sl-runtime-loading-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(directory.parent_path());
        require(fs::create_directory(directory), "Test directory already exists; refusing to reuse it");
        FixtureDirectory cleanup{directory};
        for (const auto* name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll",
                                 L"sl.pcl.dll", L"nvngx_dlssg.dll"})
            fs::copy_file(argv[1], directory / name);
        reachesExportCheck(directory);
        // The same external filename can contain a different revision between
        // launches, and failed initialization must have released its file locks.
        {
            std::ofstream revision(directory / L"nvngx_dlssg.dll", std::ios::binary | std::ios::app);
            revision << "fixture revision";
            require(revision.good(), "Failed runtime retained a file lock");
        }
        reachesExportCheck(directory);
        require(fs::remove_all(directory) == 7, "Failed runtime did not release all fixture files");
        std::puts("External runtime replacements reach required-export validation without a custom content "
                  "gate; failed owners retire cleanly.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
