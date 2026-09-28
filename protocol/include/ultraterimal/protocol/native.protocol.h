#pragma once

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::size_t kMaximumNativeHandshakeBytes = 4096;
std::expected<wire::NativeConnect, MessageError> decodeNativeConnect(std::string_view bytes);
std::expected<wire::NativeAccepted, MessageError> decodeNativeAccepted(std::string_view bytes, bool enrollment);
} // namespace ultraterimal::protocol
