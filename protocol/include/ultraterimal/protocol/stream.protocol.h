#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
// Used inside authenticated QUIC streams and TLS/TCP only, never raw UDP.
inline constexpr std::size_t kStreamHeaderBytes = 4;
inline constexpr char kNativeAlpn[] = "ultraterimal/1";
using StreamHeader = std::array<std::byte, kStreamHeaderBytes>;
std::expected<StreamHeader, MessageError> encodeStreamHeader(std::size_t messageBytes, std::size_t maximum = kMaximumMessageBytes);
std::expected<std::size_t, MessageError> decodeStreamHeader(std::span<const std::byte, kStreamHeaderBytes> header, std::size_t maximum = kMaximumMessageBytes);
} // namespace ultraterimal::protocol
