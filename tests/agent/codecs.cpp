#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "features/desktop/codecs.protocol.h"

int main(int argc, char** argv) {
    using namespace ultraterimal;
    try {
        if (argc != 3) {
            throw std::runtime_error("Expected HEVC and AV1 fixtures");
        }
        for (int index = 1; index < argc; ++index) {
            std::ifstream input(argv[index], std::ios::binary);
            const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
            const auto family = index == 1 ? wire::HEVC : wire::AV1;
            const auto frame = agent::describeVideoFrame(family, bytes, "", true);
            if (!frame || !frame->keyFrame || !agent::verifyVideoBaseline(family, bytes, 320, 240)) {
                throw std::runtime_error("Reference codec keyframe did not independently decode: " + std::to_string(index));
            }
            if (agent::verifyVideoBaseline(family, bytes, 640, 480) || agent::verifyVideoBaseline(family, std::span(bytes).first(8), 320, 240)) {
                throw std::runtime_error("Wrong geometry or truncated codec baseline accepted");
            }
            std::cout << "Verified " << frame->codec << " independent baseline\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
