#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ultraterimal::server {
// OpenSSL-backed primitives. Ciphertext is bound to its owning record via AAD.
std::string sealSecret(std::string_view key, std::string_view owner, std::string_view plaintext);
std::string openSecret(std::string_view key, std::string_view owner, std::string_view ciphertext);
std::string newTotpSecret();
std::string totpCode(std::string_view secret, std::int64_t step);
std::optional<std::int64_t> verifyTotp(std::string_view secret, std::string_view code, std::int64_t unixSeconds, std::int64_t lastStep);
bool constantEqual(std::string_view left, std::string_view right);
} // namespace ultraterimal::server
