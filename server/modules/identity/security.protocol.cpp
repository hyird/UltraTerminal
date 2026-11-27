#include "security.protocol.h"

#include <array>
#include <binary_text_encoding.h>
#include <crypto.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace ultraterimal::server {
namespace {
constexpr std::string_view kBase32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
std::string toHex(std::span<const unsigned char> bytes) {
    return ruvia::encode_lower_hex(std::as_bytes(bytes));
}
std::vector<unsigned char> fromHex(std::string_view text) {
    if (text.size() % 2 != 0 || text.size() > 8192 || !ruvia::is_lower_hex(text, text.size())) {
        throw std::invalid_argument("Invalid encrypted secret");
    }
    std::vector<unsigned char> result(text.size() / 2);
    if (!ruvia::decode_hex(text, std::as_writable_bytes(std::span(result)))) {
        throw std::invalid_argument("Invalid encrypted secret");
    }
    return result;
}
void require(bool ok) {
    if (!ok) {
        throw std::runtime_error("Secret operation failed");
    }
}
} // namespace
bool constantEqual(std::string_view left, std::string_view right) {
    return ruvia::constant_time_bytes_equal(left, right);
}
std::string sealSecret(std::string_view key, std::string_view owner, std::string_view plaintext) {
    const auto bytes = fromHex(key);
    require(bytes.size() == 32 && plaintext.size() <= 2048 && owner.size() <= 256);
    std::array<unsigned char, 12> nonce{};
    std::array<unsigned char, 16> tag{};
    require(ruvia::fill_random_bytes(std::as_writable_bytes(std::span(nonce))));
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    require(ctx && EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, bytes.data(), nonce.data()) == 1);
    int length = 0;
    require(EVP_EncryptUpdate(ctx.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(owner.data()), static_cast<int>(owner.size())) == 1);
    std::vector<unsigned char> encrypted(plaintext.size() + 16);
    require(EVP_EncryptUpdate(ctx.get(), encrypted.data(), &length, reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) == 1);
    const auto count = length;
    require(EVP_EncryptFinal_ex(ctx.get(), encrypted.data() + count, &length) == 1);
    encrypted.resize(static_cast<std::size_t>(count + length));
    require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1);
    return "v1:" + toHex(nonce) + toHex(tag) + toHex(encrypted);
}
std::string openSecret(std::string_view key, std::string_view owner, std::string_view ciphertext) {
    require(ciphertext.starts_with("v1:") && owner.size() <= 256);
    const auto bytes = fromHex(key);
    auto encrypted = fromHex(ciphertext.substr(3));
    require(bytes.size() == 32 && encrypted.size() >= 28);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    require(ctx && EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, bytes.data(), encrypted.data()) == 1);
    int length = 0;
    require(EVP_DecryptUpdate(ctx.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(owner.data()), static_cast<int>(owner.size())) == 1);
    std::string result(encrypted.size(), '\0');
    require(EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(result.data()), &length, encrypted.data() + 28, static_cast<int>(encrypted.size() - 28)) == 1);
    const auto count = length;
    require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, encrypted.data() + 12) == 1);
    require(EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(result.data()) + count, &length) == 1);
    result.resize(static_cast<std::size_t>(count + length));
    return result;
}
std::string newTotpSecret() {
    std::array<unsigned char, 20> bytes{};
    require(ruvia::fill_random_bytes(std::as_writable_bytes(std::span(bytes))));
    std::string result;
    unsigned accumulator = 0, bits = 0;
    for (const auto byte : bytes) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            result += kBase32[(accumulator >> bits) & 31];
        }
    }
    return result;
}
std::string totpCode(std::string_view secret, std::int64_t step) {
    if (secret.size() != 32 || step < 0) {
        throw std::invalid_argument("Invalid TOTP parameters");
    }
    std::vector<unsigned char> key;
    unsigned accumulator = 0, bits = 0;
    for (const char c : secret) {
        const auto value = kBase32.find(c);
        if (value == std::string_view::npos) {
            throw std::invalid_argument("Invalid TOTP secret");
        }
        accumulator = (accumulator << 5) | static_cast<unsigned>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            key.push_back(static_cast<unsigned char>(accumulator >> bits));
        }
    }
    std::array<unsigned char, 8> counter{};
    auto remaining = static_cast<std::uint64_t>(step);
    for (int i = 7; i >= 0; --i) {
        counter[static_cast<std::size_t>(i)] = static_cast<unsigned char>(remaining);
        remaining >>= 8;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    require(HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()), counter.data(), counter.size(), digest.data(), &length) != nullptr && length == 20);
    const auto offset = digest[19] & 15;
    const auto value = ((static_cast<std::uint32_t>(digest[offset]) & 127) << 24) | (static_cast<std::uint32_t>(digest[offset + 1]) << 16) | (static_cast<std::uint32_t>(digest[offset + 2]) << 8) | digest[offset + 3];
    const auto code = std::to_string(value % 1000000);
    return std::string(6 - code.size(), '0') + code;
}
std::optional<std::int64_t> verifyTotp(std::string_view secret, std::string_view code, std::int64_t unixSeconds, std::int64_t lastStep) {
    if (code.size() != 6 || code.find_first_not_of("0123456789") != std::string_view::npos || unixSeconds < 0) {
        return std::nullopt;
    }
    for (const auto delta : {0, -1, 1}) {
        const auto step = unixSeconds / 30 + delta;
        if (step >= 0 && step > lastStep && constantEqual(totpCode(secret, step), code)) {
            return step;
        }
    }
    return std::nullopt;
}
} // namespace ultraterimal::server
