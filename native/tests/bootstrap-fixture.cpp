#include "present/bootstrap-api.h"
#include <filesystem>
#include <windows.h>
#include <string_view>

namespace { int calls = 0; }
extern "C" __declspec(dllexport) int __cdecl DspAaBootstrapPresentation(
    uint32_t abi, const wchar_t* runtime, const wchar_t* data, uint32_t mode) noexcept {
    try {
        const auto expectedMode = std::wstring_view(GetCommandLineW()).find(L"--test-presentation") != std::wstring_view::npos ? 1u : 0u;
        if (abi != dspaaPresentationBootstrapAbi || !runtime || !data || mode != expectedMode) return 0;
        const std::filesystem::path directory(runtime);
        if (directory.filename() != L"DSPAASR" && directory.filename() != L"NordLandeW-DSPAASR") return 0;
        if (std::filesystem::path(data) != directory.parent_path().parent_path() / L"cache/DSPAAMod") return 0;
        ++calls;
        return 1;
    } catch (...) { return 0; }
}
extern "C" __declspec(dllexport) int __cdecl DspAaTestBootstrapSeen() { return calls; }
