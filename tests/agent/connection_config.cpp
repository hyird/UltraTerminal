#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "features/connection/connection.config.h"

namespace {
unsigned long currentProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}
} // namespace

int main() {
    const auto file = std::filesystem::temp_directory_path() / ("ultraterimal-config-" + std::to_string(currentProcessId()) + ".json");
    const auto write = [&](const std::string& text) {
        std::ofstream stream(file);
        stream << text;
    };
    const auto rejected = [&](const std::string& text) {
        write(text);
        try {
            (void)ultraterimal::agent::loadConnectionConfig(file);
            return false;
        } catch (...) {
            return true;
        }
    };
    try {
        write(R"({"host":"localhost","device_id":"device-1","ca_file":"ca.pem","credential_file":"device.bin"})");
        const auto config = ultraterimal::agent::loadConnectionConfig(file);
        if (config.port != "7443" || config.caFile != file.parent_path() / "ca.pem") {
            return 1;
        }
        try {
            (void)ultraterimal::agent::loadEnrollmentConfig(file);
            return 7;
        } catch (const std::exception&) {
        }
        write(R"({"host":"localhost","ca_file":"ca.pem","credential_file":"device.bin"})");
        auto enrollment = ultraterimal::agent::loadEnrollmentConfig(file);
        if (!enrollment.deviceId.empty() || enrollment.credentialFile != file.parent_path() / "device.bin") {
            return 8;
        }
        enrollment.deviceId = "new-device";
        write(ultraterimal::agent::connectionConfigJson(enrollment));
        const auto enrolled = ultraterimal::agent::loadConnectionConfig(file);
        if (enrolled.deviceId != enrollment.deviceId || enrolled.credentialFile != enrollment.credentialFile || enrolled.caFile != enrollment.caFile) {
            return 9;
        }
        write(R"({"host":"localhost","device_id":"device-1","ca_file":"system","credential_file":"device.bin"})");
        const auto systemCa = ultraterimal::agent::loadConnectionConfig(file);
        if (!systemCa.caFile.empty()) {
            return 10;
        }
        write(ultraterimal::agent::connectionConfigJson(systemCa));
        const auto reloadedSystemCa = ultraterimal::agent::loadConnectionConfig(file);
        if (!reloadedSystemCa.caFile.empty()) {
            return 11;
        }
        if (!rejected(R"({"host":"localhost","device_id":"device-1","ca_file":"ca.pem","credential_file":"device.bin","port":"0"})")) {
            return 2;
        }
        if (!rejected(R"({"host":"localhost\r\nInjected: yes","device_id":"device-1","ca_file":"ca.pem","credential_file":"device.bin"})")) {
            return 3;
        }
        if (!rejected(R"({"host":"localhost","device_id":"device-1","ca_file":"ca.pem","credential_file":"device.bin","verify_tls":false})")) {
            return 4;
        }
        if (!rejected(std::string(65537, 'x'))) {
            return 5;
        }
        std::filesystem::remove(file);
        std::cout << "Connection configuration validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::cerr << error.what() << '\n';
        return 6;
    }
}
