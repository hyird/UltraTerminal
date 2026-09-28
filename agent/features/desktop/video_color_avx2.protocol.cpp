#include "video_color_avx2.protocol.h"

#include <cstring>
#include <immintrin.h>

namespace ultraterimal::agent::detail {
namespace {
__m256i matrixPair(__m256i pixels, __m256i coefficients) {
    const auto products = _mm256_madd_epi16(pixels, coefficients);
    const auto sums = _mm256_add_epi32(products, _mm256_shuffle_epi32(products, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm256_shuffle_epi32(sums, _MM_SHUFFLE(2, 0, 2, 0));
}
void storeLuma(__m256i pixels, std::uint8_t* output) {
    const auto zero = _mm256_setzero_si256();
    const auto coefficients = _mm256_setr_epi16(25, 129, 66, 0, 25, 129, 66, 0, 25, 129, 66, 0, 25, 129, 66, 0);
    const auto low = matrixPair(_mm256_unpacklo_epi8(pixels, zero), coefficients);
    const auto high = matrixPair(_mm256_unpackhi_epi8(pixels, zero), coefficients);
    const auto values = _mm256_add_epi32(_mm256_srli_epi32(_mm256_add_epi32(_mm256_unpacklo_epi64(low, high), _mm256_set1_epi32(128)), 8), _mm256_set1_epi32(16));
    const auto words = _mm256_packs_epi32(values, zero);
    const auto ordered = _mm_unpacklo_epi64(_mm256_castsi256_si128(words), _mm256_extracti128_si256(words, 1));
    const auto bytes = _mm_packus_epi16(ordered, _mm_setzero_si128());
    const auto packed = static_cast<std::uint64_t>(_mm_cvtsi128_si64(bytes));
    std::memcpy(output, &packed, sizeof(packed));
}
__m256i roundedChroma(__m256i top, __m256i bottom, bool high) {
    const auto zero = _mm256_setzero_si256();
    const auto rows = high ? _mm256_add_epi16(_mm256_unpackhi_epi8(top, zero), _mm256_unpackhi_epi8(bottom, zero))
                           : _mm256_add_epi16(_mm256_unpacklo_epi8(top, zero), _mm256_unpacklo_epi8(bottom, zero));
    return _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(rows, _mm256_srli_si256(rows, 8)), _mm256_set1_epi16(2)), 2);
}
__m128i chromaBytes(__m256i pixels, __m256i coefficients) {
    const auto values = _mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32(matrixPair(pixels, coefficients), _mm256_set1_epi32(128)), 8), _mm256_set1_epi32(128));
    const auto bytes = _mm256_packus_epi16(_mm256_packs_epi32(values, _mm256_setzero_si256()), _mm256_setzero_si256());
    return _mm_unpacklo_epi16(_mm256_castsi256_si128(bytes), _mm256_extracti128_si256(bytes, 1));
}
} // namespace
// Keep the ISA-specific body out of baseline callers, including under LTCG.
__declspec(noinline) void convertVideoPixelsAvx2(const std::uint8_t* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height, VideoPixelFormat format, std::uint8_t* output) {
    const auto pixels = static_cast<std::size_t>(width) * height;
    auto* uPlane = output + pixels;
    auto* vPlane = uPlane + (format == VideoPixelFormat::kI420 ? pixels / 4 : 1);
    const std::size_t chromaStep = format == VideoPixelFormat::kI420 ? 1 : 2;
    for (std::size_t y = 0; y < height; y += 2) {
        const auto* top = bgra + y * stride;
        const auto* bottom = top + stride;
        auto* topLuma = output + y * width;
        auto* bottomLuma = topLuma + width;
        std::size_t x = 0;
        // Fuse luma and chroma so the same two row loads serve both planes.
        for (; x + 8 <= width; x += 8) {
            const auto first = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(top + x * 4));
            const auto second = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bottom + x * 4));
            storeLuma(first, topLuma + x);
            storeLuma(second, bottomLuma + x);
            const auto averaged = _mm256_unpacklo_epi64(roundedChroma(first, second, false), roundedChroma(first, second, true));
            const auto u = chromaBytes(averaged, _mm256_setr_epi16(112, -74, -38, 0, 112, -74, -38, 0, 112, -74, -38, 0, 112, -74, -38, 0));
            const auto v = chromaBytes(averaged, _mm256_setr_epi16(-18, -94, 112, 0, -18, -94, 112, 0, -18, -94, 112, 0, -18, -94, 112, 0));
            const auto index = ((y / 2) * (width / 2) + x / 2) * chromaStep;
            if (format == VideoPixelFormat::kI420) {
                const auto packedU = static_cast<std::uint32_t>(_mm_cvtsi128_si32(u));
                const auto packedV = static_cast<std::uint32_t>(_mm_cvtsi128_si32(v));
                std::memcpy(uPlane + index, &packedU, sizeof(packedU));
                std::memcpy(vPlane + index, &packedV, sizeof(packedV));
            } else {
                const auto packed = static_cast<std::uint64_t>(_mm_cvtsi128_si64(_mm_unpacklo_epi8(u, v)));
                std::memcpy(uPlane + index, &packed, sizeof(packed));
            }
        }
        for (; x < width; x += 2) {
            const auto* first = top + x * 4;
            const auto* second = bottom + x * 4;
            for (std::size_t offset = 0; offset < 2; ++offset) {
                const auto* a = first + offset * 4;
                const auto* b = second + offset * 4;
                topLuma[x + offset] = static_cast<std::uint8_t>(((66 * a[2] + 129 * a[1] + 25 * a[0] + 128) >> 8) + 16);
                bottomLuma[x + offset] = static_cast<std::uint8_t>(((66 * b[2] + 129 * b[1] + 25 * b[0] + 128) >> 8) + 16);
            }
            const int red = (first[2] + first[6] + second[2] + second[6] + 2) / 4;
            const int green = (first[1] + first[5] + second[1] + second[5] + 2) / 4;
            const int blue = (first[0] + first[4] + second[0] + second[4] + 2) / 4;
            const auto index = ((y / 2) * (width / 2) + x / 2) * chromaStep;
            uPlane[index] = static_cast<std::uint8_t>(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
            vPlane[index] = static_cast<std::uint8_t>(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
        }
    }
    _mm256_zeroupper();
}
} // namespace ultraterimal::agent::detail
