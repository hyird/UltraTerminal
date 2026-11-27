#include "enrollment.runtime.h"

#include <stdexcept>

#include "enrollment.transport.h"
#include "platform/windows/credential.transport.h"

namespace ultraterimal::agent {
asio::awaitable<ConnectionConfig> enrollDevice(ruvia::event_loop loop, ConnectionConfig config, std::string input) {
    if (input.size() > 4096 || !config.deviceId.empty()) {
        throw std::runtime_error("Invalid enrollment input");
    }
    const auto object = parseStringJsonObject(input);
    const auto code = object.find("code");
    const auto name = object.find("name");
    if (object.size() != 2 || code == object.end() || name == object.end()) {
        throw std::runtime_error("Enrollment requires code and name");
    }
    const std::string& codeText = code->second;
    const std::string& nameText = name->second;
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
