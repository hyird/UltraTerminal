#include "video_color.protocol.h"

#include <cstring>
#include <emmintrin.h>

namespace ultraterimal::agent {
namespace {
// Sum the two channel pairs of each BGRA pixel into adjacent 32-bit lanes.
__m128i matrixPair(__m128i pixels, __m128i coefficients) {
    const auto products = _mm_madd_epi16(pixels, coefficients);
    const auto sums = _mm_add_epi32(products, _mm_shuffle_epi32(products, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_shuffle_epi32(sums, _MM_SHUFFLE(2, 0, 2, 0));
}
__m128i roundedChroma(__m128i top, __m128i bottom, bool high) {
    const auto zero = _mm_setzero_si128();
    const auto rows = high ? _mm_add_epi16(_mm_unpackhi_epi8(top, zero), _mm_unpackhi_epi8(bottom, zero))
                           : _mm_add_epi16(_mm_unpacklo_epi8(top, zero), _mm_unpacklo_epi8(bottom, zero));
    return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(rows, _mm_srli_si128(rows, 8)), _mm_set1_epi16(2)), 2);
}
__m128i chromaBytes(__m128i pixels, __m128i coefficients) {
    const auto values = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(matrixPair(pixels, coefficients), _mm_set1_epi32(128)), 8), _mm_set1_epi32(128));
    return _mm_packus_epi16(_mm_packs_epi32(values, _mm_setzero_si128()), _mm_setzero_si128());
}
} // namespace
// BT.601 limited-range matrix; codec adapters own color signaling.
bool convertVideoPixels(std::span<const std::uint8_t> bgra, std::size_t stride,
                        std::uint32_t width, std::uint32_t height, VideoPixelFormat format, std::span<std::uint8_t> output) {
    if (!width || !height || width % 2 || height % 2 || width > 4096 || height > 4096 ||
        static_cast<std::uint64_t>(width) * height > 4096 * 2160 ||
        stride < static_cast<std::size_t>(width) * 4 || stride > 64 * 1024 * 1024 / height ||
        bgra.size() < stride * height || output.size() < static_cast<std::size_t>(width) * height * 3 / 2 ||
        (format != VideoPixelFormat::kI420 && format != VideoPixelFormat::kNv12)) {
        return false;
    }
    const auto pixels = static_cast<std::size_t>(width) * height;
    auto* yPlane = output.data();
    auto* uPlane = yPlane + pixels;
    auto* vPlane = uPlane + (format == VideoPixelFormat::kI420 ? pixels / 4 : 1);
    const std::size_t chromaStep = format == VideoPixelFormat::kI420 ? 1 : 2;
    // Keep luma linear so the compiler can vectorize it independently of the
    // 2x2 chroma reduction. The matrix and rounding remain identical.
    for (std::size_t y = 0; y < height; ++y) {
        const auto* row = bgra.data() + y * stride;
        auto* luma = yPlane + y * width;
        std::size_t x = 0;
        const auto zero = _mm_setzero_si128();
        const auto coefficients = _mm_setr_epi16(25, 129, 66, 0, 25, 129, 66, 0);
        for (; x + 4 <= width; x += 4) {
            const auto pixels4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x * 4));
            const auto first = matrixPair(_mm_unpacklo_epi8(pixels4, zero), coefficients);
            const auto second = matrixPair(_mm_unpackhi_epi8(pixels4, zero), coefficients);
            const auto values = _mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(_mm_unpacklo_epi64(first, second), _mm_set1_epi32(128)), 8), _mm_set1_epi32(16));
            const auto packed = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(values, zero), zero)));
            std::memcpy(luma + x, &packed, sizeof(packed));
        }
        for (; x < width; ++x) {
            const auto* pixel = row + x * 4;
            // Byte inputs give Y in [16,235] and U/V in [16,240]. No
            // saturation is needed, including after rounded chroma averaging.
            luma[x] = static_cast<std::uint8_t>(((66 * pixel[2] + 129 * pixel[1] + 25 * pixel[0] + 128) >> 8) + 16);
        }
    }
    for (std::size_t y = 0; y < height; y += 2) {
        const auto* first = bgra.data() + y * stride;
        const auto* second = first + stride;
        std::size_t x = 0;
        for (; x + 4 <= width; x += 4) {
            const auto top4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(first + x * 4));
            const auto bottom4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(second + x * 4));
            const auto averaged = _mm_unpacklo_epi64(roundedChroma(top4, bottom4, false), roundedChroma(top4, bottom4, true));
            const auto u = chromaBytes(averaged, _mm_setr_epi16(112, -74, -38, 0, 112, -74, -38, 0));
            const auto v = chromaBytes(averaged, _mm_setr_epi16(-18, -94, 112, 0, -18, -94, 112, 0));
            const auto index = ((y / 2) * (width / 2) + x / 2) * chromaStep;
            if (format == VideoPixelFormat::kI420) {
                const auto packedU = static_cast<std::uint16_t>(_mm_cvtsi128_si32(u));
                const auto packedV = static_cast<std::uint16_t>(_mm_cvtsi128_si32(v));
                std::memcpy(uPlane + index, &packedU, sizeof(packedU));
                std::memcpy(vPlane + index, &packedV, sizeof(packedV));
            } else {
                const auto packed = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_unpacklo_epi8(u, v)));
                std::memcpy(uPlane + index, &packed, sizeof(packed));
            }
        }
        for (; x < width; x += 2) {
            const auto* top = first + x * 4;
            const auto* bottom = second + x * 4;
            const int red = (top[2] + top[6] + bottom[2] + bottom[6] + 2) / 4;
            const int green = (top[1] + top[5] + bottom[1] + bottom[5] + 2) / 4;
            const int blue = (top[0] + top[4] + bottom[0] + bottom[4] + 2) / 4;
            const auto index = ((y / 2) * (width / 2) + x / 2) * chromaStep;
            uPlane[index] = static_cast<std::uint8_t>(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
            vPlane[index] = static_cast<std::uint8_t>(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
        }
    }
    return true;
}
} // namespace ultraterimal::agent
