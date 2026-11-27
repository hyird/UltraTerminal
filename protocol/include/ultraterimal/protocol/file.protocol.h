#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::size_t kFileTransferChunkBytes = 64 * 1024;
inline constexpr std::size_t kMaximumFileTransferFrameBytes = kFileTransferChunkBytes + 4096;
inline constexpr std::size_t kMaximumFilePathBytes = 4096;
inline constexpr std::size_t kMaximumFileListEntries = 2000;
inline constexpr std::size_t kMaximumFileListControlBytes = 52 * 1024;
inline constexpr std::uint32_t kMaximumArchiveFiles = 100000;
inline constexpr std::uint32_t kMaximumArchiveDepth = 64;
inline constexpr std::uint64_t kMaximumArchiveBytes = 16ULL * 1024 * 1024 * 1024;

inline bool validTransferId(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

inline bool validFilePathText(std::string_view value, bool allowEmpty) noexcept {
    if (value.empty()) {
        return allowEmpty;
    }
    return value.size() <= kMaximumFilePathBytes && value.find('\0') == std::string_view::npos;
}

inline std::string_view fileErrorCodeName(wire::FileErrorCode code) noexcept {
    switch (code) {
    case wire::FILE_ERROR_NONE:
        return "none";
    case wire::FILE_ERROR_INVALID_PATH:
        return "invalid_path";
    case wire::FILE_ERROR_NOT_FOUND:
        return "not_found";
    case wire::FILE_ERROR_PERMISSION_DENIED:
        return "permission_denied";
    case wire::FILE_ERROR_FILE_LOCKED:
        return "file_locked";
    case wire::FILE_ERROR_ALREADY_EXISTS:
        return "already_exists";
    case wire::FILE_ERROR_IS_DIRECTORY:
        return "is_directory";
    case wire::FILE_ERROR_NOT_DIRECTORY:
        return "not_directory";
    case wire::FILE_ERROR_LIMIT_EXCEEDED:
        return "limit_exceeded";
    case wire::FILE_ERROR_IO_FAILED:
        return "io_failed";
    case wire::FILE_ERROR_CANCELLED:
        return "cancelled";
    default:
        return "io_failed";
    }
}

inline std::expected<wire::FileTransferFrame, MessageError> decodeFileTransferFrame(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumFileTransferFrameBytes) {
        return std::unexpected(MessageError::kTooLarge);
    }
    wire::FileTransferFrame frame;
    if (!frame.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::unexpected(MessageError::kMalformed);
    }
    switch (frame.body_case()) {
    case wire::FileTransferFrame::kHello:
        if (!validTransferId(frame.hello().transfer_id())) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return frame;
    case wire::FileTransferFrame::kMeta:
        if (frame.meta().filename().size() > 512 || frame.meta().filename().find('\0') != std::string::npos) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return frame;
    case wire::FileTransferFrame::kChunk:
        if (frame.chunk().data().size() > kFileTransferChunkBytes) {
            return std::unexpected(MessageError::kTooLarge);
        }
        return frame;
    case wire::FileTransferFrame::kFinish:
        if (!wire::FileErrorCode_IsValid(frame.finish().error())) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return frame;
    default:
        return std::unexpected(MessageError::kMissingBody);
    }
}
} // namespace ultraterimal::protocol
