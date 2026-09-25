#pragma once
#include <d3d11_4.h>
#include <array>
#include <memory>

namespace dspaa::capture {
enum class OperationKind {
    Draw, UnsupportedDraw, Copy, CopyRegion, Resolve, ClearColor, ClearDepth,
    ClearView, Discard, Update, GenerateMips, Dispatch, ExecuteList,
    QueryBegin, QueryEnd, CpuWrite, UavWrite, ResourceLod
};
// Descriptive metadata only. Indirect/auto calls remain unsupported for replay.
enum class IndirectDrawKind { Auto, IndexedInstanced, Instanced };
// Exact direct draw arguments, observed before the application's call. These
// describe IA consumption; they never authorize geometry or a replay themselves.
struct DrawArguments {
    bool indexed = false;
    unsigned count = 0, start = 0, instances = 1, firstInstance = 0;
    int baseVertex = 0;
};
struct Operation {
    OperationKind kind = OperationKind::Draw;
    ID3D11Resource* destination = nullptr;
    ID3D11Resource* source = nullptr;
    ID3D11View* view = nullptr;
    ID3D11Asynchronous* query = nullptr;
    unsigned destinationSubresource = 0, sourceSubresource = 0;
    unsigned x = 0, y = 0, z = 0, flags = 0;
    const D3D11_BOX* box = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::array<float, 4> color{};
    float depth = 0;
    unsigned char stencil = 0;
    const D3D11_RECT* rectangles = nullptr;
    unsigned rectangleCount = 0;
    DrawArguments draw;
    IndirectDrawKind indirectKind = IndirectDrawKind::Auto;
    ID3D11Buffer* indirectArguments = nullptr; // Borrowed for this synchronous observation only.
    unsigned indirectOffset = 0;
};
// Stack-only synchronous call. run() suppresses nested driver entry-point
// aliases, and guarantees the application's original operation runs once.
struct NativeCall {
    void* object = nullptr;
    void (*invoke)(void*) = nullptr;
    bool called = false;
    void run();
    // Reissue exactly the same draw arguments only after the observer has bound
    // its private targets. Never used for application state/history operations.
    void replay();
};
struct Bypass {
    Bypass();
    ~Bypass();
    Bypass(const Bypass&) = delete;
};
struct Observer {
    virtual ~Observer() = default;
    virtual void observe(const Operation& operation, NativeCall& original) noexcept = 0;
};
// Shared hook ownership for a bounded CPU constant-buffer shadow. Callbacks
// must not issue/mutate context commands, retain mapped pointers after Unmap,
// or throw. The consumer handles its actual context/resource domain and bounds.
// These callbacks see application writes, including non-capture contexts, but
// never capture's own replay/helper commands. Registration is process-local.
struct WriteObserver {
    virtual ~WriteObserver() = default;
    virtual void mapped(ID3D11DeviceContext* context, ID3D11Resource* resource, unsigned subresource,
                        D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE& mapping) noexcept = 0;
    virtual void beforeUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource, unsigned subresource) noexcept = 0;
    virtual void beforeUpdate(ID3D11DeviceContext* context, ID3D11Resource* resource, unsigned subresource,
                              const D3D11_BOX* box, const void* data, unsigned rowPitch, unsigned depthPitch,
                              unsigned copyFlags) noexcept = 0;
    // nullptr means the destination set is opaque: invalidate every CPU shadow.
    virtual void invalidated(ID3D11Resource* resource) noexcept = 0;
};
void attachWrites(const std::shared_ptr<WriteObserver>& observer);
void detachWrites(const WriteObserver* observer) noexcept;
// MinHook is initialized by the host. Hooks/code persist; no hot removal or
// global queue/uninitialize is performed. Only this exact context is observed.
void install(ID3D11DeviceContext4* context);
void attach(ID3D11DeviceContext4* context, const std::shared_ptr<Observer>& observer);
void detach(const Observer* observer) noexcept;
} // namespace dspaa::capture
