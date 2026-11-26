#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>

#include "features/desktop/video_color.protocol.h"

int main() {
    using namespace ultraterimal::agent;
    const auto require = [](bool value) { if (!value) { throw std::runtime_error("Video color conversion invariant failed"); } };
    // A guard page catches vector overreads/overwrites that ordinary heap
    // sentinels cannot detect. Exercise tight and unaligned padded rows.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page = static_cast<std::size_t>(system.dwPageSize);
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(memory != nullptr);
    struct Pages {
        void* memory;
        ~Pages() { VirtualFree(memory, 0, MEM_RELEASE); }
    } pages{memory};
    DWORD previous = 0;
    require(VirtualProtect(memory + page, page, PAGE_NOACCESS, &previous) != FALSE);
    require(VirtualProtect(memory + page * 3, page, PAGE_NOACCESS, &previous) != FALSE);
    for (const auto backend : {VideoPixelBackend::kAutomatic, VideoPixelBackend::kSse2, VideoPixelBackend::kAvx2}) {
        if (backend == VideoPixelBackend::kAvx2 && !videoPixelAvx2Available()) {
            continue;
        }
        for (const unsigned width : {2u, 6u, 8u, 10u, 14u, 16u, 18u, 30u, 32u, 34u}) {
            for (const unsigned padding : {0u, 1u, 13u}) {
                const auto stride = width * 4 + padding;
                std::span input(memory + page - stride * 2, stride * 2);
                std::span output(memory + page * 3 - width * 3, width * 3);
                std::ranges::fill(input, 255);
                for (const auto format : {VideoPixelFormat::kI420, VideoPixelFormat::kNv12}) {
                    require(convertVideoPixels(input, stride, width, 2, format, output, backend));
                    require(std::ranges::all_of(output.first(width * 2), [](auto value) { return value == 235; }));
                    require(std::ranges::all_of(output.subspan(width * 2), [](auto value) { return value == 128; }));
                }
            }
        }
    }
    if (videoPixelAvx2Available()) {
        for (const auto [width, height] : {std::pair{1920u, 1080u}, {2560u, 1600u}, {3840u, 2160u}, {4096u, 2160u}, {2u, 4096u}}) {
            const auto stride = width * 4 + 7;
            std::vector<std::uint8_t> input(stride * height + 1), baseline(width * height * 3 / 2), vectorized(baseline.size());
            std::uint32_t random = 0xb539d1a7;
            for (auto& value : input) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                value = static_cast<std::uint8_t>(random);
            }
            for (const auto format : {VideoPixelFormat::kI420, VideoPixelFormat::kNv12}) {
                require(convertVideoPixels(std::span(input).subspan(1), stride, width, height, format, baseline, VideoPixelBackend::kSse2));
                require(convertVideoPixels(std::span(input).subspan(1), stride, width, height, format, vectorized, VideoPixelBackend::kAvx2));
                require(baseline == vectorized);
            }
        }
    }
    for (const auto backend : {VideoPixelBackend::kAutomatic, VideoPixelBackend::kSse2, VideoPixelBackend::kAvx2}) {
        if (backend == VideoPixelBackend::kAvx2 && !videoPixelAvx2Available()) {
            std::array<std::uint8_t, 16> pixels{};
            std::array<std::uint8_t, 6> untouched{};
            untouched.fill(55);
            require(!convertVideoPixels(pixels, 8, 2, 2, VideoPixelFormat::kI420, untouched, backend));
            require(std::ranges::all_of(untouched, [](auto value) { return value == 55; }));
            continue;
        }
        std::array<std::uint8_t, 40> bgra{};
        bgra.fill(0xee);
        for (std::size_t y = 0; y < 2; ++y) {
            for (std::size_t x = 0; x < 4; ++x) {
                auto* pixel = bgra.data() + y * 20 + x * 4;
                pixel[0] = 0;
                pixel[1] = x < 2 ? 0 : 255;
                pixel[2] = x < 2 ? 255 : 0;
                pixel[3] = 77; // Alpha is not part of desktop video color conversion.
            }
        }
        constexpr std::array<std::uint8_t, 12> i420{82, 82, 144, 144, 82, 82, 144, 144, 90, 54, 240, 34};
        // Use different U/V values for each chroma sample to verify interleaving.
        constexpr std::array<std::uint8_t, 12> nv12{82, 82, 144, 144, 82, 82, 144, 144, 90, 240, 54, 34};
        std::array<std::uint8_t, 14> output{};
        for (const auto format : {VideoPixelFormat::kI420, VideoPixelFormat::kNv12}) {
            output.fill(55);
            require(convertVideoPixels(bgra, 20, 4, 2, format, output, backend));
            const auto& expected = format == VideoPixelFormat::kI420 ? i420 : nv12;
            require(std::equal(expected.begin(), expected.end(), output.begin()));
            require(output[12] == 55 && output[13] == 55);
        }
        output.fill(55);
        require(!convertVideoPixels(bgra, 20, 3, 2, VideoPixelFormat::kI420, output, backend));
        require(!convertVideoPixels(bgra, 15, 4, 2, VideoPixelFormat::kI420, output, backend));
        require(!convertVideoPixels(bgra, std::numeric_limits<std::size_t>::max(), 4, 2, VideoPixelFormat::kI420, output, backend));
        require(!convertVideoPixels(std::span(bgra).first(39), 20, 4, 2, VideoPixelFormat::kI420, output, backend));
        require(!convertVideoPixels(bgra, 20, 4, 2, VideoPixelFormat::kNv12, std::span(output).first(11), backend));
        require(!convertVideoPixels(bgra, 20, 4, 2, VideoPixelFormat::kI420, output, static_cast<VideoPixelBackend>(999)));
        require(std::ranges::all_of(output, [](auto value) { return value == 55; }));
        // Mixed 2x2 colors, unaligned spans, row padding, and two-pixel tails must
        // retain scalar rounding and never touch the surrounding guard bytes.
        std::uint32_t random = 0x19e3779b;
        for (const unsigned testWidth : {2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u, 18u, 30u, 32u, 62u, 66u, 130u}) {
            for (const unsigned testHeight : {2u, 6u, 10u}) {
                const auto testStride = testWidth * 4 + 13;
                std::vector<std::uint8_t> input(testStride * testHeight + 1);
                for (auto& value : input) {
                    random ^= random << 13;
                    random ^= random >> 17;
                    random ^= random << 5;
                    value = static_cast<std::uint8_t>(random);
                }
                const auto inputPixels = std::span(input).subspan(1);
                const auto pixelCount = testWidth * testHeight;
                for (const auto format : {VideoPixelFormat::kI420, VideoPixelFormat::kNv12}) {
                    std::vector<std::uint8_t> actual(pixelCount * 3 / 2 + 8, 0xa7), expected(pixelCount * 3 / 2);
                    for (unsigned y = 0; y < testHeight; ++y) {
                        for (unsigned x = 0; x < testWidth; ++x) {
                            const auto* pixel = inputPixels.data() + y * testStride + x * 4;
                            expected[y * testWidth + x] = static_cast<std::uint8_t>(((66 * pixel[2] + 129 * pixel[1] + 25 * pixel[0] + 128) >> 8) + 16);
                        }
                    }
                    for (unsigned y = 0; y < testHeight; y += 2) {
                        for (unsigned x = 0; x < testWidth; x += 2) {
                            const auto* top = inputPixels.data() + y * testStride + x * 4;
                            const auto* bottom = top + testStride;
                            const int red = (top[2] + top[6] + bottom[2] + bottom[6] + 2) / 4;
                            const int green = (top[1] + top[5] + bottom[1] + bottom[5] + 2) / 4;
                            const int blue = (top[0] + top[4] + bottom[0] + bottom[4] + 2) / 4;
                            const auto chroma = y / 2 * (testWidth / 2) + x / 2;
                            const auto u = static_cast<std::uint8_t>(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
                            const auto v = static_cast<std::uint8_t>(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
                            if (format == VideoPixelFormat::kI420) {
                                expected[pixelCount + chroma] = u;
                                expected[pixelCount * 5 / 4 + chroma] = v;
                            } else {
                                expected[pixelCount + chroma * 2] = u;
                                expected[pixelCount + chroma * 2 + 1] = v;
                            }
                        }
                    }
                    require(convertVideoPixels(inputPixels, testStride, testWidth, testHeight, format, std::span(actual).subspan(3, expected.size()), backend));
                    require(std::equal(expected.begin(), expected.end(), actual.begin() + 3));
                    require(std::ranges::all_of(std::span(actual).first(3), [](auto value) { return value == 0xa7; }));
                    require(std::ranges::all_of(std::span(actual).last(5), [](auto value) { return value == 0xa7; }));
                }
            }
        }
        // Exhaust all 24-bit RGB values against the previous saturated matrix.
        // Uniform 2x2 blocks also cover every possible rounded chroma average.
        constexpr std::size_t width = 512, height = 512, stride = width * 4 + 16;
        std::vector<std::uint8_t> source(stride * height, 77), converted(width * height * 3 / 2);
        const auto saturated = [](int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); };
        for (int red = 0; red < 256; ++red) {
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    auto* pixel = source.data() + y * stride + x * 4;
                    pixel[0] = static_cast<std::uint8_t>(y / 2);
                    pixel[1] = static_cast<std::uint8_t>(x / 2);
                    pixel[2] = static_cast<std::uint8_t>(red);
                }
            }
            for (const auto format : {VideoPixelFormat::kI420, VideoPixelFormat::kNv12}) {
                require(convertVideoPixels(source, stride, width, height, format, converted, backend));
                std::vector<std::uint8_t> sliced(converted.size(), 0xcc);
                for (std::uint32_t slice = 0; slice < 8; ++slice) {
                    const auto rowBegin = (height / 2 * slice / 8) * 2;
                    const auto rowEnd = (height / 2 * (slice + 1) / 8) * 2;
                    require(convertVideoPixelRows(source, stride, width, height, static_cast<std::uint32_t>(rowBegin), static_cast<std::uint32_t>(rowEnd), format, sliced, backend));
                }
                require(sliced == converted);
                require(!convertVideoPixelRows(source, stride, width, height, 1, 4, format, sliced, backend));
                require(!convertVideoPixelRows(source, stride, width, height, 4, 2, format, sliced, backend));
                require(!convertVideoPixelRows(source, stride, width, height, 0, height + 2, format, sliced, backend));
                for (int blue = 0; blue < 256; ++blue) {
                    for (int green = 0; green < 256; ++green) {
                        const auto luma = saturated(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
                        const auto u = saturated(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
                        const auto v = saturated(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
                        const auto pixel = static_cast<std::size_t>(blue) * 2 * width + green * 2;
                        require(converted[pixel] == luma && converted[pixel + 1] == luma &&
                                converted[pixel + width] == luma && converted[pixel + width + 1] == luma);
                        const auto chroma = static_cast<std::size_t>(blue) * 256 + green;
                        if (format == VideoPixelFormat::kI420) {
                            require(converted[width * height + chroma] == u && converted[width * height * 5 / 4 + chroma] == v);
                        } else {
                            require(converted[width * height + chroma * 2] == u && converted[width * height + chroma * 2 + 1] == v);
                        }
                    }
                }
            }
        }
    }
}
