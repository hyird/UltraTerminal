#include "ultraterimal/protocol/native.protocol.h"

namespace ultraterimal::protocol {
namespace {
bool token(std::string_view value) { return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos; }
} // namespace
std::expected<wire::NativeConnect, MessageError> decodeNativeConnect(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumNativeHandshakeBytes) {
        return std::unexpected(MessageError::kTooLarge);
    }
    wire::NativeConnect request;
    if (!request.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::unexpected(MessageError::kMalformed);
    }
    if (request.protocol_version() != kVersion) {
        return std::unexpected(MessageError::kVersion);
    }
    if (request.purpose() == wire::NativeConnect::ENROLLMENT) {
        if (!request.credential().empty() || !token(request.enrollment_code()) || request.device_name().empty() || request.device_name().size() > 128 || request.device_name().find('\0') != std::string::npos) {
            return std::unexpected(MessageError::kInvalidField);
        }
    } else if (request.purpose() == wire::NativeConnect::CONTROL || request.purpose() == wire::NativeConnect::DESKTOP_CHANNEL || request.purpose() == wire::NativeConnect::AGENT_UPDATE || request.purpose() == wire::NativeConnect::FILE_TRANSFER) {
        if (!token(request.credential()) || !request.enrollment_code().empty() || !request.device_name().empty()) {
            return std::unexpected(MessageError::kInvalidField);
        }
    } else {
        return std::unexpected(MessageError::kInvalidField);
    }
    return request;
}
std::expected<wire::NativeAccepted, MessageError> decodeNativeAccepted(std::string_view bytes, bool enrollment) {
    if (bytes.empty() || bytes.size() > kMaximumNativeHandshakeBytes) {
        return std::unexpected(MessageError::kTooLarge);
    }
    wire::NativeAccepted response;
    if (!response.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::unexpected(MessageError::kMalformed);
    }
    if (response.protocol_version() != kVersion) {
        return std::unexpected(MessageError::kVersion);
    }
    const auto& id = response.device_id();
    if ((!response.accepted() && (!id.empty() || !response.credential().empty())) ||
        (response.accepted() && (id.empty() || id.size() > 128 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
                                 (enrollment ? !token(response.credential()) : !response.credential().empty())))) {
        return std::unexpected(MessageError::kInvalidField);
    }
    return response;
}
} // namespace ultraterimal::protocol
