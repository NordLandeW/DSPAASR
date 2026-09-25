#pragma once
#include "frame.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <filesystem>

namespace dspaa {
// Private identity only; it never exposes the underlying D3D12 COM object.
inline constexpr GUID presentationFacadeId = {0x06b235d7, 0xf060, 0x46ba, {0xa3,0xb9,0xd9,0xcb,0x7b,0x25,0x8b,0x81}};
using PresentationLog = void(*)(const char* message) noexcept;
// Construct before any real chain exists on window. The returned object exposes
// D3D11 resources and device identity; only its internal presenter uses D3D12.
HRESULT createPresentationFacade(IDXGIFactory2* factory, ID3D11Device* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1& description, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrictOutput, const std::filesystem::path& runtime, PresentationLog log,
    IDXGISwapChain1** output, PresentRetirement* retirement = nullptr) noexcept;
} // namespace dspaa
