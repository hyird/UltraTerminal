#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace ultraterimal::server {
struct ServerConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string nativeAddress = "0.0.0.0";
    std::string nativePublicAddress;
    std::string nativeHost;
    std::uint16_t nativePort = 7443;
    std::filesystem::path nativeCertificateFile, nativeKeyFile;
    std::filesystem::path webRoot;
    std::string database;
    std::string origin;
};
ServerConfig loadServerConfig(const std::filesystem::path& file);
} // namespace ultraterimal::server
