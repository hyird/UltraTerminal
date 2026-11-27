#include "idd_install.transport.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include <devguid.h>
#include <setupapi.h>
#include <wincrypt.h>

#include "idd/idd_shared.protocol.h"

namespace ultraterimal::agent {
namespace {

using UpdateDriverFn = BOOL(WINAPI*)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
constexpr DWORD kInstallFlagForce = 0x00000001u;

struct DevInfoGuard {
    HDEVINFO value = INVALID_HANDLE_VALUE;
    ~DevInfoGuard() {
        if (value != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value);
        }
    }
};

struct ModuleGuard {
    HMODULE value = nullptr;
    ~ModuleGuard() {
        if (value) {
            FreeLibrary(value);
        }
    }
};

struct CertStoreGuard {
    HCERTSTORE value = nullptr;
    ~CertStoreGuard() {
        if (value) {
            CertCloseStore(value, 0);
        }
    }
};

bool hasHardwareId(HDEVINFO devInfo, SP_DEVINFO_DATA& devData, const wchar_t* targetId) {
    DWORD type = 0;
    DWORD needed = 0;
    SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_HARDWAREID, &type, nullptr, 0, &needed);
    if (needed < sizeof(wchar_t)) {
        return false;
    }
    std::vector<wchar_t> buffer(needed / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_HARDWAREID, &type,
                                           reinterpret_cast<PBYTE>(buffer.data()), needed, nullptr)) {
        return false;
    }
    for (const wchar_t* p = buffer.data(); *p != L'\0'; p += wcslen(p) + 1) {
        if (_wcsicmp(p, targetId) == 0) {
            return true;
        }
    }
    return false;
}

bool findExistingIddDevice() {
    DevInfoGuard devInfo{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_ALLCLASSES)};
    if (devInfo.value == INVALID_HANDLE_VALUE) {
        return false;
    }
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo.value, i, &devData); ++i) {
        if (hasHardwareId(devInfo.value, devData, idd::kHardwareId)) {
            return true;
        }
    }
    return false;
}

void importCertificateIfPresent(const std::filesystem::path& cerPath) {
    if (!std::filesystem::is_regular_file(cerPath)) {
        return;
    }
    std::ifstream input(cerPath, std::ios::binary);
    if (!input) {
        return;
    }
    const std::vector<BYTE> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return;
    }
    for (const wchar_t* storeName : {L"Root", L"TrustedPublisher"}) {
        CertStoreGuard store{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                           CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
                                           storeName)};
        if (store.value) {
            (void)CertAddEncodedCertificateToStore(store.value, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                   bytes.data(), static_cast<DWORD>(bytes.size()),
                                                   CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
        }
    }
}

void createRootIddDevice() {
    DevInfoGuard devInfo{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr)};
    if (devInfo.value == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetupDiCreateDeviceInfoList");
    }
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    if (!SetupDiCreateDeviceInfoW(devInfo.value, L"Display", &GUID_DEVCLASS_DISPLAY,
                                  idd::kAdapterFriendlyName, nullptr, DICD_GENERATE_ID, &devData)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetupDiCreateDeviceInfoW");
    }
    std::wstring hwIdMultiSz(idd::kHardwareId);
    hwIdMultiSz.push_back(L'\0');
    hwIdMultiSz.push_back(L'\0');
    if (!SetupDiSetDeviceRegistryPropertyW(devInfo.value, &devData, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(hwIdMultiSz.data()),
                                           static_cast<DWORD>(hwIdMultiSz.size() * sizeof(wchar_t)))) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetupDiSetDeviceRegistryPropertyW");
    }
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devInfo.value, &devData)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
    }
}

} // namespace

void installIddDriver(const std::filesystem::path& iddDirectory, bool forceUpdate) {
    const auto infPath = iddDirectory / L"ultraterimal_idd.inf";
    if (!std::filesystem::is_regular_file(infPath)) {
        throw std::runtime_error("IDD driver INF is missing");
    }
    const bool alreadyExists = findExistingIddDevice();
    if (alreadyExists && !forceUpdate) {
        return;
    }
    importCertificateIfPresent(iddDirectory / L"ultraterimal_idd.cer");
    if (!alreadyExists) {
        createRootIddDevice();
    }
    ModuleGuard newdev{LoadLibraryW(L"newdev.dll")};
    if (!newdev.value) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "LoadLibraryW(newdev.dll)");
    }
    auto* updateFn = reinterpret_cast<UpdateDriverFn>(GetProcAddress(newdev.value, "UpdateDriverForPlugAndPlayDevicesW"));
    if (!updateFn) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcAddress(UpdateDriverForPlugAndPlayDevicesW)");
    }
    BOOL rebootRequired = FALSE;
    if (!updateFn(nullptr, idd::kHardwareId, infPath.c_str(), kInstallFlagForce, &rebootRequired)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "UpdateDriverForPlugAndPlayDevicesW");
    }
}

void removeIddDriver() {
    DevInfoGuard devInfo{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_ALLCLASSES)};
    if (devInfo.value == INVALID_HANDLE_VALUE) {
        return;
    }
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo.value, i, &devData); ++i) {
        if (hasHardwareId(devInfo.value, devData, idd::kHardwareId)) {
            (void)SetupDiCallClassInstaller(DIF_REMOVE, devInfo.value, &devData);
        }
    }
}

} // namespace ultraterimal::agent
