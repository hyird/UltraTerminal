#include "avc.protocol.h"

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
std::expected<VideoFrameDescription, VideoEncodeError> describeAvcFrame(std::span<const std::uint8_t> bytes, const std::string& previousCodec) {
    if (bytes.empty() || bytes.size() > protocol::kMaximumMessageBytes - 1024) {
        return std::unexpected(VideoEncodeError::kFrameTooLarge);
    }
    const auto prefix = [&bytes](std::size_t at) -> std::size_t {
        if (at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0) {
            if (bytes[at + 2] == 1) {
                return 3;
            }
            if (at + 4 <= bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1) {
                return 4;
            }
        }
        return 0;
    };
    VideoFrameDescription description{previousCodec, false};
    bool sps = false, pps = false, slice = false, delta = false;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto start = prefix(at);
        if (!start || at + start == bytes.size()) {
            return std::unexpected(VideoEncodeError::kCodecFailure);
        }
        const auto payload = at + start;
        auto end = payload + 1;
        while (end < bytes.size() && !prefix(end)) {
            ++end;
        }
        const auto type = bytes[payload] & 31;
        if ((bytes[payload] & 128) || !type || type >= 24) {
            return std::unexpected(VideoEncodeError::kCodecFailure);
        }
        if (type == 7) {
            if (end - payload < 4 || slice) {
                return std::unexpected(VideoEncodeError::kCodecFailure);
            }
            constexpr char hex[] = "0123456789ABCDEF";
            std::string codec = "avc1.";
            for (std::size_t i = 1; i < 4; ++i) {
                codec += hex[bytes[payload + i] >> 4];
                codec += hex[bytes[payload + i] & 15];
            }
            if (sps && codec != description.codec) {
                return std::unexpected(VideoEncodeError::kCodecFailure);
            }
            description.codec = std::move(codec);
            sps = true;
        } else if (type == 8) {
            if (end - payload < 2 || slice) {
                return std::unexpected(VideoEncodeError::kCodecFailure);
            }
            pps = true;
        } else if (type == 5 || type == 1) {
            slice = true;
            description.keyFrame |= type == 5;
            delta |= type == 1;
        }
        at = end;
    }
    if (!slice || description.codec.empty() || (description.keyFrame && (!sps || !pps || delta)) ||
        (!description.keyFrame && description.codec != previousCodec)) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    return description;
}
} // namespace ultraterimal::agent
