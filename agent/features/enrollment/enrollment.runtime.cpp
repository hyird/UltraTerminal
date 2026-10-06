#include "enrollment.runtime.h"

#include <stdexcept>

#include <boost/json.hpp>

#include "enrollment.transport.h"
#include "platform/windows/credential.transport.h"

namespace ultraterimal::agent {
asio::awaitable<ConnectionConfig> enrollDevice(ruvia::execution_handle loop, ConnectionConfig config, std::string input) {
    if (input.size() > 4096 || !config.deviceId.empty()) {
        throw std::runtime_error("Invalid enrollment input");
    }
    const auto value = boost::json::parse(input);
    if (!value.is_object() || value.as_object().size() != 2) {
        throw std::runtime_error("Enrollment requires code and name");
    }
    const auto& object = value.as_object();
    const auto* code = object.if_contains("code");
    const auto* name = object.if_contains("name");
    if (!code || !name || !code->is_string() || !name->is_string()) {
        throw std::runtime_error("Enrollment requires code and name");
    }
    const std::string codeText(code->as_string()), nameText(name->as_string());
    if (codeText.size() != 64 || codeText.find_first_not_of("0123456789abcdef") != std::string::npos || nameText.empty() || nameText.size() > 128 || nameText.find('\0') != std::string::npos) {
        throw std::runtime_error("Invalid enrollment code or device name");
    }
    if (std::filesystem::exists(config.credentialFile)) {
        throw std::runtime_error("Device credential already exists; enrollment will not replace it");
    }
    if (!std::filesystem::is_directory(config.credentialFile.parent_path())) {
        throw std::runtime_error("Device credential directory does not exist");
    }
    auto identity = co_await exchangeEnrollment(loop, config, codeText, nameText);
    saveCredential(config.credentialFile, identity.credential);
    config.deviceId = std::move(identity.deviceId);
    co_return config;
}
} // namespace ultraterimal::agent
