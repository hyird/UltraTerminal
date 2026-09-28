#pragma once
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "schemas/session.pb.h"

namespace ultraterimal::protocol {
inline constexpr unsigned kVersion = 1;
inline constexpr std::size_t kMaximumMessageBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaximumControlBytes = 64 * 1024;
enum class MessageError { kTooLarge,
                          kMalformed,
                          kVersion,
                          kMissingBody,
                          kInvalidField };
std::expected<void, MessageError> validateMessage(const wire::Envelope& message);
std::expected<void, MessageError> validateInputEvent(const wire::InputEvent& input);
std::expected<wire::Envelope, MessageError> decodeMessage(std::string_view bytes);
std::expected<std::string, MessageError> encodeMessage(const wire::Envelope& message);
bool purposeMatches(wire::SessionKind kind, wire::ChannelPurpose purpose) noexcept;
} // namespace ultraterimal::protocol
