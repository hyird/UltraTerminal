#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace ultraterimal::agent {
// Current-account DPAPI scope: service credentials must be enrolled as its identity.
void saveCredential(const std::filesystem::path& path, std::string_view value);
std::string loadCredential(const std::filesystem::path& path);
} // namespace ultraterimal::agent
