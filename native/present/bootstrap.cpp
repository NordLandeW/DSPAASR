#include "bootstrap-api.h"
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Arguments {
    LPWSTR* values = nullptr;
    int count = 0;
    Arguments() { values = CommandLineToArgvW(GetCommandLineW(), &count); }
    ~Arguments() { if (values) LocalFree(values); }
};
std::string digest(const fs::path& path) {
    const auto length = fs::file_size(path);
    if (!length || length > 64 * 1024 * 1024) throw std::runtime_error("Invalid plugin size");
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read pinned plugin");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA256");
    std::array<unsigned char, 32> result{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
                                  result.data(), static_cast<ULONG>(result.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Cannot hash pinned plugin");
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    for (auto byte : result) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text;
}
std::string line(std::ifstream& input) {
    std::string value;
    if (!std::getline(input, value)) throw std::runtime_error("Incomplete presentation opt-in");
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return value;
}
void failure(const fs::path& directory, const char* message) noexcept {
    try {
        fs::create_directories(directory);
        std::ofstream log(directory / "bootstrap-loader.log", std::ios::app);
        log << "Presentation bootstrap inactive: " << message << '\n';
    } catch (...) {}
}
} // namespace

// Unity owns discovery through its explicit boot.config PreInit setting. No XR
// provider/flags are registered, and DllMain performs no initialization or teardown.
extern "C" __declspec(dllexport) void __stdcall XRSDKPreInit(void*) noexcept {
    fs::path diagnostics;
    try {
        std::array<wchar_t, 32768> image{};
        const auto length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (!length || length >= image.size() || _wcsicmp(fs::path(image.data()).filename().c_str(), L"DSPGAME.exe"))
            return;
        Arguments arguments;
        if (!arguments.values) return;
        bool enabled = false, seenEnable = false;
        fs::path preloader;
        for (int i = 1; i < arguments.count; ++i) {
            const std::wstring key = arguments.values[i];
            if (key == L"--doorstop-enable") {
                if (seenEnable || i + 1 >= arguments.count) return;
                seenEnable = true;
                enabled = _wcsicmp(arguments.values[++i], L"true") == 0;
            } else if (key == L"--doorstop-target" || key == L"--doorstop-target-assembly") {
                if (!preloader.empty() || i + 1 >= arguments.count) return;
                preloader = arguments.values[++i];
            }
        }
        // Vanilla launches and other profiles are deliberately untouched.
        if (!enabled || !preloader.is_absolute() || !fs::is_regular_file(preloader) ||
            _wcsicmp(preloader.filename().c_str(), L"BepInEx.Preloader.dll") ||
            _wcsicmp(preloader.parent_path().filename().c_str(), L"core")) return;
        const auto profile = fs::weakly_canonical(preloader).parent_path().parent_path();
        const auto optin = profile / "config/dspaa.present.optin";
        if (!fs::is_regular_file(optin)) return;
        diagnostics = profile / "cache/DSPAAMod";
        if (fs::file_size(optin) > 512) throw std::runtime_error("Presentation opt-in is oversized");
        std::ifstream manifest(optin);
        if (line(manifest) != "DSPAASR-PRESENT-1") throw std::runtime_error("Unknown presentation opt-in ABI");
        const auto modeLine = line(manifest);
        if (modeLine != "mode=trace" && modeLine != "mode=present") throw std::runtime_error("Unsupported presentation bootstrap mode");
        const uint32_t mode = modeLine == "mode=present" ? 1 : 0;
        const auto nativeHash = line(manifest), managedHash = line(manifest);
        if (!nativeHash.starts_with("native-sha256=") || nativeHash.size() != 14 + 64 ||
            !managedHash.starts_with("managed-sha256=") || managedHash.size() != 15 + 64)
            throw std::runtime_error("Invalid presentation payload hashes");
        std::string trailing;
        while (std::getline(manifest, trailing))
            if (!trailing.empty() && trailing != "\r") throw std::runtime_error("Unexpected opt-in fields");
        fs::path plugin;
        for (const auto* relative : {L"plugins/DSPAASR", L"plugins/NordLandeW-DSPAASR"}) {
            const auto candidate = profile / relative;
            if (!fs::is_regular_file(candidate / "DSPAAMod.dll")) continue;
            if (!plugin.empty()) throw std::runtime_error("Multiple enabled DSPAASR installations");
            plugin = candidate;
        }
        if (plugin.empty()) throw std::runtime_error("DSPAASR is disabled or absent from this profile");
        if (digest(plugin / "DSPAANative.dll") != nativeHash.substr(14) ||
            digest(plugin / "DSPAAMod.dll") != managedHash.substr(15))
            throw std::runtime_error("Presentation opt-in does not match the installed plugin");
        const auto module = LoadLibraryExW((plugin / "DSPAANative.dll").c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) throw std::runtime_error("Cannot load the pinned native plugin");
        // Entry/callback addresses may remain in Unity or API trampolines. Never
        // unload a possibly active presentation module, even after an error.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, (plugin / "DSPAANative.dll").c_str(), &pinned))
            throw std::runtime_error("Cannot pin the presentation module");
        const auto entry = reinterpret_cast<DspAaPresentationBootstrap>(GetProcAddress(module, "DspAaBootstrapPresentation"));
        if (!entry || entry(dspaaPresentationBootstrapAbi, plugin.c_str(), diagnostics.c_str(), mode) != 1)
            throw std::runtime_error("Native presentation bootstrap rejected initialization");
    } catch (const std::exception& error) {
        if (!diagnostics.empty()) failure(diagnostics, error.what());
    } catch (...) {
        if (!diagnostics.empty()) failure(diagnostics, "Unknown early-entry failure");
    }
}
