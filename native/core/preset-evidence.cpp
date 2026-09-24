#include "preset-evidence.h"

namespace dspaa {
void PresetEvidence::observe(std::string_view message) noexcept {
    if (message.find("Using global override for DLSS-SR preset") != std::string_view::npos ||
        message.find("Feature dlss override enabled") != std::string_view::npos) {
        driverOverride = true;
    }
    // Exclude RR and other quality modes, even when their messages contain DLAA/preset letters.
    if (message.find("NgxDltss::FillCreationParams") == std::string_view::npos)
        return;
    const auto mode = message.find("(DLAA) ");
    if (mode == std::string_view::npos)
        return;
    message.remove_prefix(mode + 7);
    constexpr std::string_view application = "Using App hint Preset ";
    constexpr std::string_view driver = "Using DRS Overridden Preset ";
    selected = '\0';
    origin = PresetOrigin::Unknown;
    if (message.starts_with(application)) {
        message.remove_prefix(application.size());
        origin = PresetOrigin::Application;
    } else if (message.starts_with(driver)) {
        message.remove_prefix(driver.size());
        origin = PresetOrigin::Driver;
        driverOverride = true;
    } else {
        return;
    }
    if (message.empty() || message.front() < 'A' || message.front() > 'Z' ||
        (message.size() > 1 && message[1] != '\r' && message[1] != '\n' && message[1] != ' ')) {
        origin = PresetOrigin::Unknown;
        return;
    }
    selected = message.front();
}

PresetVerdict PresetEvidence::verify(char requested) const noexcept {
    if (selected == '\0' || origin == PresetOrigin::Unknown)
        return PresetVerdict::MissingEvidence;
    if (selected != requested)
        return PresetVerdict::Mismatch;
    if (driverOverride || origin == PresetOrigin::Driver)
        return PresetVerdict::DriverOverride;
    return PresetVerdict::Matched;
}

const char* verdictName(PresetVerdict verdict) noexcept {
    switch (verdict) {
    case PresetVerdict::MissingEvidence:
        return "unverified";
    case PresetVerdict::Mismatch:
        return "mismatch";
    case PresetVerdict::DriverOverride:
        return "driver-controlled";
    case PresetVerdict::Matched:
        return "matched-runtime-log";
    }
    return "unverified";
}
} // namespace dspaa
