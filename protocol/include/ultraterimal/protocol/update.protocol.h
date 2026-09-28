#pragma once
#include <array>
#include <charconv>
#include <optional>
#include <string_view>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::size_t kUpdateChunkBytes = 32 * 1024;
inline constexpr std::uint64_t kMaximumUpdateBytes = 256 * 1024 * 1024;
inline std::optional<std::array<unsigned, 3>> updateVersion(std::string_view text) {
    std::array<unsigned, 3> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto delimiter = text.find('.');
        const auto part = text.substr(0, delimiter);
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part.front() == '0')) {
            return {};
        }
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || result[i] > 65535 ||
            (i < 2 && delimiter == text.npos) || (i == 2 && delimiter != text.npos)) {
            return {};
        }
        text.remove_prefix(part.size() + (delimiter == text.npos ? 0 : 1));
    }
    return result;
}
inline bool validUpdate(const wire::AgentUpdate& update) {
    return updateVersion(update.version()).has_value() && update.sha256().size() == 64 &&
           update.sha256().find_first_not_of("0123456789abcdef") == std::string::npos &&
           update.size() > 0 && update.size() <= kMaximumUpdateBytes;
}
inline bool newerUpdate(const wire::AgentUpdate& update, std::string_view current) {
    const auto target = updateVersion(update.version()), installed = updateVersion(current);
    return validUpdate(update) && target && installed && *target > *installed;
}
inline bool validUpdateChunk(const wire::AgentUpdateChunk& chunk, std::uint64_t offset, std::uint64_t total) {
    return offset < total && chunk.offset() == offset && !chunk.data().empty() &&
           chunk.data().size() <= kUpdateChunkBytes && chunk.data().size() <= total - offset;
}
} // namespace ultraterimal::protocol
