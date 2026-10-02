#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include <windows.h>

namespace ultraterimal::agent {
// Bounded PNG encoding through the system WIC codec, on the capture worker.
std::expected<std::string, HRESULT> encodeDetailPng(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);
} // namespace ultraterimal::agent
