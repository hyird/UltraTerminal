#include "ultraterimal/protocol/stream.protocol.h"

namespace ultraterimal::protocol {
std::expected<StreamHeader, MessageError> encodeStreamHeader(std::size_t messageBytes, std::size_t maximum) {
    if (maximum == 0 || maximum > kMaximumMessageBytes || messageBytes == 0 || messageBytes > maximum) {
        return std::unexpected(MessageError::kTooLarge);
    }
    StreamHeader header{};
    for (std::size_t index = 0; index < header.size(); ++index) {
        header[index] = static_cast<std::byte>((messageBytes >> ((header.size() - index - 1) * 8)) & 0xff);
    }
    return header;
}
std::expected<std::size_t, MessageError> decodeStreamHeader(std::span<const std::byte, kStreamHeaderBytes> header, std::size_t maximum) {
    std::uint32_t size = 0;
    for (const auto byte : header) {
        size = (size << 8) | std::to_integer<std::uint32_t>(byte);
    }
    if (maximum == 0 || maximum > kMaximumMessageBytes || size == 0 || size > maximum) {
        return std::unexpected(MessageError::kTooLarge);
    }
    return size;
}
} // namespace ultraterimal::protocol
