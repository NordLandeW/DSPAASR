#include "capture.h"
#include "capture-trace.h"
#include "channel.h"
#include "scene-depth.h"
#include "ui-proof/guard.h"
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
CaptureSampleDomain domain(const DspAaCaptureDomain& source) {
    CaptureSampleDomain value;
    std::copy_n(source.matrix,4,value.matrix.begin());std::copy_n(source.bias,2,value.bias.begin());
    std::copy_n(source.offsetTexels,2,value.offsetTexels.begin());std::copy_n(source.radiusTexels,2,value.radiusTexels.begin());
    for(float v:value.matrix)if(!std::isfinite(v))throw std::invalid_argument("Nonfinite capture sampling matrix");
    for(float v:value.bias)if(!std::isfinite(v))throw std::invalid_argument("Nonfinite capture sampling bias");
    for(float v:value.offsetTexels)if(!std::isfinite(v))throw std::invalid_argument("Nonfinite capture sampling offset");
    for(unsigned v:value.radiusTexels)if(v>16384)throw std::invalid_argument("Capture support radius exceeds the texture domain");
    return value;
}
CaptureTexture texture(const DspAaCaptureTexture& source) {
    CaptureTexture value;value.epoch=source.epoch;
    if(source.resource)graphicsCheck(static_cast<ID3D11Resource*>(source.resource)->QueryInterface(IID_PPV_ARGS(&value.texture)),"Capture command resource is not a 2D texture");
    return value;
}
void reason(DspAaCaptureStatus& status,const char* text){strncpy_s(status.reason,text?text:"",_TRUNCATE);}
}
std::unique_ptr<CaptureCommand> copyCaptureCommand(const DspAaCaptureCommand* source,
    const DspAaCaptureInput* inputs,const DspAaCaptureDomain* domains,const char* basis) {
    if(!source||source->size!=sizeof(*source)||source->version!=1||source->operation>11||source->scope>4||
        !source->applicationFrameId||!source->generation||(source->flags&~31u)||source->inputCount>8||source->domainCount>128||
        (source->inputCount&&!inputs)||(source->domainCount&&!domains)||source->occlusionSlot < -1||source->occlusionSlot>=128)
        return {};
    if (source->flags & 8u) {
        const bool begin = (source->operation == 4 || source->operation == 10) && source->scope == static_cast<uint32_t>(CaptureScopeKind::DualColor) &&
            source->flags == 10 && !source->source.resource && source->source.epoch;
        const bool remember = source->operation == 7 && source->flags == 8 && !source->source.epoch;
        if (!begin && !remember) return {};
    }
    if ((source->flags & 16u) && (source->operation != 3 || source->flags != 21 || source->source.resource || !source->source.epoch)) return {};
    if (source->operation == 10 && (source->flags != 10 || !source->passId)) return {};
    if (source->operation == 11 && (source->flags || source->inputCount || source->domainCount)) return {};
    auto result=std::make_unique<CaptureCommand>();result->metadata=*source;
    result->source=texture(source->source);result->previous=texture(source->previous);
    if(basis){const auto length=strnlen_s(basis,384);if(length>=384)return {};result->basis.assign(basis,length);}
    auto& scope=result->scope;scope.kind=static_cast<CaptureScopeKind>(source->scope);scope.passId=source->passId;
    scope.fullOverwrite=(source->flags&2)!=0;scope.support.basis=result->basis;
    if ((source->operation == 4 || source->operation == 10) && (source->flags & 8u)) scope.implicitOutputEpoch = source->source.epoch;
    scope.support.occlusionResourceSlot=source->occlusionSlot;scope.support.occlusionDomain=domain(source->occlusionDomain);
    for(unsigned i=0;i<source->inputCount;++i){
        const auto& input=inputs[i];
        if(input.resourceSlot>=128||input.samplerSlot>=16||!input.domainCount||input.domainCount>64||input.firstDomain>source->domainCount||
            input.domainCount>source->domainCount-input.firstDomain)return {};
        CaptureSamplingInput target;target.pixelResourceSlot=input.resourceSlot;target.pixelSamplerSlot=input.samplerSlot;
        for(unsigned j=0;j<input.domainCount;++j)target.domains.push_back(domain(domains[input.firstDomain+j]));
        scope.support.inputs.push_back(std::move(target));
    }
    return result;
}
struct FrameCapture::Impl {
    std::shared_ptr<Dx11Dx12> graphics;
    PresentationLog log;
    std::unique_ptr<UiShaderProof> proof;
    std::unique_ptr<CaptureOwner> owner;
    std::unique_ptr<SceneDepth> sceneDepth;
    std::unique_ptr<CaptureTrace> trace;
    CaptureTexture shadow,lastColor;
    ComPtr<ID3D11Fence> unityFence;
    uint64_t generation=0,frame=0,lastSignal=0;
    bool enabled=false,stopped=false,quarantined=false,commandsReceived=false;
    bool followingTransfers=false;
    DspAaCaptureStatus observation{};
    DspAaDepthCopyResult depthResult{sizeof(DspAaDepthCopyResult), 1};
    unsigned traceCount = 0;
    uint32_t lastOperation = 0;
    uint64_t lastPass = 0;
    std::set<std::tuple<uint32_t, uint64_t, std::string, std::string>> unannotatedDiagnostics;
    Impl(std::shared_ptr<Dx11Dx12> bridge, PresentationLog output)
        : graphics(std::move(bridge)), log(output) {
        if (!graphics)
            throw std::invalid_argument("Capture pipeline needs the facade's graphics owner");
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("Initialize private capture detours");
        proof = std::make_unique<UiShaderProof>(graphics, log);
        CaptureOwnerCreateInfo info;
        info.graphics = graphics;
        info.uiShaderPolicy = [this](ID3D11PixelShader* shader) { return proof->inspect(shader); };
        info.effectShaderPolicy = [this](ID3D11PixelShader* shader, const CaptureScope& scope,
                                         const capture::DrawArguments& arguments, CaptureSupport& support) {
            return proof->inspectEffect(shader, scope, arguments, support);
        };
        char diagnostic[2]{};
        if (GetEnvironmentVariableA("DSPAASR_CAPTURE_TRACE_WRITES", diagnostic, sizeof(diagnostic)) == 1 &&
            diagnostic[0] == '1')
            info.unannotatedDraw = [this](ID3D11RenderTargetView* target) { traceUnannotated(target); };
        owner = std::make_unique<CaptureOwner>(info);
        ComPtr<ID3D11Device5> device;
        graphicsCheck(graphics->device11()->QueryInterface(IID_PPV_ARGS(&device)),
                      "Query Unity snapshot fence device");
        graphicsCheck(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&unityFence)),
                      "Create Unity snapshot retirement fence");
        observation.size = sizeof(observation);
        observation.version = 1;
        observation.flags = 1;
    }
    void traceUnannotated(ID3D11RenderTargetView* target) {
        constexpr size_t maximumDiagnostics = 16;
        if (!log || unannotatedDiagnostics.size() >= maximumDiagnostics)
            return;
        auto* context = graphics->context11();
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11VertexShader> vertex;
        context->PSGetShader(&pixel, nullptr, nullptr);
        context->VSGetShader(&vertex, nullptr, nullptr);
        const auto ps = UiShaderProof::shaderFingerprint(pixel.Get()),
                   vs = UiShaderProof::shaderFingerprint(vertex.Get());
        if (!unannotatedDiagnostics.emplace(lastOperation, lastPass, ps, vs).second)
            return;
        ComPtr<ID3D11Resource> output, input;
        ComPtr<ID3D11ShaderResourceView> srv;
        target->GetResource(&output);
        context->PSGetShaderResources(0, 1, &srv);
        if (srv)
            srv->GetResource(&input);
        ComPtr<ID3D11Texture2D> image;
        D3D11_TEXTURE2D_DESC description{};
        if (SUCCEEDED(output.As(&image)))
            image->GetDesc(&description);
        D3D11_VIEWPORT viewport{};
        UINT count = 1;
        context->RSGetViewports(&count, &viewport);
        char text[768];
        std::snprintf(text, sizeof(text),
                      "capture.unannotated frame=%llu generation=%llu lastOperation=%u lastPass=0x%llX "
                      "output=%p size=%ux%u format=%u input0=%p viewport=%.9g,%.9g,%.9g,%.9g PS=%s VS=%s",
                      static_cast<unsigned long long>(frame), static_cast<unsigned long long>(generation),
                      lastOperation, static_cast<unsigned long long>(lastPass), output.Get(),
                      description.Width, description.Height, description.Format, input.Get(),
                      viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height, ps.c_str(),
                      vs.c_str());
        log(text);
    }
    CaptureTexture resolve(const CaptureTexture& value,bool bound) {
        if(bound){
            ComPtr<ID3D11RenderTargetView> target;graphics->context11()->OMGetRenderTargets(1,&target,nullptr);
            if(!target)throw std::runtime_error("Ordered capture event has no bound color RTV");
            D3D11_RENDER_TARGET_VIEW_DESC view{};target->GetDesc(&view);
            if(view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||view.Texture2D.MipSlice)
                throw std::runtime_error("Ordered capture RTV has an unsupported mip/array/sample domain");
            ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
            CaptureTexture result;result.epoch=value.epoch;
            graphicsCheck(resource.As(&result.texture),"Query bound capture texture");
            if(!result.epoch)throw std::runtime_error("Bound capture target lacks a camera/allocation epoch");
            return result;
        }
        if(value.texture){if(!value.epoch)throw std::runtime_error("Capture resource has no allocation epoch");return value;}
        return shadow;
    }
    bool endTransfers() {
        if (!followingTransfers) return true;
        followingTransfers=false;
        return owner->endScope();
    }
    void snapshot() {
        const auto value=owner->status();const auto verified=proof->status();
        observation.flags=1u|(value.active?2u:0u)|
            (value.cleanComplete&&value.occlusionComplete&&value.influenceComplete?4u:0u)|(value.quarantined||quarantined?8u:0u);
        observation.applicationFrameId=frame;observation.generation=generation;
        observation.draws=value.observedDraws;observation.colorReplays=value.colorReplays;observation.coverageReplays=value.coverageReplays;
        observation.supportPasses=value.supportPasses;observation.privateBytes=value.allocatedFrameBytes;
        observation.proofInspected=verified.inspected;observation.proofAccepted=verified.accepted;observation.copiedConstantBytes=verified.copiedConstantBytes;
        if(!value.reason.empty())reason(observation,value.reason.c_str());
        if(!verified.reason.empty()&&!value.influenceComplete){
            const auto combined=value.reason+" | "+verified.reason;reason(observation,combined.c_str());
        }
        strncpy_s(observation.pixelShaderHash,verified.shaderHash.c_str(),_TRUNCATE);
    }
};
FrameCapture::FrameCapture(std::shared_ptr<Dx11Dx12> graphics,PresentationLog log):impl_(std::make_unique<Impl>(std::move(graphics),log)){}
FrameCapture::~FrameCapture(){if(!stop())(void)impl_.release();}
void FrameCapture::surface(ID3D11Texture2D* shadow,uint64_t generation) {
    auto lock=impl_->graphics->lock();impl_->shadow.texture=shadow;impl_->shadow.epoch=generation;impl_->generation=generation;
}
bool FrameCapture::prepareSurfaceChange() noexcept {
    try {
        auto& s=*impl_;auto lock=s.graphics->lock();
        s.owner->abort("Presentation surface is changing");s.enabled=false;
        s.graphics->drain();
        if(!s.owner->releaseApplicationReferences())return false;
        s.lastColor={};s.shadow={};s.followingTransfers=false;
        s.depthResult={sizeof(DspAaDepthCopyResult),1};return true;
    }catch(const std::exception& error){reason(impl_->observation,error.what());return false;}catch(...){return false;}
}
void FrameCapture::begin(uint64_t frame,bool enabled) noexcept {
    try {
        auto& s=*impl_;auto lock=s.graphics->lock();if(s.stopped||s.quarantined)return;
        if(s.trace)s.trace->collect();
        if(s.enabled)s.owner->abort("Capture frame was superseded before its real end-of-frame event");
        s.frame=frame;s.enabled=enabled;s.commandsReceived=false;s.followingTransfers=false;s.lastColor={};reason(s.observation,"");
        s.proof->beginFrame();
        s.depthResult={sizeof(DspAaDepthCopyResult),1};
        if(!enabled)return;
        D3D11_TEXTURE2D_DESC desc{};s.shadow.texture->GetDesc(&desc);
        if(!s.owner->beginFrame({frame,s.generation,desc.Width,desc.Height})||!s.owner->declareTexture(s.shadow))s.enabled=false;
        s.snapshot();
    }catch(const std::exception& e){reason(impl_->observation,e.what());impl_->enabled=false;}catch(...){impl_->enabled=false;}
}
void FrameCapture::execute(const CaptureCommand& command) noexcept {
    try {
        auto& s=*impl_;auto lock=s.graphics->lock();const auto& m=command.metadata;
        if(s.stopped||m.applicationFrameId!=s.frame||m.generation!=s.generation||(!s.enabled&&m.operation!=9))return;
        s.lastOperation = m.operation;
        s.lastPass = m.passId;
        if(m.operation==8){
            // Diagnostics do not establish any input dependency. A camera event
            // without an RTV is useful evidence, not a reason to cancel later observations.
            try {
                const auto source=s.resolve(command.source,(m.flags&1)!=0);
                D3D11_TEXTURE2D_DESC d{};source.texture->GetDesc(&d);
                if(s.log&&s.traceCount<64){
                    ++s.traceCount;
                    ComPtr<ID3D11RenderTargetView> bound;ComPtr<ID3D11DepthStencilView> depthView;
                    s.graphics->context11()->OMGetRenderTargets(1,&bound,&depthView);
                    D3D11_RENDER_TARGET_VIEW_DESC view{};if(bound)bound->GetDesc(&view);
                    ComPtr<ID3D11Texture2D> depthTexture;D3D11_TEXTURE2D_DESC depthDesc{};D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                    if(depthView){ComPtr<ID3D11Resource> resource;depthView->GetResource(&resource);resource.As(&depthTexture);
                        depthView->GetDesc(&dsvDesc);if(depthTexture)depthTexture->GetDesc(&depthDesc);}
                    if(m.flags&2u){ // Explicit diagnostic readback, never ordinary capture.
                        if(!s.trace)s.trace=std::make_unique<CaptureTrace>(s.graphics,s.log);
                        s.trace->observe(m.applicationFrameId,m.generation,m.passId,bound.Get());
                    }
                    char text[512];std::snprintf(text,sizeof(text),"capture.binding pass=%llu frame=%llu generation=%llu texture=%p epoch=%llu %ux%u format=%u rtvFormat=%u samples=%u bind=0x%x depth=%p depthSize=%ux%u depthFormat=%u dsvFormat=%u depthSamples=%u depthBind=0x%x",
                        static_cast<unsigned long long>(m.passId),static_cast<unsigned long long>(m.applicationFrameId),
                        static_cast<unsigned long long>(m.generation),source.texture.Get(),static_cast<unsigned long long>(source.epoch),d.Width,d.Height,d.Format,view.Format,d.SampleDesc.Count,d.BindFlags,
                        depthTexture.Get(),depthDesc.Width,depthDesc.Height,depthDesc.Format,dsvDesc.Format,depthDesc.SampleDesc.Count,depthDesc.BindFlags);s.log(text);
                }
            }catch(const std::exception& error){
                if(s.log&&s.traceCount<64){
                    ++s.traceCount;
                    const auto text="capture.binding pass="+std::to_string(m.passId)+" unavailable: "+error.what();s.log(text.c_str());
                }
            }
            return;
        }
        s.commandsReceived=true;
        bool good=true;
        // A camera-tail copy annotation is subordinate to every explicit pass or
        // root handoff. It never turns a nested explicit scope into a valid one.
        if ((m.operation==2 || m.operation==3 || m.operation==4 || m.operation==10 || m.operation==11) && !s.endTransfers())
            throw std::runtime_error("Could not close the preceding image-effect color transfer");
        if(m.operation==11) { /* Closing an absent camera tail is intentionally harmless. */ }
        else if(m.operation==10) { good=s.owner->beginScope(command.scope);s.followingTransfers=good; }
        else if(m.operation==5)good=s.owner->endScope();
        else if(m.operation==6){s.owner->abort(command.basis.c_str());s.enabled=false;}
        else if(m.operation==4)good=s.owner->beginScope(command.scope);
        else if(m.operation==9){
            s.depthResult={sizeof(DspAaDepthCopyResult),1,1,0,m.applicationFrameId,m.generation,m.passId};
            if(!s.owner->privateRasterAllowed())throw std::runtime_error("World UI depth copy intersects application raster queries");
            if(!s.sceneDepth)s.sceneDepth=std::make_unique<SceneDepth>(s.graphics);
            s.sceneDepth->copy(command.source.texture.Get(),command.previous.texture.Get(),
                m.occlusionDomain.bias[0],m.occlusionDomain.bias[1]);
            s.depthResult.flags|=2u;
        }
        else {
            const bool actualOutput = m.operation == 7 && (m.flags & 8u);
            const auto source = actualOutput ? s.owner->lastScopeOutput() : s.resolve(command.source,(m.flags&1)!=0);
            if (actualOutput && (!source.texture || !source.epoch ||
                (command.source.texture && command.source.texture.Get() != source.texture.Get())))
                throw std::runtime_error("Remember requires this scope's actual completed color output");
            switch(m.operation){
            case 0:good=s.owner->declareTexture(source);break;
            case 1:s.owner->invalidateTexture(source);break;
            case 2:good=s.owner->seedCleanRoot(source);s.lastColor=source;break;
            case 3: {
                // Prefer this camera target's already observed HDR value when
                // explicitly requested. A quantized remembered backbuffer cannot
                // reconstruct the higher-precision input of a later camera.
                auto previous=(m.flags&16)?s.owner->currentColor(source.texture.Get()):CaptureTexture{};
                if (!previous.texture) previous=(m.flags&4)?s.lastColor:s.resolve(command.previous,false);
                good=s.owner->bindCleanInput(source,previous);break;
            }
            case 7:s.lastColor=source;break;
            default:good=false;break;
            }
        }
        if(!good){s.owner->abort("Capture command could not establish its declared dependency");s.enabled=false;}
        s.snapshot();
    }catch(const std::exception& e){impl_->owner->abort(e.what());impl_->enabled=false;reason(impl_->observation,e.what());}
    catch(...){impl_->owner->abort("Unknown capture command failure");impl_->enabled=false;}
}
void FrameCapture::finish(PresentationSubmission& submission) noexcept {
    try {
        auto& s=*impl_;auto lock=s.graphics->lock();const auto id=submission.metadata.applicationFrameId;
        if(id!=s.frame||s.stopped){submission.metadata.flags&=~1u;submission.incompleteReason="Capture envelope does not belong to the active render frame";return;}
        const bool suppliedUi=!s.commandsReceived&&(submission.metadata.flags&1u)&&submission.resources[0]&&
            submission.resources[3]&&submission.resources[4];
        if(suppliedUi){
            // Presentation ABI also accepts independently owned complete inputs.
            // Merely installing the game capture observer must not replace those
            // with an unseeded internal arena. Transport still validates every input.
            s.owner->abort("Caller supplied independent complete UI inputs");s.enabled=false;
        }else if(s.enabled){
            if(submission.metadata.flags&1u){
                if (!s.endTransfers()) throw std::runtime_error("Final camera transfer did not close");
                const auto finalColor=s.owner->currentColor(s.shadow.texture.Get());
                auto lease=s.owner->seal(finalColor);
                if(lease&&lease->frame.applicationFrameId==id&&lease->frame.generation==submission.metadata.generation&&
                    lease->clean&&lease->occlusion&&lease->influence){
                    submission.resources[0]=lease->clean;submission.resources[3]=lease->occlusion;submission.resources[4]=lease->influence;
                    submission.captureLease=std::move(lease);
                }else submission.metadata.flags&=~1u;
            }else s.owner->abort("Game/UI adapter has not declared a complete capture frame");
            s.enabled=false;
        }else submission.metadata.flags&=~1u;
        if(id>s.lastSignal){graphicsCheck(s.graphics->context11()->Signal(s.unityFence.Get(),id),"Signal original Unity snapshot retirement");s.lastSignal=id;}
        s.snapshot();if(!(submission.metadata.flags&1u))submission.incompleteReason=s.observation.reason;
    }catch(const std::exception& e){submission.metadata.flags&=~1u;submission.incompleteReason=e.what();impl_->owner->abort(e.what());impl_->enabled=false;}
    catch(...){submission.metadata.flags&=~1u;impl_->enabled=false;}
}
DspAaCaptureStatus FrameCapture::status() const {
    auto& s=*impl_;auto lock=s.graphics->lock();auto value=s.observation;
    const auto completed=s.unityFence->GetCompletedValue();
    if(completed==std::numeric_limits<uint64_t>::max()){value.flags|=8;reason(value,"Capture completion fence reports a removed device");}
    else value.completedUnityFrame=std::min(completed,s.lastSignal);
    return value;
}
DspAaDepthCopyResult FrameCapture::depthCopyResult() const {
    auto lock=impl_->graphics->lock();return impl_->depthResult;
}
bool FrameCapture::stop() noexcept {
    if (!impl_)
        return true;
    try {
        auto& s = *impl_;
        auto lock = s.graphics->lock();
        if (s.stopped)
            return !s.quarantined;
        s.stopped = true;
        // Close both admissions even when one owner cannot prove retirement.
        const bool captureRetired = s.owner->stop();
        const bool proofRetired = !s.proof || s.proof->stop();
        if (!captureRetired || !proofRetired) {
            s.quarantined = true;
            return false;
        }
        s.proof.reset();
        return true;
    } catch (...) {
        impl_->quarantined = true;
        return false;
    }
}
} // namespace dspaa
