#include "core/preset-evidence.h"

#include <iostream>
#include <stdexcept>

using dspaa::PresetEvidence;
using dspaa::PresetOrigin;
using dspaa::PresetVerdict;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        PresetEvidence evidence;
        require(evidence.verify('E') == PresetVerdict::MissingEvidence,
                "No log is not successful verification");
        evidence.observe("request_preset_value=5");
        require(evidence.verify('E') == PresetVerdict::MissingEvidence,
                "A request echo is not runtime evidence");
        evidence.observe(
            "[NgxRayReconstruction::FillCreationParams:1733] Info: (DLAA) Using App hint Preset E");
        require(evidence.selected == '\0', "Ray Reconstruction must not satisfy SR validation");
        evidence.observe("[NgxDltss::FillCreationParams:2745] Info: (Quality) Using App hint Preset E");
        require(evidence.selected == '\0', "Another quality mode must not satisfy DLAA validation");
        evidence.observe("[NgxDltss::FillCreationParams::<lambda>::operator ():2745] Info: (DLAA) Using DRS "
                         "Overridden Preset K\n");
        require(evidence.selected == 'K' && evidence.origin == PresetOrigin::Driver,
                "Observe actual driver preset");
        require(evidence.verify('E') == PresetVerdict::Mismatch,
                "Driver replacing requested E with K must fail");
        require(evidence.verify('K') == PresetVerdict::DriverOverride,
                "Matching driver override is not app control");
        evidence = {};
        evidence.observe("[NgxDltss::FillCreationParams:2745] Info: (DLAA) Using App hint Preset E\r\n");
        require(evidence.verify('E') == PresetVerdict::Matched,
                "Explicit application preset observed by runtime");
        evidence.observe("[NGXSecureLoadFeature:1348] Feature dlss override enabled");
        require(evidence.verify('E') == PresetVerdict::DriverOverride,
                "Replaced runtime invalidates app-only gate");
        evidence = {};
        evidence.observe("[NgxDltss::FillCreationParams:2745] Info: (DLAA) Using App hint Preset M");
        require(evidence.verify('M') == PresetVerdict::Matched, "Second-generation preset can be observed");
        evidence.observe("[NgxDltss::FillCreationParams:2745] Info: (DLAA) Using App hint Preset Malformed");
        require(evidence.verify('M') == PresetVerdict::MissingEvidence,
                "Do not misread a malformed model label");
        evidence.observe("[NgxDltss::FillCreationParams:2745] Info: (DLAA) Future unknown diagnostic syntax");
        require(evidence.verify('M') == PresetVerdict::MissingEvidence,
                "Unknown syntax must remain unverified");
        std::cout << "preset evidence behavior passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
