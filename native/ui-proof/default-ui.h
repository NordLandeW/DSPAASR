#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dspaa::proof {
// Numerical admission for the pinned non-stereo UI/Default shader family.
// The caller separately proves the actual VS/PS, Color32 and UNORM sampling.
// gammaWord is the untouched CB word carried through a float read, NOT a float
// boolean: shader movc takes its true branch when ANY source bit is set.
inline const char* defaultUiFailure(const std::array<float, 4>& color, const std::array<float, 4>& sampleAdd,
                                    float gammaWord) noexcept {
    if (std::bit_cast<std::uint32_t>(gammaWord) != 0)
        return "Default UI gamma branch has no nonnegative RGB proof";
    if (!std::isfinite(color[3]) || color[3] < 0 || color[3] > 1)
        return "Default UI material alpha is outside [0,1]";
    if (sampleAdd[3] != 0)
        return "Default UI alpha sample-add is not zero";
    const double maximum = std::numeric_limits<float>::max();
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(color[channel]) || color[channel] < 0 || !std::isfinite(sampleAdd[channel]) ||
            sampleAdd[channel] < 0)
            return "Default UI RGB tint/sample-add is not finite and nonnegative";
        const double sum = 1.0 + static_cast<double>(sampleAdd[channel]);
        if (sum > maximum)
            return "Default UI sampled RGB addition can overflow";
        // Bound the rounded FP32 addition BEFORE bounding the subsequent RGB
        // multiply. Using the exact real sum can miss a rounding-induced Inf.
        // Ceiling to a representable float is conservative for GPU round-nearest
        // and does not assume the host's current floating-point rounding mode.
        float sampledMaximum = static_cast<float>(sum);
        if (static_cast<double>(sampledMaximum) < sum)
            sampledMaximum = std::nextafter(sampledMaximum, std::numeric_limits<float>::infinity());
        if (!std::isfinite(sampledMaximum) || static_cast<double>(color[channel]) * sampledMaximum > maximum)
            return "Default UI RGB product can overflow before premultiplication";
    }
    // Unit material/Color32 alpha remains unit after the shader's 1/255 rounding.
    // The optional rect multiplier is a product of two _sat values (NaN -> 0);
    // alpha clip only discards. Multiplying finite RGB by this alpha is safe.
    return nullptr;
}
} // namespace dspaa::proof
