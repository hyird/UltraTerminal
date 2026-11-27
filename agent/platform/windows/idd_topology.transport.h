#pragma once

#include <cstdint>
#include <string_view>

namespace ultraterimal::agent {

// Returns true when an adapter description belongs to Ultraterimal's IDD driver.
bool isUltraterimalIddAdapter(std::wstring_view description) noexcept;

// Returns true for software / paravirtual display adapters (VirtIO-GPU DOD,
// Microsoft Basic Display, Hyper-V Video, QXL, Bochs/QEMU, etc.) that lack
// hardware acceleration and suffer from slow WARP DXGI readback.
bool isSoftwareDisplayAdapter(std::wstring_view description, std::uint32_t vendorId = 0) noexcept;

// Returns true when the given GDI display name (e.g. L"\\\\.\\DISPLAY1") is
// backed by Ultraterimal Virtual Display.
bool isDisplayUltraterimalIdd(std::wstring_view displayDeviceName) noexcept;

// Ensures Ultraterimal Virtual Display is the sole active primary monitor when
// the machine has no active real hardware GPU monitor (headless or software/VM
// display only). Leaves machines with real hardware GPU monitors untouched.
// Returns true if Ultraterimal Virtual Display is now the sole active primary.
bool ensureIddPrimaryIfSoftwareDisplay() noexcept;

} // namespace ultraterimal::agent
