#pragma once
#include <cstdint>

// Separate early-entry ABI: SR ABI 2 and its initialization stay unchanged.
// Called outside the loader lock, before Unity creates its graphics device.
// mode 0 observes the original presentation path without replacing any object.
// mode 1 enables the D3D11-facing presentation owner at the Unity primary HWND.
constexpr uint32_t dspaaPresentationBootstrapAbi = 1;
using DspAaPresentationBootstrap = int(__cdecl*)(uint32_t abi, const wchar_t* runtimeDirectory,
                                               const wchar_t* dataDirectory, uint32_t mode);
