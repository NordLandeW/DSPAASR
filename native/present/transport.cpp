#include "transport.h"
#include <algorithm>
#include <stdexcept>

namespace dspaa {
namespace {
DXGI_FORMAT typed(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    default: return format;
    }
}
D3D11_TEXTURE2D_DESC describe(ID3D11Resource* resource, ID3D11Device* expected) {
    if (!resource) throw std::invalid_argument("Missing presentation input");
    Microsoft::WRL::ComPtr<ID3D11Device> device; resource->GetDevice(&device);
    if (device.Get() != expected) throw std::invalid_argument("Presentation inputs belong to another device");
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) throw std::invalid_argument("Presentation input is not a 2D texture");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality) throw std::invalid_argument("Presentation input requires one single-sample 2D subresource");
    return desc;
}
bool alpha(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8_UNORM || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R32_FLOAT; }
void metadata(PresentationFrame& target, const DspAaPresentationInputs& source) {
    target.applicationFrameId = source.applicationFrameId;
    target.reset = (source.flags & 2u) != 0; target.depthInverted = (source.flags & 4u) != 0;
    target.depthInfinite = (source.flags & 8u) != 0; target.cameraMotionIncluded = (source.flags & 16u) != 0;
    target.motionVectorsJittered = (source.flags & 32u) != 0;
    target.renderWidth = source.renderWidth; target.renderHeight = source.renderHeight;
    target.jitterX = source.jitterX; target.jitterY = source.jitterY;
    target.motionScaleX = source.motionScaleX; target.motionScaleY = source.motionScaleY;
    target.deltaMilliseconds = source.milliseconds; target.cameraNear = source.cameraNear; target.cameraFar = source.cameraFar;
    target.verticalFov = source.verticalFov; target.preExposure = source.preExposure; target.viewSpaceToMeters = source.viewSpaceToMeters;
    target.minLuminance = source.minLuminance; target.maxLuminance = source.maxLuminance;
    std::copy_n(source.cameraPosition, 3, target.cameraPosition.begin()); std::copy_n(source.cameraUp, 3, target.cameraUp.begin());
    std::copy_n(source.cameraRight, 3, target.cameraRight.begin()); std::copy_n(source.cameraForward, 3, target.cameraForward.begin());
    std::copy_n(source.cameraViewToClip, 16, target.cameraViewToClip.begin()); std::copy_n(source.clipToCameraView, 16, target.clipToCameraView.begin());
    std::copy_n(source.clipToPrevClip, 16, target.clipToPrevClip.begin()); std::copy_n(source.prevClipToClip, 16, target.prevClipToClip.begin());
    target.generationRect = {source.generationRect[0], source.generationRect[1], source.generationRect[2], source.generationRect[3]};
}
} // namespace
void PresentationTransport::copy(Slot& slot, size_t index, ID3D11Resource* input, DXGI_FORMAT format, PresentImage& output) {
    const auto desc = describe(input, bridge_->device11());
    auto& shared = slot.textures[index];
    if (shared.dx12) {
        const auto prior = shared.dx12->GetDesc();
        if (prior.Width != desc.Width || prior.Height != desc.Height || prior.Format != format) shared = {};
    }
    if (!shared.dx12) shared = bridge_->texture(desc.Width, desc.Height, format, false);
    bridge_->context11()->CopyResource(shared.dx11.Get(), input);
    output.resource = shared.dx12; output.state = D3D12_RESOURCE_STATE_COMMON;
}
FrameLease PresentationTransport::capture(ID3D11Texture2D* finalColor, uint64_t generation, DXGI_COLOR_SPACE_TYPE colorSpace,
    std::unique_ptr<PresentationSubmission> submission, std::string& reason) {
    auto access = bridge_->lock();
    Slot* free = nullptr;
    for (auto& candidate : slots_) if (!candidate.images || candidate.images.use_count() == 1) { free = &candidate; break; }
    if (!free) throw std::runtime_error("All presentation producer slots remain leased");
    auto& slot = *free;
    if (!slot.images) slot.images = std::make_shared<FrameImages>();
    // A reused pool object has no outstanding consumers, but its optional image
    // fields must not leak previous-frame inputs into an incomplete envelope.
    *slot.images = {};
    auto frame = std::make_shared<PresentationFrame>();
    frame->generation = generation; frame->colorSpace = colorSpace; frame->images = slot.images;
    const auto final = describe(finalColor, bridge_->device11());
    copy(slot, 0, finalColor, final.Format, slot.images->finalColor);
    if (submission) {
        metadata(*frame, submission->metadata);
        const auto& source = submission->metadata;
        if (source.generation != generation) reason = "Temporal inputs belong to an earlier presentation surface";
        else if (!(source.flags & 1u)) reason = submission->incompleteReason.empty() ? "Game/UI adapter has not supplied a complete temporal frame" : submission->incompleteReason;
        else {
            try {
                std::array<DXGI_FORMAT, 7> formats{};
                // Validate every source before recording any optional copies.
                for (size_t i = 0; i < formats.size(); ++i) {
                    if (i >= 5 && !submission->resources[i]) continue;
                    const auto desc = describe(submission->resources[i].Get(), bridge_->device11());
                    const auto width = (i == 1 || i == 2) ? source.renderWidth : final.Width;
                    const auto height = (i == 1 || i == 2) ? source.renderHeight : final.Height;
                    if (desc.Width != width || desc.Height != height) throw std::invalid_argument("Temporal input dimensions do not match their frame domain");
                    formats[i] = typed(desc.Format);
                    if ((i == 1 && formats[i] != DXGI_FORMAT_R32_FLOAT) || (i == 2 && formats[i] != DXGI_FORMAT_R16G16_FLOAT) ||
                        ((i == 3 || i == 4) && !alpha(formats[i])) || (i == 5 && formats[i] != DXGI_FORMAT_R16G16_FLOAT) ||
                        (i == 6 && formats[i] != DXGI_FORMAT_R16G16B16A16_FLOAT))
                        throw std::invalid_argument("Temporal input has the wrong typed format for its role");
                }
                PresentImage* images[] = {&slot.images->hudless, &slot.images->depth, &slot.images->motion,
                    &slot.images->occlusionAlpha, &slot.images->uiInfluence, &slot.images->fsrDistortion, &slot.images->slDistortion};
                for (size_t i = 0; i < std::size(images); ++i)
                    if (submission->resources[i]) copy(slot, i + 1, submission->resources[i].Get(), formats[i], *images[i]);
                frame->inputsComplete = true;
            } catch (const std::invalid_argument& error) { reason = error.what(); }
        }
        frame->producerLifetime = std::shared_ptr<PresentationSubmission>(std::move(submission));
    } else reason = "No end-of-frame envelope; preserving the real Final without inventing a simulation token";
    if (!frame->inputsComplete) frame->reset = true;
    bridge_->handoffTo12(); frame->readyValue = bridge_->handoffTo11(); frame->readyFence = bridge_->completionFence12();
    return frame;
}
} // namespace dspaa
