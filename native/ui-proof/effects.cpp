#include "effects.h"
#include "effect-pins.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dspaa {
namespace {
using U = EffectUniformRole;
using T = EffectTextureRole;
using V = std::array<float, 4>;
// Same integer-addressing envelope as capture::Gpu's support transport.
constexpr double addressLimit = std::numeric_limits<int32_t>::max() / 4;
constexpr unsigned uniformCount = static_cast<unsigned>(U::FxaaQuality) + 1;
constexpr unsigned textureCount = static_cast<unsigned>(T::Shafts) + 1;
[[noreturn]] void reject(const char* reason) { throw std::invalid_argument(reason); }
double arithmetic(double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        reject("Effect sampling arithmetic is not finite FP32");
    return value;
}
float scalar(double value) { return static_cast<float>(arithmetic(value)); }
struct Map {
    double x = 1, y = 1, bx = 0, by = 0;
};
Map st(const V& value) { return {value[0], value[1], value[2], value[3]}; }
Map flipBefore(Map value) {
    value.by = arithmetic(value.by + value.y); value.y = -value.y; return value;
}
Map flipAfter(Map value) {
    value.by = arithmetic(1 - value.by); value.y = -value.y; return value;
}
struct Source {
    const EffectTexturePin* pin = nullptr;
    unsigned width = 0, height = 0;
    bool used = false;
};
struct Uniform {
    const EffectUniformPin* pin = nullptr;
    V value{};
    bool read = false;
};
class Resolver {
  public:
    Resolver(const EffectLayout& vertex, const EffectLayout& pixel, const ReadEffectUniform& uniforms,
             const ReadEffectTexture& textures) : pixel(pixel), readUniform(uniforms), readTexture(textures) {
        if (vertex.features || vertex.textures.size() != 0) reject("Unrecognized effect vertex layout");
        loadUniforms(vertex, 1); loadUniforms(pixel, 0);
        for (const auto& pin : pixel.textures) {
            const auto role = static_cast<unsigned>(pin.role);
            if (role >= sources.size() || sources[role].pin || pin.resourceSlot >= 128 || pin.samplerSlot >= 16)
                reject("Invalid or duplicate effect texture binding");
            for (const auto& source : sources)
                if (source.pin && source.pin->resourceSlot == pin.resourceSlot)
                    reject("Effect color roles alias one resource slot");
            sources[role].pin = &pin;
        }
    }
    CaptureSupport run(const CaptureScope& scope) {
        const auto id = scope.passId;
        if (id == 0x1700) {
            if (scope.kind != CaptureScopeKind::PartialWrite || scope.fullOverwrite || pixel.features || pixel.textures.size() != 0)
                reject("Border requires an auxiliary partial-write contract");
            result.basis = "Pinned auxiliary border; preserve prior unwritten support and geometric T";
            return std::move(result);
        }
        if (scope.kind != CaptureScopeKind::DualColor || !scope.fullOverwrite)
            reject("Effect requires a full-overwrite DualColor contract");
        if (id == 0x1000) copy();
        else if (id >= 0x1100 && id <= 0x1103) worldBloom(static_cast<unsigned>(id - 0x1100));
        else if (id >= 0x1200 && id <= 0x120a) cinematicBloom(static_cast<unsigned>(id - 0x1200));
        else if (id >= 0x1302 && id <= 0x1307) dof(static_cast<unsigned>(id - 0x1300));
        else if (id == 0x1400) uber();
        else if (id == 0x1500) fxaa();
        else if (id >= 0x1600 && id <= 0x1604) shafts(static_cast<unsigned>(id - 0x1600));
        else reject("Unrecognized or shared-only effect pass cannot authorize color replay");
        for (const auto& source : sources)
            if (source.pin && !source.used) reject("Effect has an unaccounted color-dependent input");
        result.basis = "Pinned actual-draw effect support, pass " + std::to_string(id);
        if (pixel.features & EffectChromatic)
            result.basis += "; conservative chromatic M, un-dispersed geometric A";
        return std::move(result);
    }
  private:
    const EffectLayout& pixel;
    const ReadEffectUniform& readUniform;
    const ReadEffectTexture& readTexture;
    std::array<Source, textureCount> sources{};
    std::array<std::array<Uniform, uniformCount>, 2> constants{};
    CaptureSupport result;
    void loadUniforms(const EffectLayout& layout, unsigned stage) {
        for (const auto& pin : layout.uniforms) {
            const auto role = static_cast<unsigned>(pin.role);
            if (role >= uniformCount || constants[stage][role].pin || pin.bufferSlot >= 14 ||
                pin.byteOffset % 4 || pin.components == 0 || pin.components > 4 ||
                pin.byteOffset > 65536u - 4u * pin.components)
                reject("Invalid or duplicate effect uniform binding");
            constants[stage][role].pin = &pin;
        }
    }
    const V& uniform(bool vertex, U role, unsigned components) {
        auto& value = constants[vertex ? 1 : 0][static_cast<unsigned>(role)];
        if (!value.pin || value.pin->components < components) reject("Required effect uniform is absent from the pinned stage");
        if (!value.read) {
            if (!readUniform || !readUniform(vertex, *value.pin, value.value)) reject("Actual effect constant-buffer value is unavailable");
            for (unsigned i = 0; i < value.pin->components; ++i) {
                if (!std::isfinite(value.value[i])) reject("Non-finite effect support constant");
                // Do not silently substitute CPU denormal arithmetic for D3D's
                // FTZ arithmetic when deciding a flip or constructing an affine.
                if (std::fpclassify(value.value[i]) == FP_SUBNORMAL) reject("Subnormal effect support constant has no verified FP32 mapping");
            }
            value.read = true;
        }
        return value.value;
    }
    const V& texel(bool vertex, U role) {
        const auto& value = uniform(vertex, role, 2);
        if (!value[0] || !value[1]) reject("Effect texel size is zero");
        return value;
    }
    Source& source(T role) {
        auto& value = sources[static_cast<unsigned>(role)];
        if (!value.pin) reject("Required effect color input is absent from the pinned layout");
        if (!value.used) {
            if (!readTexture || !readTexture(*value.pin, value.width, value.height) || !value.width || !value.height)
                reject("Actual effect SRV subresource dimensions are unavailable");
            if (value.width > addressLimit || value.height > addressLimit) reject("Effect source exceeds safe support addressing");
            value.used = true;
        }
        return value;
    }
    void features(uint32_t allowed) {
        if (pixel.features & ~allowed) reject("Effect variant has unsupported sampling features");
    }
    CaptureSampleDomain domain(const Source& input, Map map, double ox = 0, double oy = 0,
                               double rx = 0, double ry = 0) {
        CaptureSampleDomain value;
        value.matrix = {scalar(map.x), 0, 0, scalar(map.y)};
        value.bias = {scalar(map.bx), scalar(map.by)};
        value.offsetTexels = {scalar(arithmetic(ox) * input.width), scalar(arithmetic(oy) * input.height)};
        const double sizes[] = {static_cast<double>(input.width), static_cast<double>(input.height)};
        const double scales[] = {value.matrix[0], value.matrix[3]};
        const double radii[] = {std::abs(arithmetic(rx)), std::abs(arithmetic(ry))};
        for (unsigned axis = 0; axis < 2; ++axis) {
            const double center0 = value.bias[axis] + value.offsetTexels[axis] / sizes[axis];
            const double center1 = center0 + scales[axis];
            if (std::max(std::abs(center0 * sizes[axis]), std::abs(center1 * sizes[axis])) > addressLimit)
                reject("Effect affine exceeds safe shader integer addressing");
            const double extent = radii[axis] * sizes[axis];
            if (!std::isfinite(extent) || extent > addressLimit) reject("Effect sampling radius exceeds the safe finite envelope");
            double rounded = std::ceil(extent);
            if (rounded > sizes[axis]) {
                // For a center within this axis of the source, a full-extent
                // radius already contains every possible source texel, for all
                // supported sampler address modes. Outside it (notably Border),
                // truncating the radius could lose a real in-bounds dependency.
                if (std::min(center0, center1) < 0 || std::max(center0, center1) > 1)
                    reject("Oversized effect kernel has an out-of-source center");
                rounded = sizes[axis];
            }
            value.radiusTexels[axis] = static_cast<unsigned>(rounded);
        }
        return value;
    }
    void add(T role, std::vector<CaptureSampleDomain> domains) {
        auto& input = source(role);
        if (domains.empty() || domains.size() > 64) reject("Effect has no bounded sampling domains");
        for (const auto& previous : result.inputs)
            if (previous.pixelResourceSlot == input.pin->resourceSlot) reject("Effect input was resolved twice");
        result.inputs.push_back({input.pin->resourceSlot, input.pin->samplerSlot, std::move(domains)});
    }
    void point(T role, Map map) {
        auto& input = source(role); add(role, {domain(input, map)});
    }
    void box(T role, Map map, double rx, double ry) {
        auto& input = source(role); add(role, {domain(input, map, 0, 0, rx, ry)});
    }
    void corners(T role, Map map, double x, double y, bool centerAndAxes) {
        auto& input = source(role);
        std::vector<CaptureSampleDomain> domains;
        // These are sampling footprints, not the original RGB weights/filter.
        for (int iy = -1; iy <= 1; ++iy) for (int ix = -1; ix <= 1; ++ix) {
            if (!centerAndAxes && (!ix || !iy)) continue;
            domains.push_back(domain(input, map, arithmetic(ix * x), arithmetic(iy * y)));
        }
        add(role, std::move(domains));
    }
    void geometric(T role, Map map) {
        auto& input = source(role);
        result.occlusionResourceSlot = static_cast<int>(input.pin->resourceSlot);
        result.occlusionDomain = domain(input, map);
    }
    void copy() {
        features(0); const auto map = st(uniform(true, U::MainSt, 4));
        point(T::Main, map); geometric(T::Main, map);
    }
    void prefilter(bool world, bool anti) {
        const auto map = st(uniform(!world, U::MainSt, 4));
        const auto& tex = texel(false, U::MainTexel);
        const double offset = uniform(false, U::PrefilterOffset, 1)[0];
        auto& input = source(T::Main);
        const double sx = world ? map.x : 1;
        const double sy = world ? map.y : 1;
        std::vector<CaptureSampleDomain> domains;
        auto tap = [&](int x, int y) {
            // World: offset raw UV then ST. Cinematic: ST then offset.
            domains.push_back(domain(input, map, arithmetic(arithmetic(offset + x) * tex[0]) * sx,
                                    arithmetic(arithmetic(offset + y) * tex[1]) * sy));
        };
        tap(0, 0);
        if (anti) { tap(-1, 0); tap(1, 0); tap(0, -1); tap(0, 1); }
        add(T::Main, std::move(domains));
    }
    void worldBloom(unsigned pass) {
        features(pass <= 1 ? static_cast<uint32_t>(EffectAntiFlicker) : 0u);
        if (pass == 0) { prefilter(true, (pixel.features & EffectAntiFlicker) != 0); return; }
        const auto map = st(uniform(true, U::MainSt, 4));
        const auto& tex = texel(false, U::MainTexel);
        if (pass <= 2) { corners(T::Main, map, tex[0], tex[1], false); return; }
        const double scale = uniform(false, U::SampleScale, 1)[0];
        corners(T::Main, map, arithmetic(tex[0] * scale), arithmetic(tex[1] * scale), true);
        const bool flipped = texel(true, U::BaseTexel)[1] < 0;
        point(T::Base, flipped ? flipAfter(map) : map);
        // Both branches are auxiliary Bloom; never grow geometric A with it.
    }
    void cinematicBloom(unsigned pass) {
        features(0);
        if (pass <= 1) { prefilter(false, pass == 1); return; }
        const auto map = st(uniform(true, U::MainSt, 4));
        const auto& tex = texel(false, U::MainTexel);
        if (pass <= 4) { corners(T::Main, map, tex[0], tex[1], false); return; }
        const bool highQuality = pass == 6 || pass == 8 || pass == 10;
        const double scale = arithmetic(uniform(false, U::SampleScale, 1)[0] * (highQuality ? 1.0 : 0.5));
        corners(T::Main, map, arithmetic(tex[0] * scale), arithmetic(tex[1] * scale), highQuality);
        auto base = st(uniform(true, U::BaseSt, 4));
        if (texel(true, U::BaseTexel)[1] < 0) {
            // This specific VS substitutes 1-q.y, NOT 1-BaseSt(q).y.
            base.y = -1; base.by = 1;
        }
        point(T::Base, base);
        if (pass >= 7) geometric(T::Base, base);
    }
    void dof(unsigned pass) {
        features(0);
        if (pass == 2 || pass == 7) {
            const auto& tex = texel(false, U::MainTexel);
            corners(T::Main, {}, tex[0] * 0.5, tex[1] * 0.5, false);
        } else {
            const double coc = uniform(false, U::MaxCoc, 1)[0];
            const double aspect = uniform(false, U::ReciprocalAspect, 1)[0];
            box(T::Main, {}, std::abs(arithmetic(coc * aspect)), std::abs(coc));
        }
        // CoC and blurred color are auxiliary: final Uber transports main A.
    }
    void uber() {
        constexpr uint32_t allowed = EffectChromatic | EffectDof | EffectCocView | EffectBloom | EffectBloomDirt;
        features(allowed);
        if (pixel.features & EffectCocView) reject("DoF CoC debug view has a different color contract");
        if ((pixel.features & EffectBloom) && (pixel.features & EffectBloomDirt)) reject("Conflicting Uber Bloom variants");
        const bool chromatic = (pixel.features & EffectChromatic) != 0;
        const bool hasDof = (pixel.features & EffectDof) != 0;
        const bool hasBloom = (pixel.features & (EffectBloom | EffectBloomDirt)) != 0;
        const auto main = st(uniform(!chromatic, U::MainSt, 4));
        double rx = 0, ry = 0;
        if (chromatic) {
            const double displacement = arithmetic(2.0 * std::abs(uniform(false, U::ChromaticAmount, 1)[0]));
            const auto& tex = uniform(false, U::MainTexel, 4);
            if (!tex[0] || !tex[1] || tex[2] <= 0 || tex[3] <= 0) reject("Chromatic texel geometry is invalid");
            const double cx = arithmetic(displacement * tex[2] * 0.5);
            const double cy = arithmetic(displacement * tex[3] * 0.5);
            arithmetic(cx * cx + cy * cy); // Bound the original tap-count arithmetic too.
            rx = arithmetic(displacement * std::abs(main.x));
            ry = arithmetic(displacement * std::abs(main.y));
        }
        box(T::Main, main, rx, ry);
        geometric(T::Main, main); // Deliberately un-dispersed geometry, not chromatic opacity.
        if (hasDof) {
            const bool flip = texel(!chromatic, U::MainTexel)[1] < 0;
            box(T::Dof, flip ? flipBefore(main) : main, rx, ry);
        }
        if (hasBloom) {
            auto bloom = st(uniform(true, U::MainSt, 4));
            if (texel(true, U::MainTexel)[1] < 0) bloom = flipBefore(bloom);
            const auto& tex = texel(false, U::BloomTexel);
            const double scale = uniform(false, U::BloomSettings, 2)[0];
            corners(T::Bloom, bloom, arithmetic(tex[0] * scale), arithmetic(tex[1] * scale), true);
        }
    }
    void fxaa() {
        features(0);
        const auto map = st(uniform(false, U::MainSt, 4));
        const auto& tex = texel(false, U::MainTexel);
        const auto& quality = uniform(false, U::FxaaQuality, 3);
        // Only the original preset range has a verified final subpixel bound.
        // Reject an override rather than silently narrowing its sampling domain.
        if (quality[0] < 0 || quality[0] > 1) reject("FXAA subpixel amount is outside the verified preset range");
        // Actual farthest search READ is 18.5 texels (the final +8 has no read);
        // the final subpixel read stays within one texel for these presets.
        box(T::Main, map, std::abs(arithmetic(tex[0] * 18.5)), std::abs(arithmetic(tex[1] * 18.5)));
        geometric(T::Main, map);
    }
    void shafts(unsigned pass) {
        features(0);
        if (pass == 1) {
            const auto& radius = uniform(true, U::BlurRadius, 2);
            const auto& sun = uniform(true, U::SunPosition, 2);
            auto& input = source(T::Main);
            std::vector<CaptureSampleDomain> domains;
            for (unsigned i = 0; i < 6; ++i) {
                const double x = arithmetic(static_cast<double>(i) * radius[0]);
                const double y = arithmetic(static_cast<double>(i) * radius[1]);
                domains.push_back(domain(input, {arithmetic(1 - x), arithmetic(1 - y),
                                                arithmetic(x * sun[0]), arithmetic(y * sun[1])}));
            }
            add(T::Main, std::move(domains));
            return;
        }
        point(T::Main, {});
        if (pass == 0 || pass == 4) {
            point(T::Shafts, texel(true, U::MainTexel)[1] < 0 ? flipBefore({}) : Map{});
            geometric(T::Main, {});
        }
    }
};
} // namespace
bool resolveEffectSupport(const CaptureScope& scope, const EffectLayout& vertex, const EffectLayout& pixel,
                          const ReadEffectUniform& uniform, const ReadEffectTexture& texture,
                          CaptureSupport& output, std::string& reason) {
    output = {}; reason.clear();
    try {
        Resolver resolver(vertex, pixel, uniform, texture);
        output = resolver.run(scope);
        return true;
    } catch (const std::exception& error) {
        reason = error.what();
        output = {};
        return false;
    } catch (...) {
        reason = "Effect support reader failed";
        output = {};
        return false;
    }
}
} // namespace dspaa
