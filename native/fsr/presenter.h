#pragma once

#include "present/frame.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace dspaa {
struct FsrPresenterCreationError : std::runtime_error {
    const PresentRetirement retirement;
    FsrPresenterCreationError(const std::string& message, PresentRetirement state)
        : std::runtime_error(message), retirement(state) {}
};
struct FsrPresenterCreateInfo {
    HWND window = nullptr;
    IDXGIFactory* factory = nullptr; // The original factory, never a Streamline proxy.
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* gameQueue = nullptr; // Direct queue on the same device; retained.
    DXGI_SWAP_CHAIN_DESC1 swapChain{};
    std::optional<DXGI_SWAP_CHAIN_FULLSCREEN_DESC> fullscreen;
    std::filesystem::path runtimeDirectory;
    uint64_t generation = 0;
    bool debugChecking = false;
};

struct FsrPresenterStatus {
    std::string algorithm, swapChainVersion, reason;
    uint64_t applicationPresents = 0;
    // Callback/dispatch counts are submission evidence, not measured display FPS.
    uint64_t successfulDispatches = 0, realCallbacks = 0, generatedCallbacks = 0;
    uint64_t frameGenerationMemoryBytes = 0, swapChainMemoryBytes = 0;
    bool generationActive = false, providerVerified = false, quarantined = false;
};

// One externally serialized caller owns all public operations (including status
// and queries). SDK callbacks never enter that caller or Unity. This object owns
// the only real chain on its HWND.
class FsrPresenter {
  public:
    explicit FsrPresenter(const FsrPresenterCreateInfo& info);
    ~FsrPresenter();
    FsrPresenter(const FsrPresenter&) = delete;
    FsrPresenter& operator=(const FsrPresenter&) = delete;

    // A non-TEST call always needs a valid, full-size Final. Incomplete temporal
    // inputs or generate=false preserve that Final without generating a frame.
    // TEST forwards without consuming a lease, sequence number or private slot.
    HRESULT present(FrameLease frame, const PresentArguments& arguments, bool generate) noexcept;
    HRESULT resize(const DXGI_SWAP_CHAIN_DESC1& description, uint64_t generation) noexcept;
    PresentRetirement stop() noexcept;

    // Fullscreen/mode changes first disable interpolation and prove retirement.
    HRESULT setFullscreenState(BOOL fullscreen, IDXGIOutput* output) noexcept;
    HRESULT resizeTarget(const DXGI_MODE_DESC& description) noexcept;
    HRESULT setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept;
    HRESULT setHdrMetadata(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) noexcept;
    HRESULT setMaximumFrameLatency(UINT latency) noexcept;
    HRESULT setSourceSize(UINT width, UINT height) noexcept;
    HRESULT setRotation(DXGI_MODE_ROTATION rotation) noexcept;
    HRESULT setMatrixTransform(const DXGI_MATRIX_3X2_F& matrix) noexcept;
    HRESULT setBackgroundColor(const DXGI_RGBA& color) noexcept;

    // Borrowed query-only view. Do not Present/Resize/Release or retain it across
    // resize/stop; mutating the chain behind this owner invalidates GPU retirement.
    IDXGISwapChain4* swapChainForQueries() const noexcept;
    FsrPresenterStatus status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dspaa
