#include "connection.config.h"

#include <fstream>
#include <stdexcept>

#include <boost/json.hpp>

namespace ultraterimal::agent {
namespace {
ConnectionConfig loadConfig(const std::filesystem::path& path, bool enrollment) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open connection configuration");
    }
    std::string bytes(65537, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > 65536) {
        throw std::runtime_error("Connection configuration exceeds 64 KiB");
    }
    const auto value = boost::json::parse(bytes);
    if (!value.is_object()) {
        throw std::runtime_error("Connection configuration must be an object");
    }
    const auto& object = value.as_object();
    for (const auto& field : object) {
        if (field.key() != "host" && field.key() != "port" && field.key() != "ca_file" && field.key() != "credential_file" && field.key() != "device_id" && field.key() != "address") {
            throw std::runtime_error("Unknown connection configuration field");
        }
    }
    const auto string = [&](const char* key) {
        const auto* field = object.if_contains(key);
        if (!field || !field->is_string() || field->as_string().empty()) {
            throw std::runtime_error("Missing or invalid connection configuration field");
        }
        return std::string(field->as_string());
    };
    ConnectionConfig config;
    config.host = string("host");
    if (object.contains("address")) {
        config.address = string("address");
        if (config.address.size() > 253 || config.address.find_first_of("/\\ \t\r\n") != std::string::npos || config.address.find('\0') != std::string::npos) {
            throw std::runtime_error("Invalid native endpoint address");
        }
    }
    if (object.contains("port")) {
        config.port = string("port");
    }
    if (enrollment) {
        if (object.contains("device_id")) {
            throw std::runtime_error("Enrollment configuration must not contain an existing device identity");
        }
    } else {
        config.deviceId = string("device_id");
    }
    const auto utf8Path = [&](const char* key) {
        const auto text = string(key);
        if (text.find('\0') != std::string::npos) {
            throw std::runtime_error("Invalid configuration path");
        }
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    };
    config.caFile = utf8Path("ca_file");
    config.credentialFile = utf8Path("credential_file");
    if (config.host.size() > 253 || config.host.find_first_of("/\\ \t\r\n") != std::string::npos || config.host.find('\0') != std::string::npos || config.host.empty()) {
        throw std::runtime_error("Invalid gateway host");
    }
    if (config.port.empty() || config.port.size() > 5 || config.port.find_first_not_of("0123456789") != std::string::npos || std::stoul(config.port) == 0 || std::stoul(config.port) > 65535) {
        throw std::runtime_error("Invalid gateway port");
    }
    if (config.deviceId.size() > 128 || config.deviceId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
        throw std::runtime_error("Invalid device identifier");
    }
    const auto base = std::filesystem::absolute(path).parent_path();
    if (config.caFile.is_relative()) {
        config.caFile = base / config.caFile;
    }
    if (config.credentialFile.is_relative()) {
        config.credentialFile = base / config.credentialFile;
    }
    return config;
}
} // namespace
ConnectionConfig loadConnectionConfig(const std::filesystem::path& path) { return loadConfig(path, false); }
ConnectionConfig loadEnrollmentConfig(const std::filesystem::path& path) { return loadConfig(path, true); }
std::string connectionConfigJson(const ConnectionConfig& config) {
    const auto utf8 = [](const std::filesystem::path& path) {
        const auto bytes = path.u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    boost::json::object object{{"host", config.host}, {"port", config.port}, {"device_id", config.deviceId}, {"ca_file", utf8(config.caFile)}, {"credential_file", utf8(config.credentialFile)}};
    if (!config.address.empty()) {
        object["address"] = config.address;
    }
    return boost::json::serialize(object);
}
} // namespace ultraterimal::agent
