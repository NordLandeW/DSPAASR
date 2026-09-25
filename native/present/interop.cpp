#include "channel.h"
#include <cstring>
static_assert(sizeof(DspAaPresentationConfiguration) == 32);
static_assert(sizeof(DspAaPresentationBegin) == 24);
static_assert(sizeof(DspAaPresentationInputs) == 456);
static_assert(sizeof(DspAaPresentationStatus) == 472);
uint32_t __cdecl DspAaGetPresentationAbiVersion() {
    return DspAaPresentationAbiVersion;
}
int __cdecl DspAaConfigurePresentation(const DspAaPresentationConfiguration* value) {
    try {
        auto channel = dspaa::presentationChannel();
        return value && value->size == sizeof(*value) && value->version == DspAaPresentationAbiVersion &&
               channel && channel->configure(*value);
    } catch (...) {
        return 0;
    }
}
int __cdecl DspAaBeginPresentationFrame(DspAaPresentationBegin* value) {
    try {
        auto channel = dspaa::presentationChannel();
        return value && value->size == sizeof(*value) && value->version == DspAaPresentationAbiVersion &&
               channel && channel->begin(*value);
    } catch (...) {
        return 0;
    }
}
int __cdecl DspAaPresentationSimulationEnd(uint64_t id) {
    try {
        auto channel = dspaa::presentationChannel();
        return channel && channel->marker(id, dspaa::SlMarker::SimulationEnd);
    } catch (...) {
        return 0;
    }
}
void* __cdecl DspAaQueuePresentationInputs(const DspAaPresentationInputs* value) {
    return dspaa::queuePresentationInputs(value);
}
void __cdecl DspAaCancelPresentationInputs(void* token) {
    dspaa::cancelPresentationInputs(token);
}
DspAaRenderEvent __cdecl DspAaGetPresentationRenderEvent() {
    return dspaa::presentationRenderEvent();
}
int __cdecl DspAaGetPresentationStatus(DspAaPresentationStatus* value) {
    try {
        if (!value || value->size != sizeof(*value))
            return 0;
        if (auto channel = dspaa::presentationChannel())
            *value = channel->status();
        else {
            *value = {};
            value->size = sizeof(*value);
            value->version = DspAaPresentationAbiVersion;
            strncpy_s(
                value->message,
                "No presentation bridge is active in this process; consult the profile preloader result",
                _TRUNCATE);
        }
        return 1;
    } catch (...) {
        return 0;
    }
}
