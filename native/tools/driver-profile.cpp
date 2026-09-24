#include <Windows.h>
#include <nvapi.h>

// NVIDIA's settings header requires NvU32 from nvapi.h; keep this separate include group.
#include <NvApiDriverSettings.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
constexpr wchar_t profileName[] = L"DSPAAMod temporary probe cd751b03-626f-4384-a83e-30279c066322";
constexpr std::array<std::pair<NvU32, NvU32>, 4> isolatedSettings{
    {{NGX_DLSS_SR_OVERRIDE_ID, NGX_DLSS_SR_OVERRIDE_OFF},
     {NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID, NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_OFF},
     {NGX_DLSS_SR_MODE_ID, NGX_DLSS_SR_MODE_NGX_DLSS_SR_MODE_SNIPPET_CONTROLLED},
     {NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_ID, NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_DEFAULT}}};

void check(NvAPI_Status status, const char* operation) {
    if (status == NVAPI_OK)
        return;
    NvAPI_ShortString text{};
    NvAPI_GetErrorMessage(status, text);
    throw std::runtime_error(std::string(operation) + ": " + text + " (" + std::to_string(status) + ")");
}
void unicode(NvAPI_UnicodeString& target, const std::wstring& source) {
    if (source.size() >= NVAPI_UNICODE_STRING_MAX)
        throw std::runtime_error("NVAPI string too long.");
    for (size_t i = 0; i < source.size(); ++i)
        target[i] = static_cast<NvU16>(source[i]);
    target[source.size()] = 0;
}
std::wstring wide(const NvAPI_UnicodeString& value) {
    return reinterpret_cast<const wchar_t*>(value);
}
struct Session {
    NvDRSSessionHandle handle{};
    Session() {
        check(NvAPI_Initialize(), "NvAPI_Initialize");
        check(NvAPI_DRS_CreateSession(&handle), "Create DRS session");
        const auto status = NvAPI_DRS_LoadSettings(handle);
        if (status != NVAPI_OK) {
            NvAPI_DRS_DestroySession(handle);
            handle = nullptr;
            check(status, "Load driver settings");
        }
    }
    ~Session() {
        if (handle)
            NvAPI_DRS_DestroySession(handle);
    }
};
NvDRSProfileHandle application(Session& session, const fs::path& executable) {
    NvAPI_UnicodeString name{};
    unicode(name, executable.wstring());
    NVDRS_APPLICATION app{};
    app.version = NVDRS_APPLICATION_VER;
    NvDRSProfileHandle profile{};
    const auto status = NvAPI_DRS_FindApplicationByName(session.handle, name, &profile, &app);
    if (status == NVAPI_EXECUTABLE_NOT_FOUND)
        return nullptr;
    check(status, "Find application");
    return profile;
}
void printProfile(Session& session, NvDRSProfileHandle profile, const wchar_t* label) {
    NVDRS_PROFILE info{};
    info.version = NVDRS_PROFILE_VER;
    check(NvAPI_DRS_GetProfileInfo(session.handle, profile, &info), "Get profile info");
    std::wcout << label << L" profile=" << wide(info.profileName) << L" apps=" << info.numOfApps
               << L" settings=" << info.numOfSettings << L'\n';
    for (const auto& [id, ignored] : isolatedSettings) {
        (void)ignored;
        NVDRS_SETTING setting{};
        setting.version = NVDRS_SETTING_VER;
        const auto status = NvAPI_DRS_GetSetting(session.handle, profile, id, &setting);
        std::wcout << L"  id=0x" << std::hex << id << std::dec << L" status=" << status;
        if (status == NVAPI_OK)
            std::wcout << L" value=" << setting.u32CurrentValue << L" location=" << setting.settingLocation;
        std::wcout << L'\n';
    }
}
void inspect(Session& session, const fs::path& executable) {
    NvDRSProfileHandle global{};
    check(NvAPI_DRS_GetCurrentGlobalProfile(session.handle, &global), "Get global profile");
    printProfile(session, global, L"GLOBAL (read-only)");
    const auto profile = application(session, executable);
    if (profile)
        printProfile(session, profile, L"APPLICATION");
    else
        std::wcout << L"APPLICATION absent; inherits driver global/default settings\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: driver-profile inspect | isolate <new-backup-file> | restore\n"
                     "Affects only the sibling ngx-probe.exe; never modifies global or game profiles.\n";
        return 2;
    }
    try {
        const std::wstring command(argv[1]);
        if (command != L"inspect" && command != L"isolate" && command != L"restore")
            throw std::runtime_error("Unknown command.");
        if ((command == L"isolate") != (argc == 3))
            throw std::runtime_error("Only isolate requires a backup path.");
        std::array<wchar_t, 32768> module{};
        const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length == 0 || length >= module.size())
            throw std::runtime_error("Cannot resolve own executable.");
        const auto executable = fs::canonical(fs::path(module.data()).parent_path() / "ngx-probe.exe");
        Session session;
        if (command == L"inspect") {
            inspect(session, executable);
            return 0;
        }
        NvAPI_UnicodeString name{};
        unicode(name, profileName);
        NvDRSProfileHandle profile{};
        const auto found = NvAPI_DRS_FindProfileByName(session.handle, name, &profile);
        if (command == L"isolate") {
            if (application(session, executable))
                throw std::runtime_error("An application profile already exists; refusing to modify it.");
            if (found != NVAPI_PROFILE_NOT_FOUND) {
                check(found, "Find temporary profile");
                throw std::runtime_error("Temporary profile already exists; inspect/restore it first.");
            }
            const auto backup = fs::absolute(argv[2]);
            if (fs::exists(backup))
                throw std::runtime_error("Backup exists; refusing to overwrite.");
            fs::create_directories(backup.parent_path());
            NvAPI_UnicodeString backupName{};
            unicode(backupName, backup.wstring());
            check(NvAPI_DRS_SaveSettingsToFile(session.handle, backupName), "Export pre-change DRS backup");
            inspect(session, executable);
            NVDRS_PROFILE info{};
            info.version = NVDRS_PROFILE_VER;
            unicode(info.profileName, profileName);
            check(NvAPI_DRS_CreateProfile(session.handle, &info, &profile), "Create dedicated test profile");
            NVDRS_APPLICATION app{};
            app.version = NVDRS_APPLICATION_VER;
            unicode(app.appName, executable.wstring());
            unicode(app.userFriendlyName, L"DSPAAMod headless test only");
            check(NvAPI_DRS_CreateApplication(session.handle, profile, &app), "Bind exact probe executable");
            for (const auto& [id, value] : isolatedSettings) {
                NVDRS_SETTING setting{};
                setting.version = NVDRS_SETTING_VER;
                setting.settingId = id;
                setting.settingType = NVDRS_DWORD_TYPE;
                setting.u32CurrentValue = value;
                check(NvAPI_DRS_SetSetting(session.handle, profile, &setting),
                      "Set application-only override exception");
            }
            check(NvAPI_DRS_SaveSettings(session.handle), "Save dedicated test profile");
        } else {
            check(found, "Find temporary profile");
            NVDRS_PROFILE info{};
            info.version = NVDRS_PROFILE_VER;
            check(NvAPI_DRS_GetProfileInfo(session.handle, profile, &info), "Check restoration ownership");
            if (info.isPredefined || info.numOfApps != 1 || info.numOfSettings != isolatedSettings.size() ||
                application(session, executable) != profile)
                throw std::runtime_error("Temporary profile changed; refusing automatic deletion.");
            for (const auto& [id, value] : isolatedSettings) {
                NVDRS_SETTING setting{};
                setting.version = NVDRS_SETTING_VER;
                check(NvAPI_DRS_GetSetting(session.handle, profile, id, &setting), "Check isolated setting");
                if (setting.settingType != NVDRS_DWORD_TYPE ||
                    setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION ||
                    setting.u32CurrentValue != value)
                    throw std::runtime_error("Temporary setting changed; refusing deletion.");
            }
            check(NvAPI_DRS_DeleteProfile(session.handle, profile), "Delete owned temporary profile");
            check(NvAPI_DRS_SaveSettings(session.handle), "Save restoration");
        }
        check(NvAPI_DRS_LoadSettings(session.handle), "Reload persisted driver settings");
        inspect(session, executable);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
