#include "ui-proof/effects.h"
#include "ui-proof/effect-pins.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
using namespace dspaa;
using U = EffectUniformRole;
using T = EffectTextureRole;
using V = std::array<float, 4>;
const EffectLayout empty{0, {}, {}};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void requireNear(double actual, double expected, const char* message) { require(std::abs(actual - expected) < 0.000001, message); }
struct Fixture {
    std::map<std::tuple<bool, unsigned, unsigned>, V> constants;
    std::map<unsigned, std::array<unsigned, 2>> textures;
    CaptureSupport output;
    std::string reason;
    void set(bool vertex, unsigned buffer, unsigned offset, V value) { constants[{vertex, buffer, offset}] = value; }
    bool resolve(uint64_t pass, const EffectLayout& vertex, const EffectLayout& pixel,
                 CaptureScopeKind kind = CaptureScopeKind::DualColor, bool full = true) {
        CaptureScope scope; scope.passId = pass; scope.kind = kind; scope.fullOverwrite = full;
        // A declaration is deliberately not evidence for any returned domains.
        scope.support.basis = "untrusted caller declaration";
        scope.support.occlusionResourceSlot = 99;
        return resolveEffectSupport(scope, vertex, pixel,
            [this](bool vs, const EffectUniformPin& pin, V& value) {
                auto found = constants.find({vs, pin.bufferSlot, pin.byteOffset});
                if (found == constants.end()) return false;
                value = found->second; return true;
            },
            [this](const EffectTexturePin& pin, unsigned& width, unsigned& height) {
                auto found = textures.find(pin.resourceSlot);
                if (found == textures.end()) return false;
                width = found->second[0]; height = found->second[1]; return true;
            }, output, reason);
    }
    void ok(uint64_t pass, const EffectLayout& vertex, const EffectLayout& pixel) {
        if (!resolve(pass, vertex, pixel)) throw std::runtime_error(reason);
        require(!output.basis.empty() && reason.empty(), "success lost provenance or retained an old failure");
    }
    void no(uint64_t pass, const EffectLayout& vertex, const EffectLayout& pixel) {
        require(!resolve(pass, vertex, pixel), "unsupported effect was authorized");
        require(!reason.empty() && output.basis.empty() && output.inputs.empty() && output.occlusionResourceSlot == -1,
                "failure retained stale support or supplied no reason");
    }
    const CaptureSamplingInput& input(unsigned slot) const {
        for (const auto& value : output.inputs) if (value.pixelResourceSlot == slot) return value;
        throw std::runtime_error("a color-dependent input was omitted");
    }
};
std::array<double, 2> center(const CaptureSampleDomain& domain, double x, double y, unsigned width, unsigned height) {
    return {domain.matrix[0] * x + domain.matrix[1] * y + domain.bias[0] + domain.offsetTexels[0] / static_cast<double>(width),
            domain.matrix[2] * x + domain.matrix[3] * y + domain.bias[1] + domain.offsetTexels[1] / static_cast<double>(height)};
}
bool contains(const CaptureSamplingInput& input, double x, double y, double sx, double sy, unsigned width, unsigned height) {
    for (const auto& domain : input.domains) {
        auto value = center(domain, x, y, width, height);
        if (std::abs(value[0] - sx) <= domain.radiusTexels[0] / static_cast<double>(width) + 0.000001 &&
            std::abs(value[1] - sy) <= domain.radiusTexels[1] / static_cast<double>(height) + 0.000001) return true;
    }
    return false;
}
void geometric(const CaptureSupport& value, unsigned slot, double x, double y, double sx, double sy,
               unsigned width, unsigned height) {
    require(value.occlusionResourceSlot == static_cast<int>(slot), "geometric A came from the wrong input");
    require(value.occlusionDomain.radiusTexels == std::array<unsigned, 2>{0, 0}, "color kernel grew geometric A");
    auto uv = center(value.occlusionDomain, x, y, width, height);
    requireNear(uv[0], sx, "geometric A x mapping changed"); requireNear(uv[1], sy, "geometric A y mapping changed");
}
void copyAndPrefilterOrder() {
    const EffectLayout vertex{0, {}, {{U::MainSt, 3, 64, 4}}};
    const EffectLayout copy{0, {{T::Main, 7, 2}}, {}};
    const EffectLayout world{0, {{T::Main, 7, 2}}, {{U::MainTexel, 2, 0, 4}, {U::MainSt, 2, 16, 4}, {U::PrefilterOffset, 2, 32, 1}}};
    const EffectLayout cinematic{0, {{T::Main, 7, 2}}, {{U::MainTexel, 2, 0, 2}, {U::PrefilterOffset, 2, 32, 1}}};
    Fixture f; f.textures[7] = {64, 32};
    f.set(true, 3, 64, {0.25f, 0.5f, 0.2f, 0.1f});
    f.set(false, 2, 0, {1.f / 64, 1.f / 32, 64, 32});
    f.set(false, 2, 16, {0.5f, 0.25f, 0.125f, 0.375f});
    f.set(false, 2, 32, {0.5f, 0, 0, 0});
    f.ok(0x1000, vertex, copy);
    geometric(f.output, 7, 0.25, 0.5, 0.2625, 0.35, 64, 32);
    require(f.input(7).pixelSamplerSlot == 2, "resolver guessed a sampler instead of using its pin");
    f.ok(0x1100, vertex, world);
    require(contains(f.input(7), 0.25, 0.5, 0.25390625, 0.50390625, 64, 32), "World prefilter did not offset before PS ST");
    require(!contains(f.input(7), 0.25, 0.5, 0.2578125, 0.515625, 64, 32), "World prefilter accepted the wrong ST order");
    require(f.output.occlusionResourceSlot == -1, "auxiliary World Bloom acquired physical A");
    f.ok(0x1200, vertex, cinematic);
    require(contains(f.input(7), 0.25, 0.5, 0.2703125, 0.365625, 64, 32), "Cinematic prefilter did not offset after VS ST");
    auto anti = world; anti.features = EffectAntiFlicker;
    f.ok(0x1100, vertex, anti);
    require(contains(f.input(7), 0.25, 0.5, 0.26171875, 0.50390625, 64, 32), "anti-flicker neighbor was not transported through ST");
}
void bloomBaseFlipsAndTinySource() {
    const EffectLayout vertex{0, {}, {{U::MainSt, 3, 64, 4}, {U::BaseTexel, 3, 80, 2}, {U::BaseSt, 3, 96, 4}}};
    const EffectLayout pixel{0, {{T::Main, 7, 2}, {T::Base, 9, 3}}, {{U::MainTexel, 2, 0, 2}, {U::SampleScale, 2, 16, 1}}};
    Fixture f; f.textures[7] = {1, 1}; f.textures[9] = {128, 64};
    f.set(true, 3, 64, {0.5f, 0.25f, 0.1f, 0.2f});
    f.set(true, 3, 80, {1.f / 128, -1.f / 64, 0, 0});
    f.set(true, 3, 96, {0.25f, 0.5f, 0.2f, 0.1f});
    f.set(false, 2, 0, {1, 1, 0, 0}); f.set(false, 2, 16, {1.5f, 0, 0, 0});
    f.ok(0x1103, vertex, pixel);
    require(contains(f.input(9), 0.2, 0.4, 0.2, 0.7, 128, 64), "World base must flip AFTER Main ST");
    require(!contains(f.input(9), 0.2, 0.4, 0.2, 0.35, 128, 64), "World base used a pre-ST flip");
    require(contains(f.input(7), 0.2, 0.4, 1.7, 1.8, 1, 1), "1px Bloom lost a real sample outside its subresource");
    require(f.output.occlusionResourceSlot == -1, "World upsample grew A");
    f.ok(0x1207, vertex, pixel);
    require(contains(f.input(7), 0.2, 0.4, 0.95, 1.05, 1, 1), "low-quality Cinematic kernel lost its half-scale corner");
    require(!contains(f.input(7), 0.2, 0.4, 1.7, 1.8, 1, 1), "low-quality kernel was incorrectly doubled");
    geometric(f.output, 9, 0.2, 0.4, 0.25, 0.6, 128, 64);
    f.ok(0x1208, vertex, pixel);
    require(contains(f.input(7), 0.2, 0.4, 1.7, 1.8, 1, 1), "high-quality Cinematic kernel was halved");
    geometric(f.output, 9, 0.2, 0.4, 0.25, 0.6, 128, 64);
}
void dofAndFxaaSourceTexels() {
    const EffectLayout dof{0, {{T::Main, 7, 2}}, {{U::MainTexel, 2, 0, 4}, {U::MaxCoc, 2, 16, 1}, {U::ReciprocalAspect, 2, 32, 1}}};
    Fixture f; f.textures[7] = {80, 40};
    f.set(false, 2, 0, {1.f / 80, -1.f / 40, 80, 40});
    f.set(false, 2, 16, {0.0625f, 0, 0, 0}); f.set(false, 2, 32, {0.5f, 0, 0, 0});
    f.ok(0x1303, empty, dof);
    require(f.input(7).domains.front().radiusTexels == std::array<unsigned, 2>{3, 3}, "DoF radius used output dimensions or rounded inward");
    require(f.output.occlusionResourceSlot == -1, "DoF intermediate became physical A");
    f.textures[7] = {40, 20}; f.ok(0x1306, empty, dof);
    require(f.input(7).domains.front().radiusTexels == std::array<unsigned, 2>{2, 2}, "DoF ignored the actual SRV mip dimensions");
    f.textures[7] = {80, 40}; f.ok(0x1302, empty, dof);
    require(contains(f.input(7), 0.25, 0.125, 0.25625, 0.1125, 80, 40), "DoF prefilter wrongly flipped its color UV");
    require(!contains(f.input(7), 0.25, 0.125, 0.25625, 0.8875, 80, 40), "DoF confused shared CoC UV with color UV");
    f.set(false, 2, 16, {std::numeric_limits<float>::max(), 0, 0, 0});
    f.set(false, 2, 32, {std::numeric_limits<float>::max(), 0, 0, 0}); f.no(0x1303, empty, dof);
    const EffectLayout fxaa{0, {{T::Main, 7, 2}}, {{U::MainTexel, 2, 0, 4}, {U::MainSt, 2, 48, 4}, {U::FxaaQuality, 2, 64, 3}}};
    f.textures[7] = {128, 64}; f.set(false, 2, 0, {1.f / 128, 1.f / 64, 128, 64});
    f.set(false, 2, 48, {0.5f, 0.5f, 0.25f, 0.25f}); f.set(false, 2, 64, {0.75f, 0.166f, 0.0833f, 0});
    f.ok(0x1500, empty, fxaa);
    require(f.input(7).domains.front().radiusTexels == std::array<unsigned, 2>{19, 19}, "FXAA search support lost its farthest actual read");
    geometric(f.output, 7, 0.25, 0.5, 0.375, 0.5, 128, 64);
    f.set(false, 2, 64, {32, 0.166f, 0.0833f, 0}); f.no(0x1500, empty, fxaa);
    f.set(false, 2, 64, {-1, 0.166f, 0.0833f, 0}); f.no(0x1500, empty, fxaa);
    f.set(false, 2, 64, {std::numeric_limits<float>::max(), 0.166f, 0.0833f, 0}); f.no(0x1500, empty, fxaa);
    f.textures[7] = {8, 8}; f.set(false, 2, 0, {0.125f, 0.125f, 8, 8});
    f.set(false, 2, 48, {1, 1, 0, 0}); f.set(false, 2, 64, {0.75f, 0.166f, 0.0833f, 0});
    f.ok(0x1500, empty, fxaa);
    require(f.input(7).domains.front().radiusTexels == std::array<unsigned, 2>{8, 8}, "in-source oversized kernel did not preserve all source texels");
    f.set(false, 2, 48, {1, 1, -2, 0}); f.no(0x1500, empty, fxaa); // Border reach may not be narrowed outside the source.
}
void uberStageSeparationAndGeometricA() {
    const EffectLayout vertex{0, {}, {{U::MainSt, 3, 64, 4}, {U::MainTexel, 3, 80, 4}}};
    const EffectLayout pixel{EffectChromatic | EffectDof | EffectBloom, {{T::Main, 11, 4}, {T::Dof, 12, 5}, {T::Bloom, 13, 7}},
        {{U::MainTexel, 2, 16, 4}, {U::MainSt, 2, 32, 4}, {U::ChromaticAmount, 2, 48, 1}, {U::BloomTexel, 2, 64, 4}, {U::BloomSettings, 2, 80, 2}}};
    const EffectLayout ordinary{EffectDof | EffectBloom, {{T::Main, 11, 4}, {T::Dof, 12, 5}, {T::Bloom, 13, 7}},
        {{U::MainTexel, 2, 16, 4}, {U::BloomTexel, 2, 64, 4}, {U::BloomSettings, 2, 80, 2}}};
    Fixture f; f.textures[11] = {256, 128}; f.textures[12] = {128, 64}; f.textures[13] = {32, 16};
    f.set(true, 3, 64, {0.5f, 0.25f, 0.1f, 0.2f}); f.set(true, 3, 80, {1.f / 256, -1.f / 128, 256, 128});
    f.set(false, 2, 16, {1.f / 256, 1.f / 128, 256, 128}); f.set(false, 2, 32, {0.25f, 0.5f, 0.2f, 0.1f});
    f.set(false, 2, 48, {0.125f, 0, 0, 0}); f.set(false, 2, 64, {1.f / 32, 1.f / 16, 32, 16}); f.set(false, 2, 80, {1.5f, 1, 0, 0});
    f.ok(0x1400, vertex, pixel);
    require(f.output.inputs.size() == 3 && f.input(13).pixelSamplerSlot == 7, "Uber dropped a color input or guessed its keyword-dependent sampler");
    require(f.input(11).domains.front().radiusTexels == std::array<unsigned, 2>{16, 16}, "chromatic support missed its non-affine bound");
    require(f.input(12).domains.front().radiusTexels == std::array<unsigned, 2>{8, 8}, "DoF chromatic support reused the Main source size");
    auto dofUv = center(f.input(12).domains.front(), 0.2, 0.4, 128, 64);
    requireNear(dofUv[0], 0.25, "chromatic DoF lost PS ST"); requireNear(dofUv[1], 0.3, "chromatic DoF incorrectly used the VS flip sign");
    require(contains(f.input(13), 0.2, 0.4, 0.246875, 0.44375, 32, 16), "Bloom did not use independent VS ST/flip and its own texel size");
    geometric(f.output, 11, 0.2, 0.4, 0.25, 0.3, 256, 128);
    f.ok(0x1400, vertex, ordinary);
    geometric(f.output, 11, 0.2, 0.4, 0.2, 0.3, 256, 128);
    dofUv = center(f.input(12).domains.front(), 0.2, 0.4, 128, 64);
    requireNear(dofUv[1], 0.35, "ordinary DoF must flip BEFORE VS ST");
    auto debug = ordinary; debug.features |= EffectCocView; f.no(0x1400, vertex, debug);
}
void shaftsAndFailClosed() {
    const EffectLayout radialVertex{0, {}, {{U::BlurRadius, 3, 64, 4}, {U::SunPosition, 3, 80, 4}}};
    const EffectLayout main{0, {{T::Main, 7, 2}}, {}};
    Fixture f; f.textures[7] = {64, 32};
    f.set(true, 3, 64, {0.125f, 0.25f, 0, 0}); f.set(true, 3, 80, {2, -1, 0, 0});
    f.ok(0x1601, radialVertex, main);
    require(contains(f.input(7), 0.25, 0.75, 0.25, 0.75, 64, 32), "radial kernel lost its original center");
    require(contains(f.input(7), 0.25, 0.75, 1.34375, -1.4375, 64, 32), "radial kernel lost offscreen sun or independent axis radii");
    require(!contains(f.input(7), 0.25, 0.75, -10, -10, 64, 32), "radial support became unbounded");
    require(f.output.occlusionResourceSlot == -1, "radial auxiliary result acquired A");
    const EffectLayout finalVertex{0, {}, {{U::MainTexel, 3, 80, 4}}};
    const EffectLayout finalPixel{0, {{T::Main, 7, 2}, {T::Shafts, 9, 5}}, {}};
    f.textures[9] = {32, 16}; f.set(true, 3, 80, {1.f / 64, -1.f / 32, 64, 32});
    f.ok(0x1600, finalVertex, finalPixel);
    require(contains(f.input(9), 0.25, 0.75, 0.25, 0.25, 32, 16), "final shafts input lost its own flip");
    geometric(f.output, 7, 0.25, 0.75, 0.25, 0.75, 64, 32);
    const EffectLayout copyVertex{0, {}, {{U::MainSt, 3, 96, 4}}};
    f.no(0x1000, copyVertex, main); // Missing actual CB data, not an identity fallback.
    f.set(true, 3, 96, {1, 1, 0, 0}); f.ok(0x1000, copyVertex, main);
    f.no(0x1000, empty, main); // Missing pin must not read an undeclared uniform.
    f.no(0x1300, copyVertex, main); f.no(0x1800, copyVertex, main); f.no(0xffff, copyVertex, main);
    f.textures[7] = {0, 32}; f.no(0x1000, copyVertex, main); f.textures[7] = {64, 32};
    f.set(true, 3, 96, {1, 1, std::numeric_limits<float>::infinity(), 0}); f.no(0x1000, copyVertex, main);
    f.set(true, 3, 96, {1, 1, std::numeric_limits<float>::quiet_NaN(), 0}); f.no(0x1000, copyVertex, main);
    f.set(true, 3, 96, {1, std::numeric_limits<float>::denorm_min(), 0, 0}); f.no(0x1000, copyVertex, main);
    f.set(true, 3, 96, {std::numeric_limits<float>::max(), 1, 0, 0}); f.no(0x1000, copyVertex, main);
    f.set(true, 3, 96, {1, 1, 0, 0});
    const EffectLayout extra{0, {{T::Main, 7, 2}, {T::Base, 9, 5}}, {}};
    f.no(0x1000, copyVertex, extra);
    require(!f.resolve(0x1000, copyVertex, main, CaptureScopeKind::SharedPreparation), "shared scope authorized color replay");
    require(!f.resolve(0x1000, copyVertex, main, CaptureScopeKind::DualColor, false), "partial effect discarded prior support");
    require(f.resolve(0x1700, empty, empty, CaptureScopeKind::PartialWrite, false), "constant border lost its partial-write contract");
    require(f.output.inputs.empty() && f.output.occlusionResourceSlot == -1, "auxiliary border fabricated inputs or geometric opacity");
    require(!f.resolve(0x1700, empty, empty, CaptureScopeKind::PartialWrite, true), "border declared an unsafe whole-subresource overwrite");
}
} // namespace
int main() {
    try {
        copyAndPrefilterOrder(); bloomBaseFlipsAndTinySource(); dofAndFxaaSourceTexels();
        uberStageSeparationAndGeometricA(); shaftsAndFailClosed();
        std::puts("effect support: actual stage/CB/SRV inputs, ST/flip order, source-texel kernels, geometric A and fail-closed behavior passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
