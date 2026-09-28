#pragma once
#include <chrono>
#include <filesystem>
#include <string>

namespace ultraterimal::agent {
struct ConnectionConfig {
    std::string host;
    std::string address;
    std::string port = "7443";
    std::filesystem::path caFile;
    std::filesystem::path credentialFile;
    std::string deviceId;
    std::chrono::seconds connectTimeout{10};
};
ConnectionConfig loadConnectionConfig(const std::filesystem::path& path);
ConnectionConfig loadEnrollmentConfig(const std::filesystem::path& path);
std::string connectionConfigJson(const ConnectionConfig& config);
} // namespace ultraterimal::agent
