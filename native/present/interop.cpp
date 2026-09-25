#include "channel.h"
#include <cstring>
static_assert(sizeof(DspAaPresentationConfiguration) == 32);
static_assert(sizeof(DspAaPresentationBegin) == 24);
static_assert(sizeof(DspAaPresentationInputs) == 464);
static_assert(sizeof(DspAaPresentationStatus) == 472);
uint32_t __cdecl DspAaGetPresentationAbiVersion() { return 1; }
int __cdecl DspAaConfigurePresentation(const DspAaPresentationConfiguration* value) {
    try { auto channel = dspaa::presentationChannel(); return value && value->size == sizeof(*value) && value->version == 1 && channel && channel->configure(*value); }
    catch (...) { return 0; }
}
int __cdecl DspAaBeginPresentationFrame(DspAaPresentationBegin* value) {
    try { auto channel = dspaa::presentationChannel(); return value && value->size == sizeof(*value) && value->version == 1 && channel && channel->begin(*value); }
    catch (...) { return 0; }
}
int __cdecl DspAaPresentationSimulationEnd(uint64_t id) {
    try { auto channel = dspaa::presentationChannel(); return channel && channel->marker(id, dspaa::SlMarker::SimulationEnd); }
    catch (...) { return 0; }
}
void* __cdecl DspAaQueuePresentationInputs(const DspAaPresentationInputs* value) { return dspaa::queuePresentationInputs(value); }
void __cdecl DspAaCancelPresentationInputs(void* token) { dspaa::cancelPresentationInputs(token); }
DspAaRenderEvent __cdecl DspAaGetPresentationRenderEvent() { return dspaa::presentationRenderEvent(); }
int __cdecl DspAaGetPresentationStatus(DspAaPresentationStatus* value) {
    try {
        if (!value || value->size != sizeof(*value)) return 0;
        if (auto channel = dspaa::presentationChannel()) *value = channel->status();
        else { *value = {}; value->size = sizeof(*value); value->version = 1;
            strncpy_s(value->message, "Frame generation requires the explicitly installed early presentation bootstrap and a game restart", _TRUNCATE); }
        return 1;
    } catch (...) { return 0; }
}
static_assert(sizeof(DspAaCaptureTexture) == 16);
static_assert(sizeof(DspAaCaptureDomain) == 40);
static_assert(sizeof(DspAaCaptureInput) == 16);
static_assert(sizeof(DspAaCaptureCommand) == 128);
static_assert(sizeof(DspAaCaptureStatus) == 560);
static_assert(sizeof(DspAaDepthCopyResult) == 40);
void* __cdecl DspAaQueueCaptureCommand(const DspAaCaptureCommand* value,const DspAaCaptureInput* inputs,
    const DspAaCaptureDomain* domains,const char* basis) { return dspaa::queueCaptureCommand(value,inputs,domains,basis); }
void __cdecl DspAaCancelCaptureCommand(void* token) { dspaa::cancelCaptureCommand(token); }
int __cdecl DspAaGetCaptureStatus(DspAaCaptureStatus* value) {
    try {
        if(!value||value->size!=sizeof(*value))return 0;
        if(auto channel=dspaa::presentationChannel())if(auto capture=channel->capture()){*value=capture->status();return 1;}
        *value={};value->size=sizeof(*value);value->version=1;
        strncpy_s(value->reason,"Early game/UI capture is unavailable; ordinary presentation and SR remain independent",_TRUNCATE);return 1;
    }catch(...){return 0;}
}
int __cdecl DspAaGetDepthCopyResult(DspAaDepthCopyResult* value) {
    try {
        if(!value||value->size!=sizeof(*value))return 0;
        if(auto channel=dspaa::presentationChannel())if(auto capture=channel->capture()){*value=capture->depthCopyResult();return 1;}
        *value={sizeof(*value),1};return 1;
    }catch(...){return 0;}
}
