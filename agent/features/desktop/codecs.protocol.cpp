#include "codecs.protocol.h"

#include <cstring>
#include <dav1d/dav1d.h>
#include <format>
#include <libde265/de265.h>
#include <memory>

#include <wels/codec_api.h>

#include "avc.protocol.h"

namespace ultraterimal::agent {
namespace {
std::size_t prefix(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 3 <= bytes.size() && !bytes[at] && !bytes[at + 1]) {
        if (bytes[at + 2] == 1) {
            return 3;
        }
        if (at + 4 <= bytes.size() && !bytes[at + 2] && bytes[at + 3] == 1) {
            return 4;
        }
    }
    return 0;
}
std::expected<VideoFrameDescription, VideoEncodeError> describeHevc(std::span<const std::uint8_t> bytes, const std::string& previous) {
    VideoFrameDescription result{previous, false};
    bool vps = false, sps = false, pps = false, slice = false, delta = false;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto start = prefix(bytes, at);
        const auto payload = at + start;
        if (!start || payload + 2 > bytes.size() || (bytes[payload] & 0x81) || !(bytes[payload + 1] & 7) || (bytes[payload + 1] >> 3)) {
            return std::unexpected(VideoEncodeError::kCodecFailure);
        }
        auto end = payload + 2;
        while (end < bytes.size() && !prefix(bytes, end)) {
            ++end;
        }
        const auto type = (bytes[payload] >> 1) & 63;
        if (type == 32) {
            vps = !slice;
        }
        if (type == 34) {
            pps = !slice;
        }
        if (type == 33) {
            if (slice) {
                return std::unexpected(VideoEncodeError::kCodecFailure);
            }
            std::vector<std::uint8_t> rbsp;
            unsigned zeros = 0;
            for (auto index = payload + 2; index < end; ++index) {
                const auto value = bytes[index];
                if (zeros >= 2 && value == 3) {
                    if (index + 1 == end || bytes[index + 1] > 3) {
                        return std::unexpected(VideoEncodeError::kCodecFailure);
                    }
                    zeros = 0;
                    continue;
                }
                rbsp.push_back(value);
                zeros = value ? 0 : zeros + 1;
            }
            if (rbsp.size() < 13) {
                return std::unexpected(VideoEncodeError::kCodecFailure);
            }
            const auto profile = rbsp[1] & 31;
            const auto space = rbsp[1] >> 6;
            const char tier = rbsp[1] & 32 ? 'H' : 'L';
            std::uint32_t compatibility = 0;
            for (unsigned bit = 0; bit < 32; ++bit) {
                compatibility |= static_cast<std::uint32_t>((rbsp[2 + bit / 8] >> (7 - bit % 8)) & 1) << bit;
            }
            std::string constraints;
            auto last = 12u;
            while (last > 6 && !rbsp[last - 1]) {
                --last;
            }
            for (auto index = 6u; index < last; ++index) {
                constraints += std::format(".{:X}", rbsp[index]);
            }
            const auto codec = std::format("hev1.{}{}.{:X}.{}{}{}", space ? std::string(1, static_cast<char>('A' + space - 1)) : std::string{}, profile, compatibility, tier, rbsp[12], constraints);
            if (sps && result.codec != codec) {
                return std::unexpected(VideoEncodeError::kCodecFailure);
            }
            result.codec = codec;
            sps = true;
        }
        if (type <= 31) {
            slice = true;
            const bool key = type >= 16 && type <= 21;
            result.keyFrame |= key;
            delta |= !key;
        }
        at = end;
    }
    if (!slice || result.codec.empty() || (result.keyFrame && (!vps || !sps || !pps || delta)) || (!result.keyFrame && result.codec != previous)) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    return result;
}
struct Av1Decoder {
    Dav1dContext* context = nullptr;
    Dav1dData data{};
    Dav1dPicture picture{};
    ~Av1Decoder() {
        dav1d_picture_unref(&picture);
        dav1d_data_unref(&data);
        if (context) {
            dav1d_close(&context);
        }
    }
};
} // namespace
std::expected<VideoFrameDescription, VideoEncodeError> describeVideoFrame(wire::VideoCodec codec, std::span<const std::uint8_t> bytes, const std::string& previousCodec, bool cleanPoint) {
    if (bytes.empty() || bytes.size() > protocol::kMaximumMessageBytes - 1024) {
        return std::unexpected(VideoEncodeError::kFrameTooLarge);
    }
    if (codec == wire::AVC) {
        return describeAvcFrame(bytes, previousCodec);
    }
    if (codec == wire::HEVC) {
        return describeHevc(bytes, previousCodec);
    }
    if (codec != wire::AV1) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    Dav1dSequenceHeader sequence{};
    const auto parsed = dav1d_parse_sequence_header(&sequence, bytes.data(), bytes.size());
    if (parsed == DAV1D_ERR(ENOENT) && !cleanPoint && !previousCodec.empty()) {
        return VideoFrameDescription{previousCodec, false};
    }
    if (parsed < 0 || sequence.profile != 0 || sequence.hbd || !sequence.num_operating_points ||
        sequence.max_width > 4096 || sequence.max_height > 2160) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    const auto& point = sequence.operating_points[0];
    if (point.major_level < 2 || point.major_level > 7 || point.minor_level > 3) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    const auto description = std::format("av01.0.{:02}{}.08", (point.major_level - 2) * 4 + point.minor_level, point.tier ? 'H' : 'M');
    if (!cleanPoint && description != previousCodec) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    return VideoFrameDescription{description, cleanPoint};
}
bool verifyVideoBaseline(wire::VideoCodec codec, std::span<const std::uint8_t> bytes, std::uint32_t width, std::uint32_t height) {
    if (bytes.empty() || bytes.size() > protocol::kMaximumMessageBytes || width > 4096 || height > 2160 || !width || !height) {
        return false;
    }
    if (codec == wire::HEVC) {
        std::unique_ptr<de265_decoder_context, decltype(&de265_free_decoder)> decoder(de265_new_decoder(), &de265_free_decoder);
        if (!decoder) {
            return false;
        }
        de265_set_parameter_bool(decoder.get(), DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 1);
        if (de265_push_data(decoder.get(), bytes.data(), static_cast<int>(bytes.size()), 0, nullptr) != DE265_OK || de265_flush_data(decoder.get()) != DE265_OK) {
            return false;
        }
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            int more = 0;
            if (de265_decode(decoder.get(), &more) != DE265_OK) {
                return false;
            }
            if (const auto* image = de265_get_next_picture(decoder.get())) {
                return de265_get_image_width(image, 0) == static_cast<int>(width) && de265_get_image_height(image, 0) == static_cast<int>(height) && de265_get_bits_per_pixel(image, 0) == 8;
            }
            if (!more) {
                break;
            }
        }
        return false;
    }
    if (codec == wire::AV1) {
        Av1Decoder decoder;
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = 1;
        settings.max_frame_delay = 1;
        settings.frame_size_limit = width * height;
        settings.logger.callback = nullptr;
        settings.strict_std_compliance = 1;
        if (dav1d_open(&decoder.context, &settings) < 0) {
            return false;
        }
        auto* data = dav1d_data_create(&decoder.data, bytes.size());
        if (!data) {
            return false;
        }
        std::memcpy(data, bytes.data(), bytes.size());
        if (dav1d_send_data(decoder.context, &decoder.data) < 0 || dav1d_get_picture(decoder.context, &decoder.picture) < 0) {
            return false;
        }
        return decoder.picture.p.w == static_cast<int>(width) && decoder.picture.p.h == static_cast<int>(height) && decoder.picture.p.bpc == 8 && decoder.picture.frame_hdr && decoder.picture.frame_hdr->frame_type == DAV1D_FRAME_TYPE_KEY;
    }
    if (codec != wire::AVC) {
        return false;
    }
    ISVCDecoder* raw = nullptr;
    if (WelsCreateDecoder(&raw) != 0 || !raw) {
        return false;
    }
    struct Decoder {
        ISVCDecoder* value;
        ~Decoder() {
            value->Uninitialize();
            WelsDestroyDecoder(value);
        }
    } decoder{raw};
    SDecodingParam settings{};
    settings.eEcActiveIdc = ERROR_CON_DISABLE;
    settings.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (raw->Initialize(&settings) != 0) {
        return false;
    }
    unsigned char* planes[3]{};
    SBufferInfo output{};
    return raw->DecodeFrameNoDelay(bytes.data(), static_cast<int>(bytes.size()), planes, &output) == dsErrorFree && output.iBufferStatus == 1 &&
           output.UsrData.sSystemBuffer.iWidth == static_cast<int>(width) && output.UsrData.sSystemBuffer.iHeight == static_cast<int>(height);
}
} // namespace ultraterimal::agent
