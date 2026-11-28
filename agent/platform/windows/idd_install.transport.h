#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ultraterimal::agent {

// Extracts the dotted driver version from the [Version] DriverVer directive of an INF file.
std::optional<std::wstring> parseInfDriverVersion(std::string_view infContent) noexcept;

// Installs or updates the Ultraterimal IDD virtual display driver package.
// When forceUpdate is false, skips reinstallation only if a healthy, started
// Root\UltraterimalIdd device is already running the exact version from ultraterimal_idd.inf.
void installIddDriver(const std::filesystem::path& iddDirectory, bool forceUpdate = true);

// Removes the Root\UltraterimalIdd virtual display device if present.
void removeIddDriver();

} // namespace ultraterimal::agent
