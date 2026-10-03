#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <windows.h>

#include "ultraterimal/protocol/message.protocol.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace ultraterimal::agent {
// Capture-worker owned D3D resources. The reference advances only after the
// owner submits an image. S_FALSE means optional GPU comparison is unavailable.
class DesktopTextureRegions {
  public:
    explicit DesktopTextureRegions(ID3D11Device* device);
    ~DesktopTextureRegions();
    DesktopTextureRegions(const DesktopTextureRegions&) = delete;
    DesktopTextureRegions& operator=(const DesktopTextureRegions&) = delete;
    HRESULT remember(ID3D11Texture2D* texture);
    std::expected<std::vector<std::uint32_t>, HRESULT> changes(ID3D11Texture2D* texture);
    // An atlas holds at most 32 128x64 tiles. Only the requested BGRA rows are
    // written; other bytes remain untouched and are not a complete CPU image.
    std::expected<std::uint64_t, HRESULT> read(ID3D11Texture2D* texture, std::span<const wire::Rectangle> regions, std::span<std::uint8_t> bgra);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
