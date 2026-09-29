#include <array>
#include <iostream>
#include <stdexcept>

#include "modules/identity/security.protocol.h"

using namespace ultraterimal::server;
int main() {
    try {
        const std::string secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
        const std::array<std::pair<std::int64_t, std::string_view>, 6> vectors{{{59, "287082"}, {1111111109, "081804"}, {1111111111, "050471"}, {1234567890, "005924"}, {2000000000, "279037"}, {20000000000, "353130"}}};
        for (const auto& [time, expected] : vectors) {
            if (totpCode(secret, time / 30) != expected || verifyTotp(secret, expected, time, -1) != time / 30 || verifyTotp(secret, expected, time, time / 30)) {
                throw std::runtime_error("RFC 6238 or replay check failed");
            }
        }
        if (verifyTotp(secret, "12345", 59, -1) || verifyTotp(secret, "abcdef", 59, -1) || verifyTotp(secret, "287082", 180, -1)) {
            throw std::runtime_error("Invalid TOTP accepted");
        }
        const auto generated = newTotpSecret();
        if (generated.size() != 32 || !verifyTotp(generated, totpCode(generated, 100), 3000, -1)) {
            throw std::runtime_error("Generated TOTP invalid");
        }
        const std::string key(64, 'a');
        const auto ciphertext = sealSecret(key, "totp:alice", secret);
        if (openSecret(key, "totp:alice", ciphertext) != secret || ciphertext == sealSecret(key, "totp:alice", secret)) {
            throw std::runtime_error("Secret roundtrip or nonce failed");
        }
        const auto rejects = [&](std::string_view owner, std::string_view encrypted) {
            try {
                (void)openSecret(key, owner, encrypted);
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        auto tampered = ciphertext;
        tampered.back() = tampered.back() == 'a' ? 'b' : 'a';
        if (!rejects("totp:bob", ciphertext) || !rejects("totp:alice", tampered) || !rejects("totp:alice", "v1:aa")) {
            throw std::runtime_error("Secret authentication failed");
        }
        std::cout << "TOTP RFC vectors, replay limits, secret encryption and tamper checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
