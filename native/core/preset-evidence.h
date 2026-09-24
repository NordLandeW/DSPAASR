#pragma once

#include <string_view>

namespace dspaa {
// Runtime log evidence is deliberately separate from the requested parameter map.
// These diagnostics are useful for validation, not a stable vendor query ABI.
enum class PresetOrigin { Unknown, Application, Driver };
enum class PresetVerdict { MissingEvidence, Mismatch, DriverOverride, Matched };

struct PresetEvidence {
    char selected = '\0';
    PresetOrigin origin = PresetOrigin::Unknown;
    bool driverOverride = false;

    void observe(std::string_view message) noexcept;
    [[nodiscard]] PresetVerdict verify(char requested) const noexcept;
};

[[nodiscard]] const char* verdictName(PresetVerdict verdict) noexcept;
} // namespace dspaa
