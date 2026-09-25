#include "owner.h"
#include "gpu.h"
#include "hooks.h"
#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace dspaa {
using Microsoft::WRL::ComPtr;
namespace {
using namespace capture;
ComPtr<IUnknown> identity(IUnknown* resource) {
    ComPtr<IUnknown> value;
    if (resource) graphicsCheck(resource->QueryInterface(IID_PPV_ARGS(&value)), "Identify capture resource");
    return value;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Resource* resource) {
    ComPtr<ID3D11Texture2D> value;
    if (resource) resource->QueryInterface(IID_PPV_ARGS(&value));
    return value;
}
ComPtr<ID3D11Resource> resource(ID3D11View* view) {
    ComPtr<ID3D11Resource> value; if (view) view->GetResource(&value); return value;
}
unsigned pixelBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_TYPELESS: return 2;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
    default: return 16;
    }
}
uint64_t allocationBytes(unsigned width, unsigned height, DXGI_FORMAT format) {
    // Conservative tiled row/height padding, not a claim about driver residency.
    const uint64_t row = (static_cast<uint64_t>(width)*pixelBytes(format)+1023)&~uint64_t(1023);
    return row*((static_cast<uint64_t>(height)+255)&~uint64_t(255));
}
D3D11_BLEND_DESC blendDescription(ID3D11BlendState* state) {
    D3D11_BLEND_DESC description{};
    if (state) state->GetDesc(&description);
    else {
        for (auto& target : description.RenderTarget) {
            target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
            target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
            target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
    }
    return description;
}
bool logicEnabled(ID3D11BlendState* state) {
    if (!state) return false;
    ComPtr<ID3D11BlendState1> extended;
    if (FAILED(state->QueryInterface(IID_PPV_ARGS(&extended)))) return false;
    D3D11_BLEND_DESC1 description{}; extended->GetDesc1(&description);
    for (const auto& target : description.RenderTarget) if (target.LogicOpEnable) return true;
    return false;
}
bool depthMayWrite(ID3D11DepthStencilView* view, ID3D11DepthStencilState* state) {
    if (!view) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC target{}; view->GetDesc(&target);
    D3D11_DEPTH_STENCIL_DESC description{};
    if (state) state->GetDesc(&description);
    else { description.DepthEnable = TRUE; description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; }
    if (description.DepthEnable && description.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO &&
        !(target.Flags & D3D11_DSV_READ_ONLY_DEPTH)) return true;
    if (!description.StencilEnable || !description.StencilWriteMask || (target.Flags & D3D11_DSV_READ_ONLY_STENCIL)) return false;
    const auto writes = [](const D3D11_DEPTH_STENCILOP_DESC& face) {
        return face.StencilFailOp != D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP ||
               face.StencilPassOp != D3D11_STENCIL_OP_KEEP;
    };
    return writes(description.FrontFace) || writes(description.BackFace);
}
struct Record {
    ComPtr<IUnknown> key;
    ComPtr<ID3D11Texture2D> original;
    uint64_t epoch = 0;
    ImagePtr clean, transmittance, influence, ui, alphaSupport;
    bool uiDirty = false, resetRgb = false, constantT = true;
    // One-way for this UI scratch lifetime: clear-0/MAX alpha becomes clear-1/
    // zero-ADD fragment support. Partial flushes keep the mode; scope flush,
    // invalidation or color reseed retires it. Never reinterpret old MAX data.
    bool conservativeFragments = false;
};
struct Arena {
    std::unordered_map<IUnknown*, Record> records;
    std::vector<ImagePtr> images;
    std::vector<ComPtr<ID3D11Texture2D>> depths;
    std::vector<ComPtr<ID3D11View>> views;
    uint64_t bytes = 0, generation = 0;
    unsigned width = 0, height = 0;
    size_t usedDepths = 0;
};
struct DepthCopy {
    ComPtr<IUnknown> key;
    ComPtr<ID3D11Texture2D> texture;
    std::unordered_map<ID3D11DepthStencilView*, ComPtr<ID3D11DepthStencilView>> views;
    std::vector<ComPtr<ID3D11DepthStencilView>> originals;
};
struct Pending {
    std::shared_ptr<Arena> arena;
    uint64_t value = 0;
    std::weak_ptr<const CapturedUiFrame> lease;
};
} // namespace
struct CaptureOwner::Impl final : capture::Observer, std::enable_shared_from_this<Impl> {
    CaptureOwnerCreateInfo info;
    std::shared_ptr<Dx11Dx12> graphics;
    ComPtr<ID3D11Device5> device;
    ID3D11DeviceContext4* context = nullptr;
    std::unique_ptr<Gpu> gpu;
    ComPtr<ID3D11Fence> fence;
    uint64_t nextFence = 0;
    CaptureFrameInfo frame;
    CaptureStatus state;
    std::shared_ptr<Arena> arena;
    std::deque<Pending> pending;
    std::vector<std::shared_ptr<Arena>> available;
    uint64_t firstSupportDraw = 0;
    std::optional<CaptureScope> scope;
    CaptureTexture scopeOutput;
    std::unordered_map<IUnknown*, DepthCopy> depthCopies, supplementalDepthCopies;
    std::unordered_set<ID3D11Asynchronous*> activeQueries;
    std::unordered_map<uint32_t, ComPtr<ID3D11BlendState>> uiBlends;
    ComPtr<ID3D11BlendState> alphaSupportBlend, fragmentSupportBlend;
    std::shared_ptr<Impl> quarantineOwner; // Deliberate, allocation-free safety cycle on unknown retirement.
    bool stopped = false;
    bool unannotatedDrawReported = false;

    explicit Impl(const CaptureOwnerCreateInfo& value) : info(value), graphics(value.graphics) {
        if (!graphics || !info.maximumInFlightFrames || info.maximumInFlightFrames > 8 || !info.maximumTrackedTextures ||
            info.maximumTrackedTextures > 1024 || !info.maximumFrameBytes)
            throw std::invalid_argument("Invalid bounded capture owner configuration");
        auto lock = graphics->lock(); Bypass bypass;
        graphicsCheck(graphics->device11()->QueryInterface(IID_PPV_ARGS(&device)), "Capture D3D11 device interface");
        context = graphics->context11();
        gpu = std::make_unique<Gpu>(device.Get(), context);
        graphicsCheck(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create capture retirement fence");
        capture::install(context); state.hooksReady = true;
    }
    void failure(const char* reason, bool clean = true, bool occlusion = true, bool influence = true) noexcept {
        if (clean) state.cleanComplete = false;
        if (occlusion) state.occlusionComplete = false;
        if (influence) state.influenceComplete = false;
        try { if (state.reason.empty()) state.reason = reason ? reason : "Capture failed"; } catch (...) {}
    }
    void externalFailure(const char* reason) noexcept {
        try { auto lock = graphics->lock(); failure(reason); } catch (...) {}
    }
    void quarantine() noexcept {
        try {
            auto lock = graphics->lock(); state.quarantined = true; state.active = false;
            quarantineOwner = shared_from_this(); capture::detach(this);
        } catch (...) {}
    }
    void budget(uint64_t bytes) {
        if (!arena || bytes > info.maximumFrameBytes || arena->bytes > info.maximumFrameBytes-bytes)
            throw std::runtime_error("Capture private-resource budget exhausted");
        arena->bytes += bytes; state.allocatedFrameBytes = arena->bytes;
    }
    ImagePtr allocate(unsigned width, unsigned height, DXGI_FORMAT format) {
        for (const auto& candidate : arena->images)
            if (candidate.use_count() == 1 && candidate->width == width && candidate->height == height && candidate->format == format) {
                candidate->usedThisFrame = true; return candidate;
            }
        const auto bytes = allocationBytes(width, height, format);
        if (bytes <= info.maximumFrameBytes && arena->bytes > info.maximumFrameBytes-bytes) {
            // Only previous, fully retired-frame allocations which this frame
            // has never submitted are eligible for pressure eviction.
            std::erase_if(arena->images, [&](const ImagePtr& candidate) {
                if (candidate.use_count() != 1 || candidate->usedThisFrame) return false;
                arena->bytes -= allocationBytes(candidate->width, candidate->height, candidate->format); return true;
            });
        }
        budget(bytes);
        auto result = gpu->create(width, height, format); result->usedThisFrame = true; arena->images.push_back(result); return result;
    }
    AllocateImage allocator() { return [this](unsigned width, unsigned height, DXGI_FORMAT format) { return allocate(width,height,format); }; }
    ImagePtr color(ID3D11Texture2D* original) {
        if (!Gpu::supported(original)) throw std::invalid_argument("Capture color must be resolved, single-mip, single-layer 2D");
        D3D11_TEXTURE2D_DESC description{}; original->GetDesc(&description);
        return allocate(description.Width, description.Height, description.Format);
    }
    ImagePtr plane(const Image& shape, float value, DXGI_FORMAT format) {
        auto result = allocate(shape.width, shape.height, format); gpu->clear(*result, value); return result;
    }
    Record* find(ID3D11Resource* original) {
        if (!arena || !original) return nullptr;
        const auto key = identity(original); const auto found = arena->records.find(key.Get());
        return found == arena->records.end() ? nullptr : &found->second;
    }
    Record* find(ID3D11View* view) { return find(resource(view).Get()); }
    Record& declare(const CaptureTexture& value) {
        if (!arena || !value.texture || !value.epoch || !Gpu::supported(value.texture.Get()))
            throw std::invalid_argument("Capture texture declaration needs a valid 2D texture and allocation epoch");
        ComPtr<ID3D11Device> owner; value.texture->GetDevice(&owner);
        if (identity(owner.Get()).Get() != identity(device.Get()).Get()) throw std::invalid_argument("Capture texture belongs to another D3D11 device");
        auto key = identity(value.texture.Get());
        auto existing = arena->records.find(key.Get());
        if (existing == arena->records.end()) {
            if (arena->records.size() >= info.maximumTrackedTextures) throw std::runtime_error("Capture tracked-texture limit exhausted");
            Record record; record.key = key; record.original = value.texture; record.epoch = value.epoch;
            return arena->records.emplace(key.Get(), std::move(record)).first->second;
        }
        auto& record = existing->second;
        if (record.epoch != value.epoch) { invalidate(record); record.epoch = value.epoch; }
        return record;
    }
    Record& known(const CaptureTexture& value) {
        auto* record = find(value.texture.Get());
        if (!record || record->epoch != value.epoch || !record->clean || !record->transmittance || !record->influence)
            throw std::runtime_error("Capture dependency is missing or has a stale allocation epoch");
        return *record;
    }
    void invalidate(Record& record) {
        record.clean.reset(); record.transmittance.reset(); record.influence.reset(); record.ui.reset(); record.alphaSupport.reset();
        record.uiDirty = false; record.resetRgb = false; record.constantT = true; record.conservativeFragments = false; ++state.invalidations;
    }
    void flushUi(Record& record) {
        if (!record.ui || !record.uiDirty) return;
        if (!activeQueries.empty()) throw std::runtime_error("A raster-dependent query spans capture support work");
        auto t = allocate(record.ui->width, record.ui->height, DXGI_FORMAT_R32_FLOAT);
        auto m = allocate(record.ui->width, record.ui->height, DXGI_FORMAT_R16_FLOAT);
        gpu->extractUi(*record.ui, record.alphaSupport.get(), *record.influence, *t, *m, record.conservativeFragments);
        record.transmittance = std::move(t); record.influence = std::move(m); record.uiDirty = false; state.supportPasses += 2;
    }
    void flushAllUi() {
        if (arena) for (auto& entry : arena->records) {
            flushUi(entry.second);
            // T/M already contain this scope. Release its wide FP32 scratch for
            // later cameras in the same immediate command stream.
            entry.second.ui.reset(); entry.second.alphaSupport.reset(); entry.second.resetRgb = false;
            entry.second.conservativeFragments = false;
        }
    }
    ComPtr<ID3D11DepthStencilView> depth(ID3D11DepthStencilView* original, ID3D11DepthStencilState* depthState, bool supplemental = false) {
        if (!original) return {};
        if (depthState) {
            D3D11_DEPTH_STENCIL_DESC description{}; depthState->GetDesc(&description);
            if (!description.StencilEnable && (!description.DepthEnable ||
                (description.DepthFunc == D3D11_COMPARISON_ALWAYS && description.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO))) return {};
        }
        auto source = resource(original); auto key = identity(source.Get());
        auto& copies = supplemental ? supplementalDepthCopies : depthCopies;
        auto found = copies.find(key.Get());
        if (found == copies.end()) {
            auto native = texture(source.Get());
            if (!Gpu::supported(native.Get())) throw std::runtime_error("Unsupported capture depth/stencil resource domain");
            D3D11_TEXTURE2D_DESC description{}; native->GetDesc(&description);
            description.BindFlags = D3D11_BIND_DEPTH_STENCIL; description.Usage = D3D11_USAGE_DEFAULT;
            description.CPUAccessFlags = 0; description.MiscFlags = 0;
            DepthCopy copy; copy.key = key;
            for (size_t i = arena->usedDepths; i < arena->depths.size(); ++i) {
                D3D11_TEXTURE2D_DESC cached{}; arena->depths[i]->GetDesc(&cached);
                if (cached.Width == description.Width && cached.Height == description.Height && cached.Format == description.Format) {
                    std::swap(arena->depths[i], arena->depths[arena->usedDepths]); copy.texture = arena->depths[arena->usedDepths++]; break;
                }
            }
            if (!copy.texture) {
                budget(allocationBytes(description.Width, description.Height, description.Format));
                graphicsCheck(device->CreateTexture2D(&description, nullptr, &copy.texture), "Create private capture depth/stencil");
                arena->depths.push_back(copy.texture);
                std::swap(arena->depths.back(), arena->depths[arena->usedDepths++]);
            }
            context->CopyResource(copy.texture.Get(), native.Get());
            found = copies.emplace(key.Get(), std::move(copy)).first;
        } else if (supplemental) context->CopyResource(found->second.texture.Get(), source.Get());
        if (supplemental) ++state.alphaDepthCopies;
        auto& copy = found->second;
        const auto cached = copy.views.find(original); if (cached != copy.views.end()) return cached->second;
        D3D11_DEPTH_STENCIL_VIEW_DESC description{}; original->GetDesc(&description);
        if (description.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || description.Texture2D.MipSlice)
            throw std::runtime_error("Unsupported capture depth/stencil view");
        ComPtr<ID3D11DepthStencilView> view;
        graphicsCheck(device->CreateDepthStencilView(copy.texture.Get(), &description, &view), "Create matching private depth/stencil view");
        copy.originals.emplace_back(original); copy.views.emplace(original, view); arena->views.emplace_back(view); return view;
    }
    void ensureUi(Record& record) {
        if (!record.clean || !record.transmittance || !record.influence) throw std::runtime_error("UI output has no seeded clean branch");
        if (!record.ui) {
            record.ui = allocate(record.clean->width, record.clean->height, DXGI_FORMAT_R32G32B32A32_FLOAT);
            gpu->seedUi(*record.transmittance, *record.ui); ++state.supportPasses;
        }
    }
    void resetUiRgb(Record& record) {
        auto next = allocate(record.ui->width, record.ui->height, DXGI_FORMAT_R32G32B32A32_FLOAT);
        gpu->resetUiRgb(*record.ui, *next); record.ui = std::move(next); record.resetRgb = false; ++state.supportPasses;
    }
    void uiDraw(Record& output, StateGuard& saved, NativeCall& original) {
        auto description = blendDescription(saved.blend.Get());
        const auto source = description.RenderTarget[0];
        const unsigned rgb = source.RenderTargetWriteMask & 7u;
        if (description.AlphaToCoverageEnable || logicEnabled(saved.blend.Get()) || (rgb != 0 && rgb != 7) ||
            (!rgb && source.RenderTargetWriteMask)) {
            failure("Unsupported UI color mask, logic-op, or alpha-to-coverage", false); original.run(); return;
        }
        auto policy = rgb && info.uiShaderPolicy ? info.uiShaderPolicy(saved.pixelShader.Get()) : CaptureUiShaderPolicy{};
        auto destination = rgb ? (source.BlendEnable ? source.DestBlend : D3D11_BLEND_ZERO) : D3D11_BLEND_ONE;
        auto sourceFactor = rgb ? (source.BlendEnable ? source.SrcBlend : D3D11_BLEND_ONE) : D3D11_BLEND_ZERO;
        if (rgb && (!policy.unitAlphaVerified || (source.BlendEnable && source.BlendOp != D3D11_BLEND_OP_ADD) ||
                    (destination != D3D11_BLEND_ZERO && destination != D3D11_BLEND_ONE && destination != D3D11_BLEND_INV_SRC_ALPHA) ||
                    (sourceFactor != D3D11_BLEND_ZERO && sourceFactor != D3D11_BLEND_ONE && sourceFactor != D3D11_BLEND_SRC_ALPHA &&
                     sourceFactor != D3D11_BLEND_INV_SRC_ALPHA))) {
            failure("UI shader alpha or scalar blend factors lack verified runtime conditions", false); original.run(); return;
        }
        const bool alphaCovers = destination == D3D11_BLEND_ZERO || (destination == D3D11_BLEND_INV_SRC_ALPHA && sourceFactor == D3D11_BLEND_SRC_ALPHA);
        const bool fragmentFallback = rgb && source.BlendEnable && sourceFactor == D3D11_BLEND_SRC_ALPHA &&
            destination == D3D11_BLEND_INV_SRC_ALPHA && policy.unitAlphaVerified && !policy.finiteRgbVerified &&
            policy.conservativeFragmentSupportAllowed;
        // A prior promotion is not authorization for another unverified draw.
        if (rgb && alphaCovers && destination != D3D11_BLEND_ZERO && !policy.finiteRgbVerified && !fragmentFallback)
            failure("Alpha-only UI support requires finite RGB; zero alpha does not mask NaN/Inf", false, false, true);
        const bool contribution = rgb && !alphaCovers && sourceFactor != D3D11_BLEND_ZERO;
        const bool signedDraw = contribution && !policy.nonnegativeRgbVerified;
        if (signedDraw && !policy.signedContributionWithoutSelfOverlapVerified) {
            failure("Signed UI contribution needs verified per-fragment support or a non-self-overlapping draw", false, false, true);
        }
        ensureUi(output);
        if (signedDraw && policy.signedContributionWithoutSelfOverlapVerified) { flushUi(output); resetUiRgb(output); }
        else if (contribution && output.resetRgb) resetUiRgb(output);
        auto privateDepth = depth(saved.depthView.Get(), saved.depthState.Get()); // BEFORE original can mutate it.
        D3D11_BLEND_DESC captureBlend{}; auto& target = captureBlend.RenderTarget[0];
        target.BlendEnable = TRUE; target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.SrcBlend = contribution && (!signedDraw || policy.signedContributionWithoutSelfOverlapVerified) ? sourceFactor : D3D11_BLEND_ZERO;
        target.DestBlend = signedDraw && policy.signedContributionWithoutSelfOverlapVerified ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO; target.DestBlendAlpha = destination;
        target.RenderTargetWriteMask = rgb ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0;
        if (contribution && (!signedDraw || policy.signedContributionWithoutSelfOverlapVerified)) target.RenderTargetWriteMask |= 7;
        const uint32_t blendKey = static_cast<uint32_t>(target.SrcBlend) | (static_cast<uint32_t>(target.DestBlend) << 8) |
                                  (static_cast<uint32_t>(target.DestBlendAlpha) << 16) | (static_cast<uint32_t>(target.RenderTargetWriteMask) << 24);
        auto blend = uiBlends.find(blendKey);
        if (blend == uiBlends.end()) {
            ComPtr<ID3D11BlendState> created;
            graphicsCheck(device->CreateBlendState(&captureBlend, &created), "Create UI transmittance/contribution blend");
            blend = uiBlends.emplace(blendKey, std::move(created)).first;
        }
        bool replayAlpha = rgb && destination == D3D11_BLEND_INV_SRC_ALPHA && state.influenceComplete;
        ComPtr<ID3D11DepthStencilView> alphaDepth;
        if (replayAlpha) {
            try {
                if (fragmentFallback && !output.conservativeFragments) {
                    // Commit old precise support BEFORE reusing its scratch.
                    // T and original color are unchanged. Once promoted, later
                    // source-over draws keep the wider actual-fragment bound.
                    flushUi(output);
                    if (!output.alphaSupport) output.alphaSupport = plane(*output.clean, 1, DXGI_FORMAT_R32G32B32A32_FLOAT);
                    else gpu->clear(*output.alphaSupport, 1);
                    output.conservativeFragments = true; ++state.conservativeFragmentPromotions;
                } else if (!output.alphaSupport) output.alphaSupport = plane(*output.clean, 0, DXGI_FORMAT_R32G32B32A32_FLOAT);
                // A second raster invocation must see the SAME draw pre-state.
                // Keeping writes enabled is essential for self-overlapping primitives.
                alphaDepth = depthMayWrite(privateDepth.Get(), saved.depthState.Get())
                    ? depth(privateDepth.Get(), saved.depthState.Get(), true) : privateDepth;
                auto& supportBlend = output.conservativeFragments ? fragmentSupportBlend : alphaSupportBlend;
                if (!supportBlend) {
                    D3D11_BLEND_DESC support{}; auto& alpha = support.RenderTarget[0];
                    alpha.BlendEnable = TRUE; alpha.BlendOp = D3D11_BLEND_OP_ADD;
                    alpha.BlendOpAlpha = output.conservativeFragments ? D3D11_BLEND_OP_ADD : D3D11_BLEND_OP_MAX;
                    alpha.SrcBlend = D3D11_BLEND_ZERO; alpha.DestBlend = D3D11_BLEND_ONE;
                    // Unit-alpha proof excludes NaN/Inf from this independent
                    // equation. Unknown RGB is never written or read by alpha.
                    // MIN/MAX would ignore ZERO factors and cannot be a sink.
                    alpha.SrcBlendAlpha = alpha.DestBlendAlpha = output.conservativeFragments ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
                    alpha.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
                    graphicsCheck(device->CreateBlendState(&support, &supportBlend), "Create independent UI support blend");
                }
            } catch (const std::exception& error) {
                failure(error.what(), false, false, true); replayAlpha = false;
            } catch (...) {
                failure("Independent alpha support preparation failed", false, false, true); replayAlpha = false;
            }
        }
        original.run();
        auto* view = output.ui->rtv.Get(); context->OMSetRenderTargets(1, &view, privateDepth.Get());
        context->OMSetBlendState(blend->second.Get(), saved.blendFactor.data(), saved.sampleMask);
        original.replay(); ++state.coverageReplays;
        if (replayAlpha) {
            view = output.alphaSupport->rtv.Get(); context->OMSetRenderTargets(1, &view, alphaDepth.Get());
            const auto& supportBlend = output.conservativeFragments ? fragmentSupportBlend : alphaSupportBlend;
            context->OMSetBlendState(supportBlend.Get(), saved.blendFactor.data(), saved.sampleMask);
            original.replay(); ++state.coverageReplays; ++state.alphaSupportReplays;
            if (output.conservativeFragments) ++state.conservativeFragmentReplays;
        }
        saved.restore();
        output.uiDirty = rgb != 0 || output.uiDirty;
        if (rgb && destination != D3D11_BLEND_ONE) output.constantT = false;
        if (signedDraw && policy.signedContributionWithoutSelfOverlapVerified) {
            flushUi(output); output.resetRgb = true; ++state.signedContributionDraws;
        }
    }
    void dualDraw(Record& output, StateGuard& saved, const DrawArguments& arguments, NativeCall& original) {
        CaptureSupport verifiedSupport;
        if (!info.effectShaderPolicy ||
            !info.effectShaderPolicy(saved.pixelShader.Get(), *scope, arguments, verifiedSupport) ||
            verifiedSupport.basis.empty())
            throw std::runtime_error("Effect shader or actual sampling bindings are unverified");
        const bool partial = scope->kind == CaptureScopeKind::PartialWrite || !scope->fullOverwrite;
        flushUi(output);
        if (partial && !output.clean) throw std::runtime_error("Partial capture write has no valid prior destination");
        if (!output.clean) {
            const auto blend = blendDescription(saved.blend.Get()).RenderTarget[0];
            if (blend.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL ||
                (blend.BlendEnable && (blend.DestBlend != D3D11_BLEND_ZERO || blend.DestBlendAlpha != D3D11_BLEND_ZERO)))
                throw std::runtime_error("Fresh full-overwrite destination still depends on prior color/alpha");
            output.clean = color(output.original.Get());
            output.transmittance = plane(*output.clean, 1, DXGI_FORMAT_R32_FLOAT);
            output.influence = plane(*output.clean, 0, DXGI_FORMAT_R16_FLOAT);
        }
        std::array<ID3D11ShaderResourceView*, 128> inputs = saved.resources[4];
        std::vector<ComPtr<ID3D11ShaderResourceView>> privateViews;
        std::unordered_map<unsigned, Record*> paired;
        for (unsigned slot = 0; slot < inputs.size(); ++slot) {
            if (!inputs[slot]) continue;
            auto* input = find(inputs[slot]);
            if (!input || !input->clean) continue;
            flushUi(*input);
            if (input == &output) throw std::runtime_error("Capture color pass reads and writes the same logical subresource");
            auto view = gpu->view(*input->clean, inputs[slot]); inputs[slot] = view.Get();
            privateViews.push_back(view); arena->views.emplace_back(view); paired.emplace(slot, input);
        }
        auto destination = gpu->view(*output.clean, saved.targets[0]); arena->views.emplace_back(destination);
        auto privateDepth = depth(saved.depthView.Get(), saved.depthState.Get());
        original.run(); auto* view = destination.Get(); context->OMSetRenderTargets(1, &view, privateDepth.Get());
        context->PSSetShaderResources(0, 128, inputs.data()); original.replay(); ++state.colorReplays; saved.restore();
        output.ui.reset(); output.alphaSupport.reset(); output.uiDirty = false; output.resetRgb = false; output.conservativeFragments = false;
        std::unordered_set<unsigned> listed;
        auto combined = plane(*output.clean, 0, DXGI_FORMAT_R16_FLOAT);
        for (const auto& input : verifiedSupport.inputs) {
            if (input.pixelResourceSlot >= 128 || !listed.insert(input.pixelResourceSlot).second)
                throw std::runtime_error("Capture support input is duplicated or out of range");
            const auto found = paired.find(input.pixelResourceSlot);
            if (found == paired.end()) throw std::runtime_error("Declared color support dependency has no clean/influence branch");
            const auto sampler = Gpu::samplerDescription(context, input.pixelSamplerSlot);
            auto propagated = gpu->support(*found->second->influence, input, *output.clean, sampler, allocator());
            auto united = allocate(output.clean->width, output.clean->height, DXGI_FORMAT_R16_FLOAT);
            gpu->unite(*combined, *propagated, *united); combined = std::move(united); ++state.supportPasses;
        }
        // Unused resource slots can retain old bindings after a shader switch.
        // Completeness is the verified pass contract, not every non-null SRV.
        if (partial) {
            auto united = allocate(output.clean->width, output.clean->height, DXGI_FORMAT_R16_FLOAT);
            gpu->unite(*output.influence, *combined, *united); combined = std::move(united); ++state.supportPasses;
        }
        output.influence = std::move(combined);
        if (verifiedSupport.occlusionResourceSlot >= 0) {
            const auto slot = static_cast<unsigned>(verifiedSupport.occlusionResourceSlot);
            const auto found = paired.find(slot);
            const auto input = std::find_if(verifiedSupport.inputs.begin(), verifiedSupport.inputs.end(), [slot](const auto& entry) { return entry.pixelResourceSlot == slot; });
            if (found == paired.end() || input == verifiedSupport.inputs.end()) {
                failure("Geometric occlusion input is missing from capture support", false, true, false); return;
            }
            if (partial) {
                failure("Partial-write geometric occlusion needs an explicit per-fragment transport contract", false, true, false); return;
            }
            output.transmittance = gpu->occlusion(*found->second->transmittance, verifiedSupport.occlusionDomain, *output.clean,
                                                 Gpu::samplerDescription(context, input->pixelSamplerSlot), allocator());
            output.constantT = found->second->constantT; ++state.supportPasses;
        } else if (!partial) {
            output.transmittance = plane(*output.clean, 1, DXGI_FORMAT_R32_FLOAT); output.constantT = true;
        } else if (!output.constantT) failure("Partial color-only write cannot silently erase existing geometric opacity", false, true, false);
    }
    template <class Visit> void drawOutputs(Visit&& visit) {
        if (arena->records.empty())
            return;
        // Original-only draws can write through both MRTs and OM UAVs. Acquire
        // every Get-returned reference before any fallible resource lookup.
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> retained;
        context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
        for (size_t i = 0; i < targets.size(); ++i)
            retained[i].Attach(targets[i]);
        const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1
                               ? D3D11_1_UAV_SLOT_COUNT
                               : D3D11_PS_CS_UAV_REGISTER_COUNT;
        std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> unordered{};
        std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> retainedUnordered;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, unordered.data());
        for (size_t i = 0; i < unordered.size(); ++i)
            retainedUnordered[i].Attach(unordered[i]);
        for (const auto& target : retained)
            if (auto* output = find(target.Get()))
                visit(*output, target.Get());
        for (const auto& view : retainedUnordered)
            if (auto* output = find(view.Get()))
                visit(*output, nullptr);
    }
    void draw(const DrawArguments& arguments, NativeCall& original) {
        ++state.observedDraws;
        if (!scope || scope->kind == CaptureScopeKind::SharedPreparation ||
            scope->kind == CaptureScopeKind::ExternalBlurPublication) {
            // Shared preparation is original-only, not permission to retain old
            // private values for its UAV writes. Independent consumers survive.
            drawOutputs([&](Record& output, ID3D11RenderTargetView* target) {
                if (scope || !target)
                    invalidate(output);
                else if (output.clean) {
                    if (!unannotatedDrawReported && state.reason.empty() && info.unannotatedDraw) {
                        unannotatedDrawReported = true;
                        try {
                            info.unannotatedDraw(target);
                        } catch (...) {
                        }
                    }
                    invalidate(output);
                }
            });
            original.run();
            return;
        }
        StateGuard saved(context);
        if (!saved.pureRaster || !activeQueries.empty()) throw std::runtime_error("Capture refuses UAV/SO/query/predication or unverified programmable geometry side effects");
        for (unsigned slot = 1; slot < saved.targets.size(); ++slot)
            if (saved.targets[slot]) throw std::runtime_error("Capture pass uses unsupported multiple color outputs");
        if (!saved.targets[0]) throw std::runtime_error("Capture draw has no declared color output");
        auto* output = find(saved.targets[0]);
        if (scope->implicitOutputEpoch) {
            const CaptureTexture observed{texture(resource(saved.targets[0]).Get()),scope->implicitOutputEpoch};
            output = &declare(observed);
        }
        if (!output) throw std::runtime_error("Capture output was not declared with an allocation epoch");
        if (scope->kind == CaptureScopeKind::FullOnlyUiCoverage) uiDraw(*output, saved, original);
        else {
            dualDraw(*output, saved, arguments, original);
            scopeOutput = {output->original,output->epoch};
        }
    }
    void transfer(const Operation& operation, NativeCall& original) {
        const auto depthOutput = depthCopies.find(identity(operation.destination).Get());
        if (depthOutput != depthCopies.end()) {
            const auto depthInput = depthCopies.find(identity(operation.source).Get());
            auto* source = depthInput == depthCopies.end() ? operation.source : depthInput->second.texture.Get();
            original.run();
            if (operation.kind == OperationKind::Copy) context->CopyResource(depthOutput->second.texture.Get(), source);
            else if (operation.kind == OperationKind::CopyRegion)
                context->CopySubresourceRegion1(depthOutput->second.texture.Get(), operation.destinationSubresource, operation.x, operation.y, operation.z,
                                               source, operation.sourceSubresource, operation.box, operation.flags);
            else throw std::runtime_error("Unsupported depth/stencil resolve inside capture scope");
            ++state.mirroredTransfers; return;
        }
        auto* input = find(operation.source); auto* output = find(operation.destination);
        if (!output) { original.run(); return; }
        const bool shared = scope && (scope->kind == CaptureScopeKind::SharedPreparation || scope->kind == CaptureScopeKind::ExternalBlurPublication);
        if (shared || !input || !input->clean) {
            const bool lost = !shared && static_cast<bool>(output->clean); invalidate(*output);
            if (lost) failure("A captured transfer source has no clean dependency");
            original.run(); return;
        }
        if (scope && scope->kind == CaptureScopeKind::FullOnlyUiCoverage)
            throw std::runtime_error("UI-scope color transfer needs an explicit independent-UI composition contract");
        if (input == output || operation.sourceSubresource || operation.destinationSubresource || operation.z)
            throw std::runtime_error("Capture transfer has overlapping or unsupported subresources");
        flushUi(*input); flushUi(*output);
        const bool partial = operation.kind == OperationKind::CopyRegion;
        if (partial && (operation.flags & D3D11_COPY_DISCARD)) throw std::runtime_error("Discarding partial transfer has no preserved destination contract");
        if (partial && !output->clean) throw std::runtime_error("Partial transfer has no prior captured destination");
        if (!output->clean) {
            output->clean = color(output->original.Get());
            output->transmittance = plane(*output->clean, 1, DXGI_FORMAT_R32_FLOAT);
            output->influence = plane(*output->clean, 0, DXGI_FORMAT_R16_FLOAT);
        }
        if (operation.kind == OperationKind::Resolve) throw std::runtime_error("Multisample capture must resolve before the clean root is seeded");
        original.run();
        if (partial) {
            context->CopySubresourceRegion1(output->clean->texture.Get(), 0, operation.x, operation.y, 0, input->clean->texture.Get(), 0, operation.box, operation.flags);
            context->CopySubresourceRegion1(output->transmittance->texture.Get(), 0, operation.x, operation.y, 0, input->transmittance->texture.Get(), 0, operation.box, operation.flags);
            context->CopySubresourceRegion1(output->influence->texture.Get(), 0, operation.x, operation.y, 0, input->influence->texture.Get(), 0, operation.box, operation.flags);
            output->constantT = output->constantT && input->constantT;
        } else {
            context->CopyResource(output->clean->texture.Get(), input->clean->texture.Get());
            context->CopyResource(output->transmittance->texture.Get(), input->transmittance->texture.Get());
            context->CopyResource(output->influence->texture.Get(), input->influence->texture.Get()); output->constantT = input->constantT;
        }
        output->ui.reset(); output->alphaSupport.reset(); output->uiDirty = false; output->resetRgb = false; output->conservativeFragments = false; ++state.mirroredTransfers;
    }
    void clear(const Operation& operation, NativeCall& original) {
        auto source = resource(operation.view);
        ComPtr<ID3D11DepthStencilView> depthView;
        if (operation.view && SUCCEEDED(operation.view->QueryInterface(IID_PPV_ARGS(&depthView)))) {
            if (scope && scope->kind != CaptureScopeKind::SharedPreparation && scope->kind != CaptureScopeKind::ExternalBlurPublication) {
                auto copy = depth(depthView.Get(), nullptr); original.run();
                if (operation.kind == OperationKind::ClearDepth)
                    context->ClearDepthStencilView(copy.Get(), operation.flags, operation.depth, operation.stencil);
                else context->ClearView(copy.Get(), operation.color.data(), operation.rectangles, operation.rectangleCount);
                ++state.mirroredTransfers; return;
            }
            original.run(); return;
        }
        auto* output = find(source.Get());
        if (!output) { original.run(); return; }
        if (!scope || scope->kind == CaptureScopeKind::SharedPreparation || scope->kind == CaptureScopeKind::ExternalBlurPublication) {
            const bool unscoped = !scope && static_cast<bool>(output->clean); invalidate(*output);
            if (unscoped) failure("An unannotated clear changed a captured color dependency");
            original.run(); return;
        }
        if (scope->kind == CaptureScopeKind::FullOnlyUiCoverage) throw std::runtime_error("UI color clear needs an explicit clean-background handoff");
        ComPtr<ID3D11RenderTargetView> native;
        if (FAILED(operation.view->QueryInterface(IID_PPV_ARGS(&native)))) throw std::runtime_error("Unsupported capture ClearView resource class");
        const bool partial = operation.kind == OperationKind::ClearView && operation.rectangleCount;
        flushUi(*output);
        if (partial && !output->clean) throw std::runtime_error("Partial clear has no captured destination");
        if (!output->clean) {
            output->clean = color(output->original.Get()); output->transmittance = plane(*output->clean,1,DXGI_FORMAT_R32_FLOAT);
            output->influence = plane(*output->clean,0,DXGI_FORMAT_R16_FLOAT);
        }
        auto view = gpu->view(*output->clean, native.Get()); arena->views.emplace_back(view); original.run();
        if (operation.kind == OperationKind::ClearView) context->ClearView(view.Get(), operation.color.data(), operation.rectangles, operation.rectangleCount);
        else context->ClearRenderTargetView(view.Get(), operation.color.data());
        const float one[] = {1,1,1,1}, zero[] = {0,0,0,0};
        context->ClearView(output->transmittance->rtv.Get(), one, operation.rectangles, operation.rectangleCount);
        context->ClearView(output->influence->rtv.Get(), zero, operation.rectangles, operation.rectangleCount);
        if (!partial) output->constantT = true;
        output->ui.reset(); output->alphaSupport.reset(); output->uiDirty = false; output->resetRgb = false; output->conservativeFragments = false; ++state.mirroredTransfers;
    }
    void query(const Operation& operation) {
        if (operation.kind == OperationKind::QueryEnd) { activeQueries.erase(operation.query); return; }
        ComPtr<ID3D11Query> query;
        if (!operation.query || FAILED(operation.query->QueryInterface(IID_PPV_ARGS(&query)))) { activeQueries.insert(operation.query); return; }
        D3D11_QUERY_DESC description{}; query->GetDesc(&description);
        if (description.Query != D3D11_QUERY_TIMESTAMP && description.Query != D3D11_QUERY_TIMESTAMP_DISJOINT && description.Query != D3D11_QUERY_EVENT)
            activeQueries.insert(operation.query);
    }
    void observe(const Operation& operation, NativeCall& original) noexcept override {
        try {
            auto lock = graphics->lock(); Bypass bypass;
            if (operation.kind == OperationKind::QueryBegin || operation.kind == OperationKind::QueryEnd) { query(operation); original.run(); return; }
            if (!state.active || !arena || stopped) { original.run(); return; }
            switch (operation.kind) {
            case OperationKind::Draw:
                draw(operation.draw, original);
                return;
            case OperationKind::Copy: case OperationKind::CopyRegion: case OperationKind::Resolve: transfer(operation, original); return;
            case OperationKind::ClearColor: case OperationKind::ClearDepth: case OperationKind::ClearView: clear(operation, original); return;
            case OperationKind::ExecuteList:
                failure("Opaque deferred command list cannot provide verified dual-color/UI coverage"); original.run(); return;
            case OperationKind::UnsupportedDraw: {
                const bool shared = scope && (scope->kind == CaptureScopeKind::SharedPreparation ||
                                              scope->kind == CaptureScopeKind::ExternalBlurPublication);
                if (scope && !shared)
                    failure("Indirect/auto draw has no bounded capture replay contract");
                drawOutputs([&](Record& output, ID3D11RenderTargetView*) {
                    if (!shared && output.clean)
                        failure("An unsupported draw changed a captured dependency");
                    invalidate(output);
                });
                original.run();
                return;
            }
            case OperationKind::Dispatch: {
                const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
                std::array<ID3D11UnorderedAccessView*, 64> views{}; context->CSGetUnorderedAccessViews(0, count, views.data());
                for (auto* view : views) if (view) { if (auto* output = find(view)) invalidate(*output); view->Release(); }
                if (scope && scope->kind != CaptureScopeKind::SharedPreparation && scope->kind != CaptureScopeKind::ExternalBlurPublication)
                    failure("Compute color work requires an explicit non-replayed preparation contract");
                original.run(); return;
            }
            default: {
                auto viewResource = resource(operation.view); auto* destination = operation.destination ? operation.destination : viewResource.Get();
                if (depthCopies.count(identity(destination).Get())) failure("Unmirrored depth/stencil mutation inside capture scope");
                auto* output = find(destination);
                if (output) {
                    const bool wasPaired = static_cast<bool>(output->clean); invalidate(*output);
                    const bool shared = scope && (scope->kind == CaptureScopeKind::SharedPreparation || scope->kind == CaptureScopeKind::ExternalBlurPublication);
                    if (wasPaired && !shared && operation.kind != OperationKind::Discard) failure("Unsupported mutation invalidated a captured dependency");
                }
                original.run(); return;
            }
            }
        } catch (const std::exception& error) {
            try { auto lock = graphics->lock(); failure(error.what()); } catch (...) {}
        } catch (...) { try { auto lock = graphics->lock(); failure("Unknown capture observer failure"); } catch (...) {} }
        if (!original.called) original.run();
    }
    void retire(const CapturedUiLease& lease = {}) {
        scopeOutput = {};
        if (!arena) return;
        if (nextFence == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("Capture fence timeline exhausted");
        const auto value = ++nextFence;
        graphicsCheck(context->Signal(fence.Get(), value), "Signal captured UI completion");
        pending.push_back({arena, value, lease}); arena.reset(); depthCopies.clear(); supplementalDepthCopies.clear(); scope.reset();
        state.active = false;
    }
    void reap() {
        const auto completed = fence->GetCompletedValue();
        if (completed == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("Capture device was removed");
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->value > completed || !it->lease.expired()) { ++it; continue; }
            auto ready = it->arena; ready->records.clear(); ready->views.clear(); ready->usedDepths = 0;
            for (const auto& image : ready->images) image->usedThisFrame = false;
            available.push_back(std::move(ready)); it = pending.erase(it);
        }
    }
    bool releaseApplicationReferences() noexcept {
        try {
            auto lock = graphics->lock();
            if (arena) return false;
            const auto completed = fence->GetCompletedValue();
            if (completed == std::numeric_limits<uint64_t>::max() || completed < nextFence) return false;
            // Keep all private images/views and external leases. Only the records
            // own references to the application's color textures/identities.
            for (auto& entry : pending) entry.arena->records.clear();
            for (auto& entry : available) entry->records.clear();
            return true;
        } catch (...) { return false; }
    }
    bool stop() noexcept {
        try {
            auto lock = graphics->lock(); Bypass bypass;
            if (stopped) return !state.quarantined;
            stopped = true; capture::detach(this);
            if (arena) { failure("Capture stopped before frame seal"); retire(); }
            if (!pending.empty()) {
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event) throw std::runtime_error("Create capture retirement event failed");
                const auto hr = fence->SetEventOnCompletion(nextFence, event);
                if (FAILED(hr)) { CloseHandle(event); graphicsCheck(hr, "Arm capture retirement event"); }
                context->Flush(); const auto waited = WaitForSingleObject(event, 30000); CloseHandle(event);
                if (waited != WAIT_OBJECT_0) throw std::runtime_error("Capture GPU retirement exceeded its bounded wait");
            }
            // Event wake-up alone is not GPU retirement (including device loss).
            if (!releaseApplicationReferences()) throw std::runtime_error("Capture retirement fence did not actually complete");
            pending.clear(); available.clear(); depthCopies.clear(); supplementalDepthCopies.clear(); activeQueries.clear(); state.active = false; return true;
        } catch (const std::exception& error) { externalFailure(error.what()); quarantine(); return false; }
        catch (...) { externalFailure("Capture stop failed"); quarantine(); return false; }
    }
};
CaptureOwner::CaptureOwner(const CaptureOwnerCreateInfo& info) : impl_(std::make_shared<Impl>(info)) { capture::attach(impl_->context, impl_); }
CaptureOwner::~CaptureOwner() { stop(); }
bool CaptureOwner::beginFrame(const CaptureFrameInfo& frame) noexcept {
    try {
        auto lock = impl_->graphics->lock(); Bypass bypass;
        if (impl_->stopped || impl_->state.quarantined) return false;
        if (impl_->arena) { impl_->externalFailure("Previous capture frame was not sealed"); impl_->retire(); }
        impl_->reap();
        impl_->state = {}; impl_->state.hooksReady = true; impl_->state.applicationFrameId = frame.applicationFrameId; impl_->state.generation = frame.generation;
        impl_->unannotatedDrawReported = false;
        if (!frame.applicationFrameId || !frame.generation || !frame.width || !frame.height || frame.width > 16384 || frame.height > 16384)
            throw std::invalid_argument("Invalid capture frame identity/extent");
        if (impl_->pending.size() >= impl_->info.maximumInFlightFrames) throw std::runtime_error("Capture frame leases are still in flight");
        impl_->frame = frame;
        if (!impl_->available.empty()) { impl_->arena = std::move(impl_->available.back()); impl_->available.pop_back(); }
        else impl_->arena = std::make_shared<Arena>();
        if (impl_->arena->generation != frame.generation || impl_->arena->width != frame.width || impl_->arena->height != frame.height) {
            impl_->arena->images.clear(); impl_->arena->depths.clear(); impl_->arena->bytes = 0;
            impl_->arena->generation = frame.generation; impl_->arena->width = frame.width; impl_->arena->height = frame.height;
        }
        impl_->state.allocatedFrameBytes = impl_->arena->bytes; impl_->firstSupportDraw = impl_->gpu->drawCount();
        impl_->state.active = impl_->state.cleanComplete = impl_->state.occlusionComplete = impl_->state.influenceComplete = true; return true;
    } catch (const std::exception& error) { impl_->externalFailure(error.what()); return false; } catch (...) { impl_->externalFailure("Begin capture frame failed"); return false; }
}
bool CaptureOwner::declareTexture(const CaptureTexture& texture) noexcept {
    try { auto lock = impl_->graphics->lock(); impl_->declare(texture); return true; }
    catch (const std::exception& error) { impl_->externalFailure(error.what()); return false; } catch (...) { impl_->externalFailure("Declare capture texture failed"); return false; }
}
void CaptureOwner::invalidateTexture(const CaptureTexture& texture) noexcept {
    try { auto lock = impl_->graphics->lock(); if (auto* record = impl_->find(texture.texture.Get()); record && record->epoch == texture.epoch) impl_->invalidate(*record); }
    catch (...) { impl_->externalFailure("Invalidate capture texture failed"); }
}
bool CaptureOwner::seedCleanRoot(const CaptureTexture& texture) noexcept {
    try {
        auto lock = impl_->graphics->lock(); Bypass bypass;
        auto& record = impl_->declare(texture); impl_->invalidate(record);
        record.clean = impl_->color(texture.texture.Get()); impl_->context->CopyResource(record.clean->texture.Get(), texture.texture.Get());
        record.transmittance = impl_->plane(*record.clean, 1, DXGI_FORMAT_R32_FLOAT);
        record.influence = impl_->plane(*record.clean, 0, DXGI_FORMAT_R16_FLOAT); return true;
    } catch (const std::exception& error) { impl_->externalFailure(error.what()); return false; } catch (...) { impl_->externalFailure("Seed clean root failed"); return false; }
}
bool CaptureOwner::bindCleanInput(const CaptureTexture& full, const CaptureTexture& previous) noexcept {
    try {
        auto lock = impl_->graphics->lock(); Bypass bypass;
        auto& source = impl_->known(previous); impl_->flushUi(source);
        if (!full.texture || !full.epoch) throw std::invalid_argument("Camera handoff requires a current target epoch");
        if (impl_->find(full.texture.Get()) == &source) {
            // A depth-only camera may reuse the same color allocation. The
            // explicit handoff transfers its value to the new camera epoch;
            // declaring first would invalidate the very planes being handed off.
            source.epoch = full.epoch; return true;
        }
        auto& destination = impl_->declare(full);
        auto clean = impl_->color(full.texture.Get());
        if (clean->width != source.clean->width || clean->height != source.clean->height || clean->format != source.clean->format)
            throw std::invalid_argument("Camera clean handoff needs matching native color dimensions/format");
        auto t = impl_->allocate(clean->width, clean->height, DXGI_FORMAT_R32_FLOAT);
        auto m = impl_->allocate(clean->width, clean->height, DXGI_FORMAT_R16_FLOAT);
        impl_->context->CopyResource(clean->texture.Get(), source.clean->texture.Get());
        impl_->context->CopyResource(t->texture.Get(), source.transmittance->texture.Get()); impl_->context->CopyResource(m->texture.Get(), source.influence->texture.Get());
        impl_->invalidate(destination); destination.clean = std::move(clean); destination.transmittance = std::move(t); destination.influence = std::move(m);
        destination.constantT = source.constantT; ++impl_->state.mirroredTransfers; return true;
    } catch (const std::exception& error) { impl_->externalFailure(error.what()); return false; } catch (...) { impl_->externalFailure("Bind clean camera input failed"); return false; }
}
bool CaptureOwner::beginScope(const CaptureScope& scope) noexcept {
    try {
        auto lock = impl_->graphics->lock();
        if (!impl_->state.active || impl_->scope || !scope.passId) throw std::runtime_error("Invalid or nested capture scope");
        if (scope.implicitOutputEpoch && (scope.kind != CaptureScopeKind::DualColor || !scope.fullOverwrite))
            throw std::invalid_argument("Implicit draw target requires a trusted full-overwrite color scope");
        impl_->scopeOutput = {}; impl_->scope = scope; impl_->depthCopies.clear(); impl_->supplementalDepthCopies.clear(); return true;
    } catch (const std::exception& error) { impl_->externalFailure(error.what()); return false; } catch (...) { impl_->externalFailure("Begin capture scope failed"); return false; }
}
bool CaptureOwner::endScope() noexcept {
    try {
        auto lock = impl_->graphics->lock(); Bypass bypass;
        if (!impl_->scope) throw std::runtime_error("Capture scope end has no matching begin");
        impl_->flushAllUi(); impl_->scope.reset(); impl_->depthCopies.clear(); impl_->supplementalDepthCopies.clear(); return true;
    } catch (const std::exception& error) { impl_->externalFailure(error.what()); impl_->scope.reset(); return false; }
    catch (...) { impl_->externalFailure("End capture scope failed"); impl_->scope.reset(); return false; }
}
CaptureTexture CaptureOwner::lastScopeOutput() const {
    auto lock = impl_->graphics->lock();
    if (!impl_->arena || !impl_->state.active || !impl_->state.cleanComplete || impl_->scope) return {};
    const auto* record = impl_->find(impl_->scopeOutput.texture.Get());
    if (!record || record->epoch != impl_->scopeOutput.epoch || !record->clean) return {};
    return impl_->scopeOutput;
}
CaptureTexture CaptureOwner::currentColor(ID3D11Texture2D* texture) const {
    auto lock = impl_->graphics->lock();
    if (!texture || !impl_->arena || !impl_->state.active || !impl_->state.cleanComplete) return {};
    const auto* record = impl_->find(texture);
    if (!record || !record->clean || !record->transmittance || !record->influence) return {};
    return {record->original,record->epoch};
}
CapturedUiLease CaptureOwner::seal(const CaptureTexture& finalColor) noexcept {
    try {
        auto lock = impl_->graphics->lock(); Bypass bypass;
        if (!impl_->state.active || !impl_->arena) throw std::runtime_error("Seal has no active capture frame");
        if (impl_->scope) throw std::runtime_error("Capture frame ended inside a pass scope");
        if (!impl_->activeQueries.empty()) throw std::runtime_error("Capture seal overlaps a raster-dependent query");
        auto& record = impl_->known(finalColor); impl_->flushUi(record);
        if (record.clean->width != impl_->frame.width || record.clean->height != impl_->frame.height)
            throw std::runtime_error("Final capture color does not match the declared output extent");
        auto result = std::make_shared<CapturedUiFrame>(); result->frame = impl_->frame;
        if (impl_->state.cleanComplete) result->clean = record.clean->texture;
        if (impl_->state.occlusionComplete) {
            auto a = impl_->allocate(record.clean->width, record.clean->height, DXGI_FORMAT_R32_FLOAT);
            impl_->gpu->opacity(*record.transmittance, *a); ++impl_->state.supportPasses; result->occlusion = a->texture;
        }
        if (impl_->state.influenceComplete) result->influence = record.influence->texture;
        result->storage = impl_->arena; result->readyFence = impl_->fence; result->readyValue = impl_->nextFence+1;
        impl_->state.active = false; impl_->state.supportPasses = impl_->gpu->drawCount()-impl_->firstSupportDraw; result->status = impl_->state;
        impl_->retire(result); return result;
    } catch (const std::exception& error) { impl_->externalFailure(error.what()); abort(error.what()); return {}; }
    catch (...) { impl_->externalFailure("Seal capture frame failed"); abort("Seal capture frame failed"); return {}; }
}
void CaptureOwner::abort(const char* reason) noexcept {
    try { auto lock = impl_->graphics->lock(); Bypass bypass; impl_->externalFailure(reason); impl_->retire(); impl_->state.active = false; }
    catch (...) { impl_->externalFailure("Capture abort could not prove GPU retirement"); impl_->quarantine(); }
}
CaptureStatus CaptureOwner::status() const {
    auto lock = impl_->graphics->lock(); auto state = impl_->state;
    state.supportPasses = impl_->gpu->drawCount()-impl_->firstSupportDraw; return state;
}
bool CaptureOwner::privateRasterAllowed() const {
    auto lock = impl_->graphics->lock();
    return !impl_->stopped && !impl_->state.quarantined && impl_->activeQueries.empty();
}
bool CaptureOwner::releaseApplicationReferences() noexcept { return !impl_ || impl_->releaseApplicationReferences(); }
bool CaptureOwner::stop() noexcept { return !impl_ || impl_->stop(); }
} // namespace dspaa
