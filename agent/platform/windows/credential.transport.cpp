#include "credential.transport.h"

#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <windows.h>

#include <dpapi.h>

#include "platform/windows/handle.types.h"

namespace ultraterimal::agent {
namespace {
class ProtectedBuffer {
  public:
    DATA_BLOB blob{};
    ~ProtectedBuffer() {
        if (blob.pbData) {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
        }
    }
};
void require(BOOL success, const char* operation) {
    if (!success) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
}
} // namespace
void saveCredential(const std::filesystem::path& path, std::string_view value) {
    if (value.empty() || value.size() > 4096) {
        throw std::runtime_error("Invalid credential length");
    }
    DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    ProtectedBuffer encrypted;
    require(CryptProtectData(&input, L"Ultraterimal device credential", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted.blob), "CryptProtectData");
    // CREATE_NEW avoids silently replacing a registered device identity.
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        require(FALSE, "CreateFile credential");
    }
    try {
        DWORD written = 0;
        require(WriteFile(file.get(), encrypted.blob.pbData, encrypted.blob.cbData, &written, nullptr), "WriteFile credential");
        if (written != encrypted.blob.cbData) {
            throw std::runtime_error("Incomplete credential write");
        }
        require(FlushFileBuffers(file.get()), "FlushFileBuffers credential");
    } catch (...) {
        // Delete the file we created by handle, without a close/reopen path race.
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition));
        throw;
    }
}
std::string loadCredential(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open device credential");
    }
    std::vector<char> bytes(16385);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    if (bytes.empty() || bytes.size() > 16384) {
        throw std::runtime_error("Invalid protected credential length");
    }
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    ProtectedBuffer plain;
    require(CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.blob), "CryptUnprotectData");
    if (plain.blob.cbData == 0 || plain.blob.cbData > 4096) {
        throw std::runtime_error("Invalid device credential length");
    }
    return {reinterpret_cast<const char*>(plain.blob.pbData), plain.blob.cbData};
}
} // namespace ultraterimal::agent
