#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

#include "features/desktop/png.protocol.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint32_t readUint32Be(const std::string& data, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}
} // namespace

int main() {
    using namespace ultraterimal::agent;
    try {
        constexpr std::uint32_t srcW = 16;
        constexpr std::uint32_t srcH = 8;
        std::vector<std::uint8_t> bgra(srcW * srcH * 4, 0);
        for (std::size_t i = 0; i < srcW * srcH; ++i) {
            bgra[i * 4 + 0] = 10;  // B
            bgra[i * 4 + 1] = 200; // G
            bgra[i * 4 + 2] = 50;  // R
            bgra[i * 4 + 3] = 255; // A
        }

        std::uint32_t outW = 0;
        std::uint32_t outH = 0;
        const auto png = encodeBgraPng(bgra, srcW, srcH, 8, 8, outW, outH);
        require(png.has_value(), "PNG encoding failed");
        require(outW == 8 && outH == 4, "Aspect-preserving downscale failed");
        require(png->size() > 33 && png->substr(0, 8) == "\x89PNG\r\n\x1a\n", "Missing PNG signature");
        require(png->substr(12, 4) == "IHDR" && readUint32Be(*png, 16) == 8 && readUint32Be(*png, 20) == 4, "Invalid PNG IHDR");
        require(png->find("IDAT") != std::string::npos && png->find("IEND") != std::string::npos, "Missing PNG IDAT/IEND chunks");

        const std::size_t idatPos = png->find("IDAT");
        const std::uint32_t idatLen = readUint32Be(*png, idatPos - 4);
        std::vector<std::uint8_t> raw(4 * (8 * 3 + 1), 0);
        uLongf rawLen = static_cast<uLongf>(raw.size());
        const int zOk = uncompress(raw.data(), &rawLen,
                                   reinterpret_cast<const Bytef*>(png->data() + idatPos + 4),
                                   idatLen);
        require(zOk == Z_OK && rawLen == raw.size(), "PNG IDAT decompression failed");
        require(raw[0] == 0 && raw[1] == 50 && raw[2] == 200 && raw[3] == 10, "BGRA->RGB channel mapping mismatch");

        std::cout << "PNG downscale and compression checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
