#include "sl/runtime.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace {
namespace fs = std::filesystem;
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct FixtureDirectory {
    fs::path path;
    ~FixtureDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void reachesExportCheck(const fs::path& directory) {
    dspaa::SlRuntimeCreateInfo info;
    info.runtimeDirectory = directory;
    std::string failure;
    try {
        dspaa::SlRuntime runtime(info);
    } catch (const std::exception& error) {
        failure = error.what();
    }
    // The unsigned project fixture is a real loadable DLL, but deliberately
    // exports no Streamline API. Reaching this error proves that file contents
    // were not rejected by a release pin or custom signature check. No vendor
    // code, SDK initialization, window or graphics device is involved.
    require(failure == "Missing Streamline export: slInit",
            "Replacement DLL was not rejected at the required-export boundary");
    require(!GetModuleHandleW(L"sl.interposer.dll"), "Failed runtime retained its inactive fixture module");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3, "Pass the harmless fixture DLL and test output directory");
        const auto directory =
            fs::path(argv[2]) / (L"sl-runtime-loading-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(directory.parent_path());
        require(fs::create_directory(directory), "Test directory already exists; refusing to reuse it");
        FixtureDirectory cleanup{directory};
        for (const auto* name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll",
                                 L"sl.pcl.dll", L"nvngx_dlssg.dll"})
            fs::copy_file(argv[1], directory / name);
        reachesExportCheck(directory);
        // The same external filename can contain a different revision between
        // launches, and failed initialization must have released its file locks.
        {
            std::ofstream revision(directory / L"nvngx_dlssg.dll", std::ios::binary | std::ios::app);
            revision << "fixture revision";
            require(revision.good(), "Failed runtime retained a file lock");
        }
        reachesExportCheck(directory);
        require(fs::remove_all(directory) == 7, "Failed runtime did not release all fixture files");
        std::puts("External runtime replacements reach required-export validation without a custom content "
                  "gate; failed owners retire cleanly.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
