#pragma once

#include <filesystem>

namespace ultraterimal::agent {

// Installs or updates the Ultraterimal IDD virtual display driver package.
// When forceUpdate is false and the Root\UltraterimalIdd device already exists,
// returns without restarting the display driver.
void installIddDriver(const std::filesystem::path& iddDirectory, bool forceUpdate = true);

// Removes the Root\UltraterimalIdd virtual display device if present.
void removeIddDriver();

} // namespace ultraterimal::agent
