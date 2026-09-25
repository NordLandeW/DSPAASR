#include <windows.h>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    const auto bootstrap = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!bootstrap) return 3;
    const auto entry = reinterpret_cast<void(__stdcall*)(void*)>(GetProcAddress(bootstrap, "XRSDKPreInit"));
    if (!entry) return 4;
    entry(nullptr);
    const auto native = GetModuleHandleW(argv[2]);
    const bool expected = wcscmp(argv[3], L"true") == 0;
    if (!!native != expected) return 5;
    if (native) {
        const auto seen = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(native, "DspAaTestBootstrapSeen"));
        if (!seen || seen() != 1) return 6;
    }
    return 0;
}
