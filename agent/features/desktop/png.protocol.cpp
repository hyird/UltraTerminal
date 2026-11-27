#include "features/desktop/png.protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <zlib.h>

namespace ultraterimal::agent {
namespace {
void appendUint32Be(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void appendPngChunk(std::string& out, std::string_view type, std::span<const std::uint8_t> payload) {
    appendUint32Be(out, static_cast<std::uint32_t>(payload.size()));
    out.append(type.data(), 4);
    if (!payload.empty()) {
        out.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type.data()), 4);
    if (!payload.empty()) {
        crc = crc32(crc, reinterpret_cast<const Bytef*>(payload.data()), static_cast<uInt>(payload.size()));
    }
    appendUint32Be(out, static_cast<std::uint32_t>(crc));
}
} // namespace

ScaledRgbImage downscaleBgraToRgb(std::span<const std::uint8_t> bgra, std::uint32_t srcWidth, std::uint32_t srcHeight,
                                  std::uint32_t maxWidth, std::uint32_t maxHeight) {
    if (!srcWidth || !srcHeight || srcWidth > 16384 || srcHeight > 16384) {
        throw std::invalid_argument("Invalid source image dimensions");
    }
    const std::size_t expectedBytes = static_cast<std::size_t>(srcWidth) * srcHeight * 4;
    if (bgra.size() < expectedBytes) {
        throw std::invalid_argument("Incomplete BGRA buffer");
    }

    const std::uint32_t boundW = maxWidth ? maxWidth : srcWidth;
    const std::uint32_t boundH = maxHeight ? maxHeight : srcHeight;
    double scale = 1.0;
    if (srcWidth > boundW || srcHeight > boundH) {
        scale = std::min(static_cast<double>(boundW) / static_cast<double>(srcWidth),
                         static_cast<double>(boundH) / static_cast<double>(srcHeight));
    }
    const std::uint32_t dstWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<double>(srcWidth) * scale)));
    const std::uint32_t dstHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<double>(srcHeight) * scale)));

    ScaledRgbImage out;
    out.width = dstWidth;
    out.height = dstHeight;
    out.rgb.resize(static_cast<std::size_t>(dstWidth) * dstHeight * 3);

    if (dstWidth == srcWidth && dstHeight == srcHeight) {
        for (std::uint32_t y = 0; y < srcHeight; ++y) {
            const std::uint8_t* srcRow = bgra.data() + static_cast<std::size_t>(y) * srcWidth * 4;
            std::uint8_t* dstRow = out.rgb.data() + static_cast<std::size_t>(y) * dstWidth * 3;
            for (std::uint32_t x = 0; x < srcWidth; ++x) {
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0];
            }
        }
        return out;
    }

    for (std::uint32_t dy = 0; dy < dstHeight; ++dy) {
        const std::uint32_t sy0 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(dy) * srcHeight) / dstHeight);
        const std::uint32_t sy1 = std::max(sy0 + 1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(dy + 1) * srcHeight + dstHeight - 1) / dstHeight));
        const std::uint32_t syEnd = std::min(srcHeight, sy1);
        std::uint8_t* dstRow = out.rgb.data() + static_cast<std::size_t>(dy) * dstWidth * 3;
        for (std::uint32_t dx = 0; dx < dstWidth; ++dx) {
            const std::uint32_t sx0 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(dx) * srcWidth) / dstWidth);
            const std::uint32_t sx1 = std::max(sx0 + 1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(dx + 1) * srcWidth + dstWidth - 1) / dstWidth));
            const std::uint32_t sxEnd = std::min(srcWidth, sx1);
            std::uint64_t sumR = 0;
            std::uint64_t sumG = 0;
            std::uint64_t sumB = 0;
            std::uint64_t count = 0;
            for (std::uint32_t sy = sy0; sy < syEnd; ++sy) {
                const std::uint8_t* srcRow = bgra.data() + static_cast<std::size_t>(sy) * srcWidth * 4;
                for (std::uint32_t sx = sx0; sx < sxEnd; ++sx) {
                    sumB += srcRow[sx * 4 + 0];
                    sumG += srcRow[sx * 4 + 1];
                    sumR += srcRow[sx * 4 + 2];
                    ++count;
                }
            }
            dstRow[dx * 3 + 0] = static_cast<std::uint8_t>(sumR / count);
            dstRow[dx * 3 + 1] = static_cast<std::uint8_t>(sumG / count);
            dstRow[dx * 3 + 2] = static_cast<std::uint8_t>(sumB / count);
        }
    }
    return out;
}

std::expected<std::string, std::string> encodeRgbPng(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384) {
        return std::unexpected(std::string("Invalid PNG dimensions"));
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 3;
    if (rgb.size() != rowBytes * height) {
        return std::unexpected(std::string("RGB buffer size mismatch"));
    }

    std::vector<std::uint8_t> scanlines((rowBytes + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t dstOffset = static_cast<std::size_t>(y) * (rowBytes + 1);
        scanlines[dstOffset] = 0; // Filter type 0 (None)
        std::copy_n(rgb.data() + static_cast<std::size_t>(y) * rowBytes, rowBytes, scanlines.data() + dstOffset + 1);
    }

    uLongf compressedCapacity = compressBound(static_cast<uLong>(scanlines.size()));
    std::vector<std::uint8_t> compressed(compressedCapacity);
    const int zStatus = compress2(compressed.data(), &compressedCapacity, scanlines.data(), static_cast<uLong>(scanlines.size()), Z_DEFAULT_COMPRESSION);
    if (zStatus != Z_OK) {
        return std::unexpected(std::string("zlib compression failed"));
    }
    compressed.resize(compressedCapacity);

    std::string png;
    png.reserve(64 + compressed.size());
    static constexpr std::array<char, 8> kSignature{'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    png.append(kSignature.data(), kSignature.size());

    std::array<std::uint8_t, 13> ihdr{};
    ihdr[0] = static_cast<std::uint8_t>((width >> 24) & 0xff);
    ihdr[1] = static_cast<std::uint8_t>((width >> 16) & 0xff);
    ihdr[2] = static_cast<std::uint8_t>((width >> 8) & 0xff);
    ihdr[3] = static_cast<std::uint8_t>(width & 0xff);
    ihdr[4] = static_cast<std::uint8_t>((height >> 24) & 0xff);
    ihdr[5] = static_cast<std::uint8_t>((height >> 16) & 0xff);
    ihdr[6] = static_cast<std::uint8_t>((height >> 8) & 0xff);
    ihdr[7] = static_cast<std::uint8_t>(height & 0xff);
    ihdr[8] = 8;  // Bit depth
    ihdr[9] = 2;  // Color type: RGB
    ihdr[10] = 0; // Compression
    ihdr[11] = 0; // Filter
    ihdr[12] = 0; // Interlace
    appendPngChunk(png, "IHDR", ihdr);
    appendPngChunk(png, "IDAT", compressed);
    appendPngChunk(png, "IEND", {});
    return png;
}

std::expected<std::string, std::string> encodeBgraPng(std::span<const std::uint8_t> bgra, std::uint32_t srcWidth, std::uint32_t srcHeight,
                                                      std::uint32_t maxWidth, std::uint32_t maxHeight,
                                                      std::uint32_t& outWidth, std::uint32_t& outHeight) {
    try {
        const auto scaled = downscaleBgraToRgb(bgra, srcWidth, srcHeight, maxWidth, maxHeight);
        outWidth = scaled.width;
        outHeight = scaled.height;
        return encodeRgbPng(scaled.rgb, scaled.width, scaled.height);
    } catch (const std::exception& error) {
        return std::unexpected(std::string(error.what()));
    }
}
} // namespace ultraterimal::agent
