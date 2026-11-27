#pragma once
#include <string_view>

namespace ultraterimal::protocol {
inline bool validPlatform(std::string_view value) noexcept {
    return value == "windows-x64" || value == "linux-x64";
}

inline std::string_view normalizePlatform(std::string_view value) noexcept {
    return value.empty() ? std::string_view("windows-x64") : value;
}
} // namespace ultraterimal::protocol
