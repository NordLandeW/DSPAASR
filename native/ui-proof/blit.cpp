#include "blit.h"
#include "shadow.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dspaa::proof {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID layoutKey{0xa41ac69b, 0xc695, 0x498b, {0x92, 0x4f, 0x39, 0x04, 0x8c, 0x0c, 0x11, 0x32}};
constexpr unsigned triangleVertices = 3, rectangleTriangles = 2;
constexpr unsigned maximumVertices = triangleVertices * rectangleTriangles;
struct Element {
    unsigned slot = 0, offset = 0, bytes = 0;
};
struct Layout {
    Element position, uv;
    bool valid = false;
};
[[noreturn]] void reject(const char* text) {
    throw std::invalid_argument(text);
}
bool finite(float value) {
    return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL;
}
double cross(const ClipBlitVertex& a, const ClipBlitVertex& b, double x, double y) {
    return (double(b.position[0]) - a.position[0]) * (y - a.position[1]) -
           (double(b.position[1]) - a.position[1]) * (x - a.position[0]);
}
unsigned formatBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 4 * sizeof(float);
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return 3 * sizeof(float);
    case DXGI_FORMAT_R32G32_FLOAT:
        return 2 * sizeof(float);
    case DXGI_FORMAT_R32_FLOAT:
        return sizeof(float);
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return 4;
    default:
        return 0;
    }
}
void readBytes(ConstantShadow& shadow, ID3D11Buffer* buffer, uint64_t offset, unsigned count, void* output) {
    if (offset > std::numeric_limits<unsigned>::max())
        reject("Blit IA byte offset overflow");
    ShadowReadInfo info;
    if (!shadow.readBytes(buffer, static_cast<unsigned>(offset), count, output, info))
        throw std::invalid_argument(std::string("Blit IA bytes unavailable: ") +
                                    (info.failure ? info.failure : "unknown") +
                                    " allocationBytes=" + std::to_string(info.description.ByteWidth) +
                                    " bind=" + std::to_string(info.description.BindFlags) +
                                    " usage=" + std::to_string(info.description.Usage) +
                                    " cpuAccess=" + std::to_string(info.description.CPUAccessFlags) +
                                    " requestBytes=" + std::to_string(count));
}
void element(ConstantShadow& shadow, ID3D11DeviceContext* context, const Element& part, uint64_t vertex,
             float* output, unsigned components) {
    ComPtr<ID3D11Buffer> buffer;
    UINT stride = 0, offset = 0;
    context->IAGetVertexBuffers(part.slot, 1, &buffer, &stride, &offset);
    if (!buffer || !stride || uint64_t(part.offset) + part.bytes > stride)
        reject("Blit IA layout does not fit its actual vertex stride");
    readBytes(shadow, buffer.Get(), uint64_t(offset) + vertex * stride + part.offset,
              components * sizeof(float), output);
}
void raster(ID3D11DeviceContext4* context) {
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, &target, &depth);
    if (!target)
        reject("Blit has no color attachment");
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    target->GetDesc(&view);
    ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture)))
        reject("Blit output is not a 2D texture");
    D3D11_TEXTURE2D_DESC image{};
    texture->GetDesc(&image);
    if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice ||
        image.ArraySize != 1 || image.MipLevels != 1 || image.SampleDesc.Count != 1)
        reject("Blit output is not a single complete raster subresource");
    D3D11_VIEWPORT viewport{};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    if (count != 1 || viewport.TopLeftX != 0 || viewport.TopLeftY != 0 || viewport.Width != image.Width ||
        viewport.Height != image.Height || !finite(viewport.MinDepth) || !finite(viewport.MaxDepth) ||
        viewport.MinDepth < 0 || viewport.MaxDepth > 1 || viewport.MinDepth > viewport.MaxDepth)
        reject("Blit viewport does not cover its whole output");
    ComPtr<ID3D11RasterizerState> state;
    context->RSGetState(&state);
    D3D11_RASTERIZER_DESC description{};
    if (state)
        state->GetDesc(&description);
    else {
        description.FillMode = D3D11_FILL_SOLID;
        description.CullMode = D3D11_CULL_BACK;
        description.DepthClipEnable = TRUE;
    }
    if (description.FillMode != D3D11_FILL_SOLID || description.CullMode != D3D11_CULL_NONE ||
        description.DepthBias || description.DepthBiasClamp != 0 || description.SlopeScaledDepthBias != 0)
        reject("Blit raster state does not prove solid uncropped coverage");
    if (description.ScissorEnable) {
        D3D11_RECT clip{};
        count = 1;
        context->RSGetScissorRects(&count, &clip);
        if (count != 1 || clip.left > 0 || clip.top > 0 || clip.right < static_cast<LONG>(image.Width) ||
            clip.bottom < static_cast<LONG>(image.Height))
            reject("Blit scissor clips its output");
    }
    ComPtr<ID3D11BlendState> blend;
    UINT sampleMask = 0;
    context->OMGetBlendState(&blend, nullptr, &sampleMask);
    if (!(sampleMask & 1u))
        reject("Blit sample mask disables its only sample");
    if (blend) {
        D3D11_BLEND_DESC settings{};
        blend->GetDesc(&settings);
        const auto& color = settings.RenderTarget[0];
        if (settings.AlphaToCoverageEnable || color.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL ||
            (color.BlendEnable &&
             (color.SrcBlend != D3D11_BLEND_ONE || color.DestBlend != D3D11_BLEND_ZERO ||
              color.SrcBlendAlpha != D3D11_BLEND_ONE || color.DestBlendAlpha != D3D11_BLEND_ZERO ||
              color.BlendOp != D3D11_BLEND_OP_ADD || color.BlendOpAlpha != D3D11_BLEND_OP_ADD)))
            reject("Blit does not overwrite all color channels");
        ComPtr<ID3D11BlendState1> extended;
        if (SUCCEEDED(blend.As(&extended))) {
            D3D11_BLEND_DESC1 value{};
            extended->GetDesc1(&value);
            if (value.RenderTarget[0].LogicOpEnable)
                reject("Blit logic operation is unverified");
        }
    }
    if (depth) {
        ComPtr<ID3D11DepthStencilState> tests;
        context->OMGetDepthStencilState(&tests, nullptr);
        D3D11_DEPTH_STENCIL_DESC settings{};
        if (tests)
            tests->GetDesc(&settings);
        else {
            settings.DepthEnable = TRUE;
            settings.DepthFunc = D3D11_COMPARISON_LESS;
        }
        if ((settings.DepthEnable && settings.DepthFunc != D3D11_COMPARISON_ALWAYS) || settings.StencilEnable)
            reject("Blit depth/stencil can reject covered pixels");
    }
}
} // namespace

bool clipBlitDomain(std::span<const ClipBlitVertex> vertices, D3D11_PRIMITIVE_TOPOLOGY topology,
                    CaptureSampleDomain& output, std::string& reason) {
    output = {};
    reason.clear();
    try {
        const bool single = vertices.size() == triangleVertices;
        const bool strip = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        if ((topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && !strip) ||
            (!single && vertices.size() != (strip ? 4u : maximumVertices)))
            reject("Blit is neither one triangle nor a two-triangle rectangle");
        for (const auto& vertex : vertices) {
            for (float value : vertex.position)
                if (!finite(value))
                    reject("Non-finite/subnormal blit position");
            for (float value : vertex.uv)
                if (!finite(value))
                    reject("Non-finite/subnormal blit UV");
            if (vertex.position[2] < 0 || vertex.position[2] > 1)
                reject("Blit geometry intersects depth clipping");
        }
        const auto& a = vertices[0];
        const auto& b = vertices[1];
        const auto& c = vertices[2];
        const double determinant = cross(a, b, c.position[0], c.position[1]);
        if (!std::isfinite(determinant) || determinant == 0)
            reject("Degenerate blit triangle");
        if (single) {
            for (double x : {-1., 1.})
                for (double y : {-1., 1.}) {
                    const double edges[]{cross(a, b, x, y), cross(b, c, x, y), cross(c, a, x, y)};
                    for (double edge : edges)
                        if (determinant > 0 ? edge < 0 : edge > 0)
                            reject("Blit triangle leaves part of the viewport uncovered");
                }
        } else {
            float left = a.position[0], right = left, bottom = a.position[1], top = bottom;
            for (const auto& vertex : vertices) {
                left = std::min(left, vertex.position[0]);
                right = std::max(right, vertex.position[0]);
                bottom = std::min(bottom, vertex.position[1]);
                top = std::max(top, vertex.position[1]);
            }
            if (left > -1 || right < 1 || bottom > -1 || top < 1)
                reject("Blit rectangle leaves part of the viewport uncovered");
            std::array<unsigned, maximumVertices> corners{};
            for (size_t i = 0; i < vertices.size(); ++i) {
                const auto& p = vertices[i].position;
                if ((p[0] != left && p[0] != right) || (p[1] != bottom && p[1] != top))
                    reject("Blit triangles do not form an axis-aligned rectangle");
                corners[i] = (p[0] == right ? 1u : 0u) | (p[1] == top ? 2u : 0u);
            }
            const std::array<unsigned, 3> first{corners[0], corners[1], corners[2]};
            const std::array<unsigned, 3> second =
                strip ? std::array<unsigned, 3>{corners[2], corners[1], corners[3]}
                      : std::array<unsigned, 3>{corners[3], corners[4], corners[5]};
            unsigned masks[2]{};
            for (unsigned corner : first)
                masks[0] |= 1u << corner;
            for (unsigned corner : second)
                masks[1] |= 1u << corner;
            const unsigned shared = masks[0] & masks[1];
            // Two distinct triangles must meet on a diagonal, not merely cover
            // the four outer corners while leaving a gap between their interiors.
            if ((masks[0] | masks[1]) != 0xFu || (shared != 0x9u && shared != 0x6u) || masks[0] == shared ||
                masks[1] == shared)
                reject("Blit triangles do not cover one complete rectangle");
        }
        CaptureSampleDomain domain;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const double dx = double(b.position[0]) - a.position[0],
                         dy = double(b.position[1]) - a.position[1];
            const double ex = double(c.position[0]) - a.position[0],
                         ey = double(c.position[1]) - a.position[1];
            const double du = double(b.uv[channel]) - a.uv[channel],
                         eu = double(c.uv[channel]) - a.uv[channel];
            const double x = (du * ey - eu * dy) / determinant, y = (dx * eu - ex * du) / determinant;
            const double bias = a.uv[channel] - x * a.position[0] - y * a.position[1];
            for (const auto& vertex : vertices)
                if (static_cast<float>(x * vertex.position[0] + y * vertex.position[1] + bias) !=
                    vertex.uv[channel])
                    reject("Blit UVs are not one affine map across both triangles");
            // D3D viewport origin is top-left: clip=(2*q.x-1,1-2*q.y).
            domain.matrix[channel * 2] = static_cast<float>(2 * x);
            domain.matrix[channel * 2 + 1] = static_cast<float>(-2 * y);
            domain.bias[channel] = static_cast<float>(bias - x + y);
            // The downstream support map is FP32. Do not silently replace the
            // observed affine relation with a rounded or underflowed map.
            if (double(domain.matrix[channel * 2]) != 2 * x ||
                double(domain.matrix[channel * 2 + 1]) != -2 * y ||
                double(domain.bias[channel]) != bias - x + y)
                reject("Blit affine relation is not exactly representable by the support map");
        }
        for (float value : domain.matrix)
            if (!finite(value))
                reject("Blit UV transform is not finite FP32");
        for (float value : domain.bias)
            if (!finite(value))
                reject("Blit UV translation is not finite FP32");
        output = domain;
        return true;
    } catch (const std::exception& error) {
        reason = error.what();
        return false;
    }
}

void observeBlitLayout(ID3D11InputLayout* target, const D3D11_INPUT_ELEMENT_DESC* elements,
                       unsigned count) noexcept {
    if (!target || !elements)
        return;
    Layout result{};
    unsigned positions = 0, uvs = 0;
    std::array<unsigned, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> ends{};
    for (unsigned i = 0; i < count; ++i) {
        const auto& value = elements[i];
        if (value.InputSlot >= ends.size())
            continue;
        const unsigned bytes = formatBytes(value.Format);
        unsigned offset = value.AlignedByteOffset;
        if (offset == D3D11_APPEND_ALIGNED_ELEMENT)
            offset = ends[value.InputSlot];
        ends[value.InputSlot] = bytes && offset <= std::numeric_limits<unsigned>::max() - bytes
                                    ? offset + bytes
                                    : D3D11_APPEND_ALIGNED_ELEMENT;
        const bool known = bytes && offset != D3D11_APPEND_ALIGNED_ELEMENT &&
                           value.InputSlotClass == D3D11_INPUT_PER_VERTEX_DATA && !value.InstanceDataStepRate;
        if (!value.SemanticName || value.SemanticIndex)
            continue;
        if (_stricmp(value.SemanticName, "POSITION") == 0) {
            ++positions;
            if (known && (value.Format == DXGI_FORMAT_R32G32B32_FLOAT ||
                          value.Format == DXGI_FORMAT_R32G32B32A32_FLOAT))
                result.position = {value.InputSlot, offset, bytes};
        } else if (_stricmp(value.SemanticName, "TEXCOORD") == 0) {
            ++uvs;
            if (known &&
                (value.Format == DXGI_FORMAT_R32G32_FLOAT || value.Format == DXGI_FORMAT_R32G32B32_FLOAT ||
                 value.Format == DXGI_FORMAT_R32G32B32A32_FLOAT))
                result.uv = {value.InputSlot, offset, bytes};
        }
    }
    result.valid = positions == 1 && uvs == 1 && result.position.bytes && result.uv.bytes;
    target->SetPrivateData(layoutKey, sizeof(result), &result);
}

bool inspectClipBlit(ID3D11DeviceContext4* context, ConstantShadow& shadow,
                     const capture::DrawArguments& arguments, unsigned sourceWidth, unsigned sourceHeight,
                     CaptureSupport& output, std::string& reason) {
    output = {};
    reason.clear();
    try {
        if (!arguments.count || arguments.count > maximumVertices || arguments.instances != 1)
            reject("Blit draw count/instancing has no bounded fullscreen proof");
        raster(context);
        ComPtr<ID3D11InputLayout> object;
        context->IAGetInputLayout(&object);
        Layout layout{};
        UINT bytes = sizeof(layout);
        if (!object || FAILED(object->GetPrivateData(layoutKey, &bytes, &layout)) ||
            bytes != sizeof(layout) || !layout.valid)
            reject("Blit input POSITION/TEXCOORD layout was not observed");
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetPrimitiveTopology(&topology);
        ComPtr<ID3D11Buffer> indices;
        DXGI_FORMAT indexFormat{};
        UINT indexOffset = 0;
        if (arguments.indexed)
            context->IAGetIndexBuffer(&indices, &indexFormat, &indexOffset);
        std::array<ClipBlitVertex, maximumVertices> vertices{};
        for (unsigned i = 0; i < arguments.count; ++i) {
            int64_t vertex = uint64_t(arguments.start) + i;
            if (arguments.indexed) {
                uint32_t index = 0;
                if (indexFormat == DXGI_FORMAT_R16_UINT) {
                    uint16_t narrow = 0;
                    readBytes(shadow, indices.Get(),
                              uint64_t(indexOffset) + (uint64_t(arguments.start) + i) * sizeof(narrow),
                              sizeof(narrow), &narrow);
                    index = narrow;
                } else if (indexFormat == DXGI_FORMAT_R32_UINT)
                    readBytes(shadow, indices.Get(),
                              uint64_t(indexOffset) + (uint64_t(arguments.start) + i) * sizeof(index),
                              sizeof(index), &index);
                else
                    reject("Blit index format is not a supported integer width");
                // A real indexed-list probe with negative base also dropped
                // the primitive using the maximum index. Never use this special
                // IA value as evidence of complete raster coverage.
                if (index == (indexFormat == DXGI_FORMAT_R16_UINT ? UINT16_MAX : UINT32_MAX))
                    reject("Blit maximum index cannot prove complete geometry");
                vertex = int64_t(index) + arguments.baseVertex;
            }
            if (vertex < 0 || uint64_t(vertex) > UINT32_MAX)
                reject("Blit vertex address is outside the IA range");
            element(shadow, context, layout.position, static_cast<uint64_t>(vertex),
                    vertices[i].position.data(), 3);
            element(shadow, context, layout.uv, static_cast<uint64_t>(vertex), vertices[i].uv.data(), 2);
        }
        CaptureSampleDomain domain;
        if (!clipBlitDomain(std::span(vertices.data(), arguments.count), topology, domain, reason))
            return false;
        if (!sourceWidth || !sourceHeight)
            reject("Blit source dimensions are unavailable");
        // Same safe integer-addressing envelope as the support transport.
        constexpr double addressLimit = std::numeric_limits<int32_t>::max() / 4;
        for (double x : {0., 1.})
            for (double y : {0., 1.})
                for (unsigned axis = 0; axis < 2; ++axis) {
                    const double uv =
                        domain.matrix[axis * 2] * x + domain.matrix[axis * 2 + 1] * y + domain.bias[axis];
                    if (std::abs(uv * (axis ? sourceHeight : sourceWidth)) > addressLimit)
                        reject("Blit UV map exceeds safe support addressing");
                }
        output.basis = "Exact native clip-blit shader pair and observed full-output IA geometry/raster";
        output.inputs.push_back({0, 0, {domain}});
        output.occlusionResourceSlot = 0;
        output.occlusionDomain = domain;
        return true;
    } catch (const std::exception& error) {
        reason = error.what();
        output = {};
        return false;
    }
}
} // namespace dspaa::proof
