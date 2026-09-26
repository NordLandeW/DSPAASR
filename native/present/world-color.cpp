#include "world-color.h"
#include "channel.h"
#include "core/performance.h"
#include "graphics/predication.h"
#include <stdexcept>

namespace dspaa {
namespace {
DXGI_FORMAT colorStorage(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return format;
    }
}
} // namespace
void WorldColor::surface(ID3D11Texture2D* source, uint64_t generation) {
    auto access = bridge_->lock();
    if (generation != generation_) {
        pending_.reset();
        slots_ = {};
        frame_ = 0;
    }
    source_ = source;
    generation_ = generation;
}
void WorldColor::releaseSurfaceReference() {
    auto access = bridge_->lock();
    source_.Reset();
}
void WorldColor::capture(uint64_t frame) {
    DSPAA_PERF_SCOPE(WorldCapture);
    auto access = bridge_->lock();
    if (!source_ || !frame || frame <= frame_)
        return;
    frame_ = frame;
    pending_.reset();
    // Unity can render its current frame into an intermediate display target.
    // The display swapchain surface is not updated until its later final blit.
    // Observe the actual color attachment at the ordered world-camera boundary.
    auto* context = bridge_->context11();
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (!view)
        return;
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    if (viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || viewDesc.Texture2D.MipSlice)
        return;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> current;
    if (!resource || FAILED(resource.As(&current)))
        return;
    D3D11_TEXTURE2D_DESC desc{}, expected{};
    current->GetDesc(&desc);
    source_->GetDesc(&expected);
    if (!desc.Width || !desc.Height || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
        colorStorage(desc.Format) != colorStorage(expected.Format) ||
        colorStorage(viewDesc.Format) != colorStorage(expected.Format))
        return;
    // Copy-compatible typed/typeless/sRGB views preserve storage bytes, without
    // a shader decode/encode round trip. SDK inputs use the presentation format.
    desc.Format = expected.Format;
    std::shared_ptr<WorldColorImage>* free = nullptr;
    for (auto& slot : slots_)
        if (!slot || (slot.use_count() == 1 && slot->ready.complete())) {
            free = &slot;
            break;
        }
    if (!free)
        return; // An unavailable input pauses FG, never original rendering.
    if (!*free)
        *free = std::make_shared<WorldColorImage>();
    auto& image = **free;
    image.srgbView = viewDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                     viewDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (image.texture.dx11) {
        D3D11_TEXTURE2D_DESC prior{};
        image.texture.dx11->GetDesc(&prior);
        if (prior.Width != desc.Width || prior.Height != desc.Height || prior.Format != desc.Format)
            image.texture = {};
    }
    if (!image.texture.dx11)
        image.texture = bridge_->texture(desc.Width, desc.Height, desc.Format, false);
    image.ready = {};
    UnpredicatedCopy unconditional(context);
    context->CopyResource(image.texture.dx11.Get(), current.Get());
    // Exact world-target bytes: no guessed gamma/PQ conversion or second PP chain.
    pending_ = *free;
}
void WorldColor::attach(PresentationSubmission& submission) {
    auto access = bridge_->lock();
    if (submission.resources[0])
        return; // Explicit producer input, e.g. a GPU probe.
    if (!pending_ || frame_ != submission.metadata.applicationFrameId ||
        generation_ != submission.metadata.generation) {
        submission.metadata.flags &= ~1u;
        return;
    }
    D3D11_TEXTURE2D_DESC captured{}, display{};
    pending_->texture.dx11->GetDesc(&captured);
    if (!source_) {
        submission.metadata.flags &= ~1u;
        return;
    }
    source_->GetDesc(&display);
    const auto& input = submission.metadata;
    const auto width = input.logicalOutputWidth ? input.logicalOutputWidth : display.Width;
    const auto height = input.logicalOutputHeight ? input.logicalOutputHeight : display.Height;
    if ((input.logicalOutputWidth == 0) != (input.logicalOutputHeight == 0) || captured.Width != width ||
        captured.Height != height) {
        submission.metadata.flags &= ~1u;
        return;
    }
    submission.hudlessSrgbView = pending_->srgbView;
    submission.resources[0] = pending_->texture.dx11;
    submission.metadata.hudless = pending_->texture.dx11.Get();
    submission.worldLifetime = std::move(pending_);
    // The lease covers backend consumption; the image's independent fence/value
    // additionally protects pending producer work if the frame is released early.
}
} // namespace dspaa
