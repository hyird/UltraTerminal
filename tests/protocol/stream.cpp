#include <iostream>
#include <limits>
#include <stdexcept>

#include "ultraterimal/protocol/stream.protocol.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    using namespace ultraterimal::protocol;
    try {
        for (const std::size_t size : {std::size_t{1}, std::size_t{255}, std::size_t{256}, kMaximumControlBytes, kMaximumMessageBytes}) {
            const auto header = encodeStreamHeader(size);
            require(header.has_value(), "Valid size rejected");
            require(decodeStreamHeader(*header) == size, "Stream size round trip failed");
        }
        const StreamHeader networkOrder{std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}};
        require(decodeStreamHeader(networkOrder) == 0x010203, "Not network byte order");
        require(encodeStreamHeader(0x010203) == networkOrder, "Wrong header bytes");
        require(!encodeStreamHeader(0), "Empty record accepted");
        require(!encodeStreamHeader(std::numeric_limits<std::size_t>::max()), "Truncated oversized length");
        require(!decodeStreamHeader(StreamHeader{}), "Empty inbound record accepted");
        require(!decodeStreamHeader(StreamHeader{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}}), "Hostile length accepted");
        require(!decodeStreamHeader(*encodeStreamHeader(kMaximumControlBytes + 1), kMaximumControlBytes), "Control quota bypassed");
        require(!encodeStreamHeader(1, 0), "Invalid zero quota accepted");
        require(!decodeStreamHeader(networkOrder, kMaximumMessageBytes + 1), "Invalid oversized quota accepted");
        std::cout << "PASS: native stream framing, byte order and allocation bounds\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
