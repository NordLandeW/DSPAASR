#include "hooks.h"
#include <MinHook.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace dspaa::capture {
namespace {
thread_local unsigned bypass = 0;
struct Hook { void* target = nullptr; void* original = nullptr; bool enabled = false; };
struct State {
    std::mutex installMutex, ownerMutex;
    std::weak_ptr<Observer> owner;
    std::weak_ptr<WriteObserver> writes;
    std::atomic<ID3D11DeviceContext*> context{nullptr};
    HMODULE module = nullptr;
    std::array<Hook, 149> hooks;
};
State& state() { static auto* value = new State; return *value; }
std::shared_ptr<WriteObserver> writer() noexcept {
    try { if (bypass) return {};
        auto& host = state(); std::lock_guard lock(host.ownerMutex); return host.writes.lock();
    } catch (...) { return {}; }
}
void invalidateWrites(ID3D11DeviceContext* context, const Operation& operation) noexcept {
    switch (operation.kind) {
    case OperationKind::Copy: case OperationKind::CopyRegion: case OperationKind::Resolve:
    case OperationKind::Discard: case OperationKind::UavWrite: case OperationKind::ClearView:
    case OperationKind::ExecuteList: case OperationKind::Dispatch: break;
    default: return;
    }
    const auto observer = writer(); if (!observer) return;
    switch (operation.kind) {
    case OperationKind::Copy: case OperationKind::CopyRegion: case OperationKind::Resolve:
        observer->invalidated(operation.destination); break;
    case OperationKind::Discard: case OperationKind::UavWrite: case OperationKind::ClearView: {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        if (operation.view) operation.view->GetResource(&resource);
        observer->invalidated(operation.destination ? operation.destination : resource.Get()); break;
    }
    case OperationKind::ExecuteList: observer->invalidated(nullptr); break;
    case OperationKind::Dispatch: {
        Microsoft::WRL::ComPtr<ID3D11Device> device; context->GetDevice(&device);
        const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
        std::array<ID3D11UnorderedAccessView*, 64> views{}; context->CSGetUnorderedAccessViews(0, count, views.data());
        for (auto* view : views) if (view) {
            Microsoft::WRL::ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
            observer->invalidated(resource.Get()); view->Release();
        }
        break;
    }
    default: break;
    }
}
template<unsigned Slot, class Function> Function original() { return reinterpret_cast<Function>(state().hooks[Slot].original); }
template<class Function> void invoke(ID3D11DeviceContext* context, const Operation& operation, Function&& function) noexcept {
    NativeCall call{&function, [](void* object) { (*static_cast<std::remove_reference_t<Function>*>(object))(); }};
    try {
        invalidateWrites(context, operation);
        auto& host = state();
        if (!bypass && host.context.load(std::memory_order_acquire) == context) {
            std::shared_ptr<Observer> owner;
            { std::lock_guard lock(host.ownerMutex); owner = host.owner.lock(); }
            if (owner) owner->observe(operation, call);
        }
    } catch (...) { /* The original application's operation is never suppressed. */ }
    if (!call.called) call.run();
}
void STDMETHODCALLTYPE drawIndexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    invoke(self, {}, [&] { original<12, F>()(self, count, start, base); });
}
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* self, UINT count, UINT start) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
    invoke(self, {}, [&] { original<13, F>()(self, count, start); });
}
void STDMETHODCALLTYPE drawIndexedInstanced(ID3D11DeviceContext* self, UINT count, UINT instances, UINT start, INT base, UINT firstInstance) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    invoke(self, {}, [&] { original<20, F>()(self, count, instances, start, base, firstInstance); });
}
void STDMETHODCALLTYPE drawInstanced(ID3D11DeviceContext* self, UINT count, UINT instances, UINT start, UINT firstInstance) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    invoke(self, {}, [&] { original<21, F>()(self, count, instances, start, firstInstance); });
}
void STDMETHODCALLTYPE drawAuto(ID3D11DeviceContext* self) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
    Operation op; op.kind = OperationKind::UnsupportedDraw;
    invoke(self, op, [&] { original<38, F>()(self); });
}
void STDMETHODCALLTYPE indexedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buffer, UINT offset) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    Operation op; op.kind = OperationKind::UnsupportedDraw;
    invoke(self, op, [&] { original<39, F>()(self, buffer, offset); });
}
void STDMETHODCALLTYPE indirect(ID3D11DeviceContext* self, ID3D11Buffer* buffer, UINT offset) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    Operation op; op.kind = OperationKind::UnsupportedDraw;
    invoke(self, op, [&] { original<40, F>()(self, buffer, offset); });
}
void STDMETHODCALLTYPE begin(ID3D11DeviceContext* self, ID3D11Asynchronous* query) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
    Operation op; op.kind = OperationKind::QueryBegin; op.query = query;
    invoke(self, op, [&] { original<27, F>()(self, query); });
}
void STDMETHODCALLTYPE end(ID3D11DeviceContext* self, ID3D11Asynchronous* query) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
    Operation op; op.kind = OperationKind::QueryEnd; op.query = query;
    invoke(self, op, [&] { original<28, F>()(self, query); });
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
    Operation op; op.kind = OperationKind::Dispatch;
    invoke(self, op, [&] { original<41, F>()(self, x, y, z); });
}
void STDMETHODCALLTYPE dispatchIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buffer, UINT offset) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    Operation op; op.kind = OperationKind::Dispatch;
    invoke(self, op, [&] { original<42, F>()(self, buffer, offset); });
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* self, ID3D11Resource* destination, ID3D11Resource* source) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
    Operation op; op.kind = OperationKind::Copy; op.destination = destination; op.source = source;
    invoke(self, op, [&] { original<47, F>()(self, destination, source); });
}
void STDMETHODCALLTYPE copyRegion(ID3D11DeviceContext* self, ID3D11Resource* destination, UINT dstSub, UINT x, UINT y, UINT z,
                                  ID3D11Resource* source, UINT srcSub, const D3D11_BOX* box) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
    Operation op; op.kind = OperationKind::CopyRegion; op.destination = destination; op.source = source;
    op.destinationSubresource = dstSub; op.sourceSubresource = srcSub; op.x = x; op.y = y; op.z = z; op.box = box;
    invoke(self, op, [&] { original<46, F>()(self, destination, dstSub, x, y, z, source, srcSub, box); });
}
void STDMETHODCALLTYPE copyRegion1(ID3D11DeviceContext1* self, ID3D11Resource* destination, UINT dstSub, UINT x, UINT y, UINT z,
                                   ID3D11Resource* source, UINT srcSub, const D3D11_BOX* box, UINT flags) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT);
    Operation op; op.kind = OperationKind::CopyRegion; op.destination = destination; op.source = source;
    op.destinationSubresource = dstSub; op.sourceSubresource = srcSub; op.x = x; op.y = y; op.z = z; op.box = box; op.flags = flags;
    invoke(self, op, [&] { original<115, F>()(self, destination, dstSub, x, y, z, source, srcSub, box, flags); });
}
void STDMETHODCALLTYPE resolve(ID3D11DeviceContext* self, ID3D11Resource* destination, UINT dstSub, ID3D11Resource* source, UINT srcSub, DXGI_FORMAT format) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
    Operation op; op.kind = OperationKind::Resolve; op.destination = destination; op.source = source;
    op.destinationSubresource = dstSub; op.sourceSubresource = srcSub; op.format = format;
    invoke(self, op, [&] { original<57, F>()(self, destination, dstSub, source, srcSub, format); });
}
void STDMETHODCALLTYPE clearColor(ID3D11DeviceContext* self, ID3D11RenderTargetView* view, const FLOAT color[4]) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
    Operation op; op.kind = OperationKind::ClearColor; op.view = view;
    for (unsigned i = 0; i < 4; ++i) op.color[i] = color[i];
    invoke(self, op, [&] { original<50, F>()(self, view, color); });
}
void STDMETHODCALLTYPE clearDepth(ID3D11DeviceContext* self, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
    Operation op; op.kind = OperationKind::ClearDepth; op.view = view; op.flags = flags; op.depth = depth; op.stencil = stencil;
    invoke(self, op, [&] { original<53, F>()(self, view, flags, depth, stencil); });
}
void STDMETHODCALLTYPE clearView(ID3D11DeviceContext1* self, ID3D11View* view, const FLOAT color[4], const D3D11_RECT* rects, UINT count) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT);
    Operation op; op.kind = OperationKind::ClearView; op.view = view; op.rectangles = rects; op.rectangleCount = count;
    for (unsigned i = 0; i < 4; ++i) op.color[i] = color[i];
    invoke(self, op, [&] { original<132, F>()(self, view, color, rects, count); });
}
void STDMETHODCALLTYPE discardResource(ID3D11DeviceContext1* self, ID3D11Resource* resource) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*);
    Operation op; op.kind = OperationKind::Discard; op.destination = resource;
    invoke(self, op, [&] { original<117, F>()(self, resource); });
}
void STDMETHODCALLTYPE discardView(ID3D11DeviceContext1* self, ID3D11View* view) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*);
    Operation op; op.kind = OperationKind::Discard; op.view = view;
    invoke(self, op, [&] { original<118, F>()(self, view); });
}
void STDMETHODCALLTYPE discardView1(ID3D11DeviceContext1* self, ID3D11View* view, const D3D11_RECT* rects, UINT count) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT);
    Operation op; op.kind = OperationKind::Discard; op.view = view; op.rectangles = rects; op.rectangleCount = count;
    invoke(self, op, [&] { original<133, F>()(self, view, rects, count); });
}
void STDMETHODCALLTYPE update(ID3D11DeviceContext* self, ID3D11Resource* destination, UINT sub, const D3D11_BOX* box, const void* data, UINT row, UINT depth) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
    Operation op; op.kind = OperationKind::Update; op.destination = destination; op.destinationSubresource = sub; op.box = box;
    if (auto observer = writer()) observer->beforeUpdate(self, destination, sub, box, data, row, depth, 0);
    invoke(self, op, [&] { original<48, F>()(self, destination, sub, box, data, row, depth); });
}
void STDMETHODCALLTYPE update1(ID3D11DeviceContext1* self, ID3D11Resource* destination, UINT sub, const D3D11_BOX* box, const void* data, UINT row, UINT depth, UINT flags) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
    Operation op; op.kind = OperationKind::Update; op.destination = destination; op.destinationSubresource = sub; op.box = box; op.flags = flags;
    if (auto observer = writer()) observer->beforeUpdate(self, destination, sub, box, data, row, depth, flags);
    invoke(self, op, [&] { original<116, F>()(self, destination, sub, box, data, row, depth, flags); });
}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* output) {
    using F = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    if (type == D3D11_MAP_READ) return original<14, F>()(self, resource, sub, type, flags, output);
    Operation op; op.kind = OperationKind::CpuWrite; op.destination = resource; op.destinationSubresource = sub;
    HRESULT result = E_FAIL;
    invoke(self, op, [&] { result = original<14, F>()(self, resource, sub, type, flags, output); });
    if (SUCCEEDED(result) && output) if (auto observer = writer()) observer->mapped(self, resource, sub, type, *output);
    return result;
}
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT sub) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
    if (auto observer = writer()) observer->beforeUnmap(self, resource, sub);
    Bypass guard; original<15, F>()(self, resource, sub);
}
void STDMETHODCALLTYPE generateMips(ID3D11DeviceContext* self, ID3D11ShaderResourceView* view) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ShaderResourceView*);
    Operation op; op.kind = OperationKind::GenerateMips; op.view = view;
    invoke(self, op, [&] { original<54, F>()(self, view); });
}
void STDMETHODCALLTYPE lod(ID3D11DeviceContext* self, ID3D11Resource* resource, FLOAT minimum) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, FLOAT);
    Operation op; op.kind = OperationKind::ResourceLod; op.destination = resource;
    invoke(self, op, [&] { original<55, F>()(self, resource, minimum); });
}
void STDMETHODCALLTYPE execute(ID3D11DeviceContext* self, ID3D11CommandList* list, BOOL restore) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
    Operation op; op.kind = OperationKind::ExecuteList;
    invoke(self, op, [&] { original<58, F>()(self, list, restore); });
}
void STDMETHODCALLTYPE clearUint(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* view, const UINT values[4]) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT*);
    Operation op; op.kind = OperationKind::UavWrite; op.view = view;
    invoke(self, op, [&] { original<51, F>()(self, view, values); });
}
void STDMETHODCALLTYPE clearFloat(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* view, const FLOAT values[4]) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT*);
    Operation op; op.kind = OperationKind::UavWrite; op.view = view;
    invoke(self, op, [&] { original<52, F>()(self, view, values); });
}
void STDMETHODCALLTYPE copyCounter(ID3D11DeviceContext* self, ID3D11Buffer* destination, UINT offset, ID3D11UnorderedAccessView* source) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*);
    Operation op; op.kind = OperationKind::UavWrite; op.destination = destination;
    invoke(self, op, [&] { original<49, F>()(self, destination, offset, source); });
}
void hook(ID3D11DeviceContext4* context, unsigned slot, void* detour) {
    auto& value = state().hooks[slot];
    void* target = (*reinterpret_cast<void***>(context))[slot];
    if (value.target) {
        if (value.target != target) throw std::runtime_error("D3D11 capture encountered an unobserved context implementation");
        if (value.enabled) return;
    } else {
        const auto created = MH_CreateHook(target, detour, &value.original);
        if (created != MH_OK) throw std::runtime_error(std::string("Create D3D11 capture hook: ") + MH_StatusToString(created));
        value.target = target;
    }
    // Enable only this target. Applying the global queued set could activate
    // a different host subsystem's partially prepared hook transaction.
    const auto enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        throw std::runtime_error(std::string("Enable D3D11 capture hook: ") + MH_StatusToString(enabled));
    value.enabled = true;
}
} // namespace
Bypass::Bypass() { ++bypass; }
Bypass::~Bypass() { --bypass; }
void NativeCall::run() {
    if (called) return;
    called = true;
    Bypass guard;
    invoke(object);
}
void NativeCall::replay() { Bypass guard; invoke(object); }
void install(ID3D11DeviceContext4* context) {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        throw std::invalid_argument("Capture requires the game's original immediate context");
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> base;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> version1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&base))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&version1))) ||
        base.Get() != static_cast<ID3D11DeviceContext*>(context) || version1.Get() != static_cast<ID3D11DeviceContext1*>(context))
        throw std::runtime_error("Capture requires a single context interface implementation, not unobserved proxy aliases");
    auto& host = state();
    std::lock_guard lock(host.installMutex);
    if (!host.module && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                           reinterpret_cast<LPCWSTR>(&install), &host.module))
        throw std::runtime_error("Cannot pin process-lifetime capture hook code");
#define CAPTURE_HOOK(slot, name) hook(context, slot, reinterpret_cast<void*>(&name))
    CAPTURE_HOOK(12, drawIndexed); CAPTURE_HOOK(13, draw); CAPTURE_HOOK(14, map); CAPTURE_HOOK(15, unmap);
    CAPTURE_HOOK(20, drawIndexedInstanced); CAPTURE_HOOK(21, drawInstanced);
    CAPTURE_HOOK(27, begin); CAPTURE_HOOK(28, end); CAPTURE_HOOK(38, drawAuto);
    CAPTURE_HOOK(39, indexedIndirect); CAPTURE_HOOK(40, indirect); CAPTURE_HOOK(41, dispatch);
    CAPTURE_HOOK(42, dispatchIndirect); CAPTURE_HOOK(46, copyRegion); CAPTURE_HOOK(47, copy);
    CAPTURE_HOOK(48, update); CAPTURE_HOOK(49, copyCounter); CAPTURE_HOOK(50, clearColor); CAPTURE_HOOK(51, clearUint);
    CAPTURE_HOOK(52, clearFloat); CAPTURE_HOOK(53, clearDepth); CAPTURE_HOOK(54, generateMips);
    CAPTURE_HOOK(55, lod); CAPTURE_HOOK(57, resolve); CAPTURE_HOOK(58, execute);
    CAPTURE_HOOK(115, copyRegion1); CAPTURE_HOOK(116, update1); CAPTURE_HOOK(117, discardResource);
    CAPTURE_HOOK(118, discardView); CAPTURE_HOOK(132, clearView); CAPTURE_HOOK(133, discardView1);
#undef CAPTURE_HOOK
}
void attach(ID3D11DeviceContext4* context, const std::shared_ptr<Observer>& observer) {
    auto& host = state();
    std::lock_guard lock(host.ownerMutex);
    if (!host.owner.expired()) throw std::runtime_error("Another capture owner is still registered");
    host.owner = observer;
    host.context.store(context, std::memory_order_release);
}
void detach(const Observer* observer) noexcept {
    try {
        auto& host = state(); std::lock_guard lock(host.ownerMutex);
        const auto current = host.owner.lock();
        if (current.get() == observer) { host.context.store(nullptr, std::memory_order_release); host.owner.reset(); }
    } catch (...) {}
}
void attachWrites(const std::shared_ptr<WriteObserver>& observer) {
    auto& host = state(); std::lock_guard lock(host.ownerMutex);
    if (!host.writes.expired()) throw std::runtime_error("A D3D11 write observer is already registered");
    host.writes = observer;
}
void detachWrites(const WriteObserver* observer) noexcept {
    try { auto& host = state(); std::lock_guard lock(host.ownerMutex);
        if (host.writes.lock().get() == observer) host.writes.reset();
    } catch (...) {}
}
} // namespace dspaa::capture
