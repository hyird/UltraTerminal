#pragma once

#include <filesystem>

namespace ultraterimal::agent {
// Admin-only installation of this provider's own 64-bit keys. Refuse existing
// registrations, and leave all Windows/default credential providers untouched.
void installWindowsLoginProvider(const std::filesystem::path& module);
void removeWindowsLoginProvider(const std::filesystem::path& module);
} // namespace ultraterimal::agent
