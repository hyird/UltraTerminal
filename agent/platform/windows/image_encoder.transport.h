#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include <windows.h>

namespace ultraterimal::agent {
// Thread-affine WIC resources, created and destroyed by the capture worker.
class DesktopPngEncoder {
  public:
    DesktopPngEncoder();
    ~DesktopPngEncoder();
    DesktopPngEncoder(const DesktopPngEncoder&) = delete;
    DesktopPngEncoder& operator=(const DesktopPngEncoder&) = delete;
    DesktopPngEncoder(DesktopPngEncoder&&) = delete;
    DesktopPngEncoder& operator=(DesktopPngEncoder&&) = delete;

  private:
    struct State;
    std::unique_ptr<State> state_;
    friend std::expected<std::string, HRESULT> encodeDetailPng(DesktopPngEncoder&, std::span<const std::uint8_t>, std::uint32_t, std::uint32_t);
};
// Each call creates a fresh stream/frame; factory and COM lifetime stay owned.
std::expected<std::string, HRESULT> encodeDetailPng(DesktopPngEncoder& encoder, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);
} // namespace ultraterimal::agent
