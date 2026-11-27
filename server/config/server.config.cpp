#include "server.config.h"

#include <binary_text_encoding.h>
#include <fstream>
#include <model.h>
#include <stdexcept>

namespace ultraterimal::server {
namespace {
RUVIA_MODEL(ServerConfigModel,
            RUVIA_OPTIONAL_FIELD(address, ruvia::string),
            RUVIA_OPTIONAL_FIELD(port, std::int64_t),
            RUVIA_REQUIRED_FIELD(database, ruvia::json_object),
            RUVIA_REQUIRED_FIELD(origin, ruvia::string),
            RUVIA_OPTIONAL_FIELD(web_root, ruvia::string),
            RUVIA_OPTIONAL_FIELD(agent_release_dir, ruvia::string),
            RUVIA_OPTIONAL_FIELD(native_address, ruvia::string),
            RUVIA_OPTIONAL_FIELD(native_public_address, ruvia::string),
            RUVIA_OPTIONAL_FIELD(native_host, ruvia::string),
            RUVIA_OPTIONAL_FIELD(native_port, std::int64_t),
            RUVIA_REQUIRED_FIELD(native_certificate_file, ruvia::string),
            RUVIA_REQUIRED_FIELD(native_key_file, ruvia::string),
            RUVIA_OPTIONAL_FIELD(security_key_file, ruvia::string));
} // namespace
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
    auto parsed = ruvia::parse_json<ServerConfigModel>(bytes, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        throw std::invalid_argument("Invalid gateway configuration");
    }
    const auto model = std::move(parsed).take_value();
    const auto readText = [](const ruvia::string& value) {
        const auto v = value.view();
        if (v.empty() || v.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("Invalid gateway setting");
        }
        return std::string(v);
    };
    const auto readPort = [](std::int64_t p, const char* error) {
        if (p < 1 || p > 65535) {
            throw std::invalid_argument(error);
        }
        return static_cast<std::uint16_t>(p);
    };
    const auto localPath = [&](const ruvia::string& value) {
        const auto t = readText(value);
        auto path = std::filesystem::path(std::u8string(t.begin(), t.end()));
        return path.is_absolute() ? path : file.parent_path() / path;
    };
    ServerConfig config;
    if (const auto& address = model.get<"address">()) {
        config.address = readText(*address);
    }
    if (const auto& port = model.get<"port">()) {
        config.port = readPort(*port, "Invalid gateway port");
    }
    if (const auto& nativePort = model.get<"native_port">()) {
        config.nativePort = readPort(*nativePort, "Invalid native gateway port");
    }
    config.nativeCertificateFile = localPath(model.get<"native_certificate_file">());
    config.nativeKeyFile = localPath(model.get<"native_key_file">());
    if (const auto& nativeAddress = model.get<"native_address">()) {
        config.nativeAddress = readText(*nativeAddress);
    }
    if (const auto& webRoot = model.get<"web_root">()) {
        config.webRoot = localPath(*webRoot);
    }
    if (const auto& agentReleaseDir = model.get<"agent_release_dir">()) {
        config.agentReleaseDir = localPath(*agentReleaseDir);
    }
    config.database = parseDatabaseConfig(model.get<"database">().view());
    config.origin = readText(model.get<"origin">());
    if (!config.origin.starts_with("https://") || config.origin.find_first_of("\r\n ") != std::string::npos || config.origin.find('/', 8) != std::string::npos) {
        throw std::invalid_argument("Expected HTTPS origin without a path");
    }
    const auto originAuthority = config.origin.substr(8);
    const auto separator = originAuthority.rfind(':');
    const auto originHost = separator == std::string::npos ? originAuthority : originAuthority.substr(0, separator);
    const auto& nativeHost = model.get<"native_host">();
    config.nativeHost = nativeHost ? readText(*nativeHost) : originHost;
    const auto& nativePublicAddress = model.get<"native_public_address">();
    config.nativePublicAddress = nativePublicAddress ? readText(*nativePublicAddress) : config.nativeHost;
    const auto validHost = [](const std::string& value) {
        return !value.empty() && value.size() <= 253 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos && value.front() != '.' && value.back() != '.';
    };
    if (!validHost(config.nativeHost) || !validHost(config.nativePublicAddress)) {
        throw std::invalid_argument("Invalid public native gateway address");
    }
    if (const auto& securityKeyFile = model.get<"security_key_file">()) {
        const auto keyPath = localPath(*securityKeyFile);
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
        if (!ruvia::is_lower_hex(key, 64)) {
            throw std::runtime_error("Invalid security key file");
        }
        config.securityKey = std::move(key);
    }
    return config;
}
} // namespace ultraterimal::server
