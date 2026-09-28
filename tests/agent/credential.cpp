#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <windows.h>

#include "platform/windows/credential.transport.h"

int main() {
    using namespace ultraterimal::agent;
    auto path = std::filesystem::temp_directory_path() / (L"ultraterimal-credential-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    try {
        const std::string secret = "test-only-device-credential";
        saveCredential(path, secret);
        if (loadCredential(path) != secret) {
            throw std::runtime_error("DPAPI round trip failed");
        }
        bool overwriteRejected = false;
        try {
            saveCredential(path, "replacement");
        } catch (...) {
            overwriteRejected = true;
        }
        if (!overwriteRejected || loadCredential(path) != secret) {
            throw std::runtime_error("Existing identity was overwritten");
        }
        std::ifstream input(path, std::ios::binary);
        std::string encrypted((std::istreambuf_iterator<char>(input)), {});
        input.close();
        if (encrypted.find(secret) != std::string::npos) {
            throw std::runtime_error("Credential stored in cleartext");
        }
        std::ofstream damaged(path, std::ios::binary | std::ios::trunc);
        damaged << "not-a-dpapi-blob";
        damaged.close();
        bool tamperingRejected = false;
        try {
            (void)loadCredential(path);
        } catch (...) {
            tamperingRejected = true;
        }
        if (!tamperingRejected) {
            throw std::runtime_error("Tampered credential accepted");
        }
        std::filesystem::remove(path);
        std::cout << "Protected storage, overwrite prevention and tamper rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
