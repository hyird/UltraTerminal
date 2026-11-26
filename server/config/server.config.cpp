#include "server.config.h"

#include <fstream>
#include <stdexcept>

#include <boost/json.hpp>
namespace ultraterimal::server {
ServerConfig loadServerConfig(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open gateway configuration");
    }
    std::string bytes(65537, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > 65536) {
        throw std::runtime_error("Gateway configuration too large");
    }
    const auto value = boost::json::parse(bytes);
    if (!value.is_object()) {
        throw std::invalid_argument("Expected gateway configuration object");
    }
    const auto& object = value.as_object();
    for (const auto& item : object) {
        if (item.key() != "address" && item.key() != "port" && item.key() != "database" && item.key() != "origin" && item.key() != "web_root" && item.key() != "agent_release_dir" && item.key() != "native_address" && item.key() != "native_public_address" && item.key() != "native_host" && item.key() != "native_port" && item.key() != "native_certificate_file" && item.key() != "native_key_file" && item.key() != "security_key_file") {
            throw std::invalid_argument("Unknown gateway setting");
        }
    }
    const auto text = [&](const char* key) { const auto& v=object.at(key); if(!v.is_string() || v.as_string().empty() || v.as_string().find('\0')!=boost::json::string::npos) { throw std::invalid_argument("Invalid gateway setting"); } return std::string(v.as_string()); };
    ServerConfig config;
    if (object.contains("address")) {
        config.address = text("address");
    }
    if (object.contains("port")) {
        const auto p = object.at("port").as_int64();
        if (p < 1 || p > 65535) {
            throw std::invalid_argument("Invalid gateway port");
        }
        config.port = static_cast<std::uint16_t>(p);
    }
    const auto localPath = [&](const char* key) { const auto t=text(key); auto path=std::filesystem::path(std::u8string(t.begin(),t.end())); return path.is_absolute()?path:file.parent_path()/path; };
    if (object.contains("native_port")) {
        const auto port = object.at("native_port").as_int64();
        if (port < 1 || port > 65535) {
            throw std::invalid_argument("Invalid native gateway port");
        }
        config.nativePort = static_cast<std::uint16_t>(port);
    }
    config.nativeCertificateFile = localPath("native_certificate_file");
    config.nativeKeyFile = localPath("native_key_file");
    if (object.contains("native_address")) {
        config.nativeAddress = text("native_address");
    }
    if (object.contains("web_root")) {
        config.webRoot = localPath("web_root");
    }
    if (object.contains("agent_release_dir")) {
        config.agentReleaseDir = localPath("agent_release_dir");
    }
    config.database = parseDatabaseConfig(boost::json::serialize(object.at("database")));
    config.origin = text("origin");
    if (!config.origin.starts_with("https://") || config.origin.find_first_of("\r\n ") != std::string::npos || config.origin.find('/', 8) != std::string::npos) {
        throw std::invalid_argument("Expected HTTPS origin without a path");
    }
    const auto originAuthority = config.origin.substr(8);
    const auto separator = originAuthority.rfind(':');
    const auto originHost = separator == std::string::npos ? originAuthority : originAuthority.substr(0, separator);
    config.nativeHost = object.contains("native_host") ? text("native_host") : originHost;
    config.nativePublicAddress = object.contains("native_public_address") ? text("native_public_address") : config.nativeHost;
    const auto validHost = [](const std::string& value) {
        return !value.empty() && value.size() <= 253 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos && value.front() != '.' && value.back() != '.';
    };
    if (!validHost(config.nativeHost) || !validHost(config.nativePublicAddress)) {
        throw std::invalid_argument("Invalid public native gateway address");
    }
    if (object.contains("security_key_file")) {
        const auto keyPath = localPath("security_key_file");
#ifndef _WIN32
        const auto permissions = std::filesystem::status(keyPath).permissions();
        if ((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) != std::filesystem::perms::none) {
            throw std::runtime_error("Security key file permissions are too broad");
        }
#endif
        std::ifstream keyInput(keyPath, std::ios::binary);
        std::string key(66, '\0');
        keyInput.read(key.data(), static_cast<std::streamsize>(key.size()));
        key.resize(static_cast<std::size_t>(keyInput.gcount()));
        if (!key.empty() && key.back() == '\n') {
            key.pop_back();
        }
        if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos) {
            throw std::runtime_error("Invalid security key file");
        }
        config.securityKey = std::move(key);
    }
    return config;
}
} // namespace ultraterimal::server
