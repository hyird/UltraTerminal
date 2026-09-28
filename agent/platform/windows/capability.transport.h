#pragma once
#include <cstdint>
#include <string>

namespace ultraterimal::agent {
struct Capabilities {
    bool conpty = false;
    bool mediaFoundationPresent = false;
    bool elevated = false;
    std::uint32_t processSession = 0;
    std::uint32_t activeConsoleSession = 0xffffffff;
    std::uint32_t attachedDisplays = 0;
    std::uint32_t graphicsError = 0;
};
Capabilities detectCapabilities();
std::string capabilityJson(const Capabilities& capabilities);
} // namespace ultraterimal::agent
