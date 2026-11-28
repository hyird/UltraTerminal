#include "idd_install.transport.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

// clang-format off
#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>
#include <wincrypt.h>
// clang-format on

#include "idd/idd_shared.protocol.h"

namespace ultraterimal::agent {
namespace {

using UpdateDriverFn = BOOL(WINAPI*)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
constexpr DWORD kInstallFlagForce = 0x00000001u;
constexpr DWORD kErrorDeviceInstallInProgress = 0xE000023Du;

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

struct RegKeyGuard {
    HKEY value = reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE);
    ~RegKeyGuard() {
        if (value != reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE) && value != nullptr) {
            RegCloseKey(value);
        }
    }
};

std::string_view trimAscii(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool equalsAsciiIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

bool isValidDottedVersion(std::string_view ver) noexcept {
    if (ver.empty() || ver.size() > 64 || ver.front() == '.' || ver.back() == '.') {
        return false;
    }
    bool sawDot = false;
    bool prevDot = false;
    for (const char ch : ver) {
        if (ch == '.') {
            if (prevDot) {
                return false;
            }
            sawDot = true;
            prevDot = true;
        } else if (ch >= '0' && ch <= '9') {
            prevDot = false;
        } else {
            return false;
        }
    }
    return sawDot;
}

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

enum class DevNodeState {
    kPhantom,
    kPresentUnhealthy,
    kPresentStarted,
};

DevNodeState queryDevNodeState(DEVINST devInst) noexcept {
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CR_SUCCESS) {
        return DevNodeState::kPhantom;
    }
    if ((status & DN_STARTED) != 0 && (status & DN_HAS_PROBLEM) == 0 && problem == 0) {
        return DevNodeState::kPresentStarted;
    }
    return DevNodeState::kPresentUnhealthy;
}

std::optional<std::wstring> queryInstalledDriverVersion(HDEVINFO devInfo, SP_DEVINFO_DATA& devData) {
    RegKeyGuard key{SetupDiOpenDevRegKey(devInfo, &devData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ)};
    if (key.value == reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE) || key.value == nullptr) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.value, L"DriverVersion", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 256) {
        return std::nullopt;
    }
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key.value, L"DriverVersion", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ) {
        return std::nullopt;
    }
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    if (buffer.empty()) {
        return std::nullopt;
    }
    return buffer;
}

std::optional<std::wstring> readInfDriverVersion(const std::filesystem::path& infPath) {
    std::ifstream input(infPath, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parseInfDriverVersion(content);
}

struct ExistingIddStatus {
    bool present = false;
    bool healthyAndCurrent = false;
};

ExistingIddStatus inspectAndPruneIddDevices(const std::optional<std::wstring>& targetVersion) {
    ExistingIddStatus result{};
    DevInfoGuard devInfo{SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
    if (devInfo.value == INVALID_HANDLE_VALUE) {
        return result;
    }
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo.value, i, &devData); ++i) {
        if (!hasHardwareId(devInfo.value, devData, idd::kHardwareId)) {
            devData.cbSize = sizeof(devData);
            continue;
        }
        const auto state = queryDevNodeState(devData.DevInst);
        if (state == DevNodeState::kPhantom || result.present) {
            (void)SetupDiCallClassInstaller(DIF_REMOVE, devInfo.value, &devData);
            devData.cbSize = sizeof(devData);
            continue;
        }
        result.present = true;
        if (state == DevNodeState::kPresentStarted && targetVersion) {
            const auto installed = queryInstalledDriverVersion(devInfo.value, devData);
            result.healthyAndCurrent = installed.has_value() && *installed == *targetVersion;
        }
        devData.cbSize = sizeof(devData);
    }
    return result;
}

void importCertificateIfPresent(const std::filesystem::path& cerPath) {
    if (!std::filesystem::is_regular_file(cerPath)) {
        return;
    }
    std::ifstream input(cerPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open IDD driver certificate");
    }
    const std::vector<BYTE> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        throw std::runtime_error("IDD driver certificate is empty");
    }
    for (const wchar_t* storeName : {L"Root", L"TrustedPublisher"}) {
        // Omit CERT_STORE_OPEN_EXISTING_FLAG so LocalMachine\TrustedPublisher is
        // created automatically on clean Windows installations where it does not exist yet.
        CertStoreGuard store{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                           CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName)};
        if (!store.value) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CertOpenStore");
        }
        if (!CertAddEncodedCertificateToStore(store.value, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                              bytes.data(), static_cast<DWORD>(bytes.size()),
                                              CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "CertAddEncodedCertificateToStore");
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

std::optional<std::wstring> parseInfDriverVersion(std::string_view infContent) noexcept {
    try {
        if (infContent.size() >= 3 &&
            static_cast<unsigned char>(infContent[0]) == 0xEFu &&
            static_cast<unsigned char>(infContent[1]) == 0xBBu &&
            static_cast<unsigned char>(infContent[2]) == 0xBFu) {
            infContent.remove_prefix(3);
        }
        bool inVersionSection = false;
        while (!infContent.empty()) {
            const auto newline = infContent.find('\n');
            auto line = newline == std::string_view::npos ? infContent : infContent.substr(0, newline);
            infContent = newline == std::string_view::npos ? std::string_view{} : infContent.substr(newline + 1);

            const auto comment = line.find(';');
            if (comment != std::string_view::npos) {
                line = line.substr(0, comment);
            }
            line = trimAscii(line);
            if (line.empty()) {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                const auto section = trimAscii(line.substr(1, line.size() - 2));
                inVersionSection = equalsAsciiIgnoreCase(section, "Version");
                continue;
            }
            if (!inVersionSection) {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                continue;
            }
            const auto key = trimAscii(line.substr(0, eq));
            if (!equalsAsciiIgnoreCase(key, "DriverVer")) {
                continue;
            }
            const auto rhs = trimAscii(line.substr(eq + 1));
            const auto comma = rhs.find(',');
            if (comma == std::string_view::npos) {
                return std::nullopt;
            }
            const auto ver = trimAscii(rhs.substr(comma + 1));
            if (!isValidDottedVersion(ver)) {
                return std::nullopt;
            }
            return std::wstring(ver.begin(), ver.end());
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

void installIddDriver(const std::filesystem::path& iddDirectory, bool forceUpdate) {
    const auto fullDir = std::filesystem::weakly_canonical(iddDirectory);
    const auto infPath = fullDir / L"ultraterimal_idd.inf";
    const auto dllPath = fullDir / L"ultraterimal_idd.dll";
    const auto catPath = fullDir / L"ultraterimal_idd.cat";
    if (!std::filesystem::is_regular_file(infPath) ||
        !std::filesystem::is_regular_file(dllPath) ||
        !std::filesystem::is_regular_file(catPath)) {
        throw std::runtime_error("IDD driver package files are missing");
    }

    const auto targetVersion = readInfDriverVersion(infPath);
    const auto existing = inspectAndPruneIddDevices(targetVersion);
    if (existing.present && existing.healthyAndCurrent && !forceUpdate) {
        return;
    }

    importCertificateIfPresent(fullDir / L"ultraterimal_idd.cer");

    bool createdNewDevice = false;
    if (!existing.present) {
        createRootIddDevice();
        createdNewDevice = true;
    }

    ModuleGuard newdev{LoadLibraryW(L"newdev.dll")};
    if (!newdev.value) {
        const int err = static_cast<int>(GetLastError());
        if (createdNewDevice) {
            removeIddDriver();
        }
        throw std::system_error(err, std::system_category(), "LoadLibraryW(newdev.dll)");
    }
    auto* updateFn = reinterpret_cast<UpdateDriverFn>(GetProcAddress(newdev.value, "UpdateDriverForPlugAndPlayDevicesW"));
    if (!updateFn) {
        const int err = static_cast<int>(GetLastError());
        if (createdNewDevice) {
            removeIddDriver();
        }
        throw std::system_error(err, std::system_category(), "GetProcAddress(UpdateDriverForPlugAndPlayDevicesW)");
    }

    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 3; ++attempt) {
        BOOL rebootRequired = FALSE;
        if (updateFn(nullptr, idd::kHardwareId, infPath.c_str(), kInstallFlagForce, &rebootRequired)) {
            return;
        }
        lastError = GetLastError();
        if (lastError != kErrorDeviceInstallInProgress || attempt == 2) {
            break;
        }
        Sleep(500);
    }

    if (createdNewDevice) {
        removeIddDriver();
    }
    throw std::system_error(static_cast<int>(lastError), std::system_category(), "UpdateDriverForPlugAndPlayDevicesW");
}

void removeIddDriver() {
    DevInfoGuard devInfo{SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
    if (devInfo.value == INVALID_HANDLE_VALUE) {
        return;
    }
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo.value, i, &devData); ++i) {
        if (hasHardwareId(devInfo.value, devData, idd::kHardwareId)) {
            (void)SetupDiCallClassInstaller(DIF_REMOVE, devInfo.value, &devData);
        }
        devData.cbSize = sizeof(devData);
    }
}

} // namespace ultraterimal::agent
