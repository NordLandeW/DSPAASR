#include "internal.h"
// The official helper owns its WinTrust/Crypt32 function table. Include it in
// exactly this TU and call it only during serialized process initialization.
#include <sl_security.h>
#include <bcrypt.h>
#include <array>

namespace dspaa::detail {
namespace {
struct TrustState {
    WINTRUST_DATA data{};
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    TrustState() { data.cbStruct = sizeof(data); }
    ~TrustState() {
        if (data.hWVTStateData) { data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &policy, &data); }
    }
};
struct HashState {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    ~HashState() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
void cryptoCheck(NTSTATUS result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + " failed (NTSTATUS " + std::to_string(result) + ")");
}
void verifyNgx(const std::filesystem::path& path, HANDLE file) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo); fileInfo.pcwszFilePath = path.c_str(); fileInfo.hFile = file;
    TrustState trust;
    trust.data.dwUIChoice = WTD_UI_NONE;
    trust.data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.data.dwUnionChoice = WTD_CHOICE_FILE;
    trust.data.pFile = &fileInfo;
    trust.data.dwStateAction = WTD_STATEACTION_VERIFY;
    // No UI or boot-time network dependency; use the current local Windows trust
    // policy/cache, matching the official SL helper's primary-signature policy.
    trust.data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const auto result = WinVerifyTrust(nullptr, &trust.policy, &trust.data);
    if (result != ERROR_SUCCESS) throw std::runtime_error("The pinned NGX FG DLL failed Windows signature verification");
    const auto* provider = WTHelperProvDataFromStateData(trust.data.hWVTStateData);
    auto* signer = provider ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0) : nullptr;
    const auto* certificate = signer && signer->csCertChain ? signer->pasCertChain[0].pCert : nullptr;
    if (!certificate) throw std::runtime_error("The verified NGX FG DLL has no signer certificate");
    std::array<wchar_t, 256> name{};
    if (!CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME),
                            name.data(), static_cast<DWORD>(name.size())) || std::wstring(name.data()) != L"NVIDIA Corporation")
        throw std::runtime_error("The pinned NGX FG DLL is not signed by NVIDIA Corporation");
    HashState hash;
    cryptoCheck(BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "Open SHA-256");
    DWORD bytes = 0, objectBytes = 0;
    cryptoCheck(BCryptGetProperty(hash.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &bytes, 0), "Get SHA-256 state size");
    hash.object.resize(objectBytes);
    cryptoCheck(BCryptCreateHash(hash.algorithm, &hash.hash, hash.object.data(), objectBytes, nullptr, 0, 0), "Create SHA-256 state");
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Seek locked NGX DLL");
    std::array<UCHAR, 65536> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Read locked NGX DLL");
        if (!count) break;
        cryptoCheck(BCryptHashData(hash.hash, buffer.data(), count, 0), "Hash locked NGX DLL");
    }
    std::array<UCHAR, 32> digest{};
    cryptoCheck(BCryptFinishHash(hash.hash, digest.data(), static_cast<ULONG>(digest.size()), 0), "Finish NGX SHA-256");
    constexpr std::array<UCHAR, 32> expected{
        0xff,0x6e,0x90,0xeb,0x78,0xb8,0x27,0x92,0x7d,0xff,0x5b,0x4e,0xcc,0x6b,0x1c,0x87,
        0x0c,0x2e,0x9b,0xca,0x29,0xed,0x9f,0x48,0xc7,0xd3,0x48,0xcc,0x9e,0x17,0x0b,0x82};
    if (digest != expected) throw std::runtime_error("NGX FG DLL differs from the authenticated Streamline 2.14.1 release pin");
    // BCryptDestroyHash may still read its caller-owned object storage.
    BCryptDestroyHash(hash.hash); hash.hash = nullptr;
}
} // namespace
void verifyRuntimeFiles(SlRuntimeState& state) {
    constexpr const wchar_t* names[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
                                       L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll"};
    for (const auto* name : names) {
        const auto path = state.pluginPath / name;
        auto file = std::make_unique<SlFileLock>();
        file->value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->value == INVALID_HANDLE_VALUE) graphicsCheck(HRESULT_FROM_WIN32(GetLastError()), "Lock official Streamline runtime DLL");
        if (std::wstring_view(name) == L"nvngx_dlssg.dll") {
            // This official NGX DLL has no nested NVS signature. Do not apply the
            // sl.*-specific secondary-key check to a different signing contract.
            verifyNgx(path, file->value);
        } else if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
            throw std::runtime_error("Streamline DLL failed NVIDIA primary/secondary signature validation: " + path.string());
        }
        // Retain the no-write/no-delete sharing lock across verification, SDK
        // loading and all workers/callbacks (including a quarantined runtime).
        state.files.push_back(std::move(file));
    }
}
} // namespace dspaa::detail
