#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace ultraterimal::agent {
struct ScaledRgbImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

ScaledRgbImage downscaleBgraToRgb(std::span<const std::uint8_t> bgra, std::uint32_t srcWidth, std::uint32_t srcHeight,
                                  std::uint32_t maxWidth, std::uint32_t maxHeight);

std::expected<std::string, std::string> encodeRgbPng(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height);

std::expected<std::string, std::string> encodeBgraPng(std::span<const std::uint8_t> bgra, std::uint32_t srcWidth, std::uint32_t srcHeight,
                                                      std::uint32_t maxWidth, std::uint32_t maxHeight,
                                                      std::uint32_t& outWidth, std::uint32_t& outHeight);
} // namespace ultraterimal::agent
