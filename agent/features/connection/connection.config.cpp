#include "connection.config.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace ultraterimal::agent {
namespace {
void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        output.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}
std::uint16_t parseHex4(std::string_view bytes, std::size_t& position) {
    if (position + 4 > bytes.size()) {
        throw std::runtime_error("Truncated JSON unicode escape");
    }
    std::uint16_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const char ch = bytes[position++];
        std::uint16_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<std::uint16_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<std::uint16_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<std::uint16_t>(ch - 'A' + 10);
        } else {
            throw std::runtime_error("Invalid JSON unicode escape");
        }
        value = static_cast<std::uint16_t>((value << 4) | digit);
    }
    return value;
}
void skipWhitespace(std::string_view bytes, std::size_t& position) {
    while (position < bytes.size() && (bytes[position] == ' ' || bytes[position] == '\t' || bytes[position] == '\r' || bytes[position] == '\n')) {
        ++position;
    }
}
std::string parseJsonString(std::string_view bytes, std::size_t& position) {
    if (position >= bytes.size() || bytes[position] != '"') {
        throw std::runtime_error("Expected JSON string");
    }
    ++position;
    std::string output;
    while (position < bytes.size()) {
        const unsigned char ch = static_cast<unsigned char>(bytes[position++]);
        if (ch == '"') {
            return output;
        }
        if (ch < 0x20u) {
            throw std::runtime_error("Unescaped control character in JSON string");
        }
        if (ch != '\\') {
            output.push_back(static_cast<char>(ch));
            continue;
        }
        if (position >= bytes.size()) {
            throw std::runtime_error("Truncated JSON escape");
        }
        const char escaped = bytes[position++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
            break;
        case 'b':
            output.push_back('\b');
            break;
        case 'f':
            output.push_back('\f');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        case 'u': {
            const std::uint16_t high = parseHex4(bytes, position);
            if (high >= 0xD800u && high <= 0xDBFFu) {
                if (position + 2 > bytes.size() || bytes[position] != '\\' || bytes[position + 1] != 'u') {
                    throw std::runtime_error("Missing JSON low surrogate");
                }
                position += 2;
                const std::uint16_t low = parseHex4(bytes, position);
                if (low < 0xDC00u || low > 0xDFFFu) {
                    throw std::runtime_error("Invalid JSON low surrogate");
                }
                appendUtf8(output, 0x10000u + ((static_cast<std::uint32_t>(high - 0xD800u) << 10) | static_cast<std::uint32_t>(low - 0xDC00u)));
            } else if (high >= 0xDC00u && high <= 0xDFFFu) {
                throw std::runtime_error("Unexpected JSON low surrogate");
            } else {
                appendUtf8(output, high);
            }
            break;
        }
        default:
            throw std::runtime_error("Invalid JSON escape");
        }
    }
    throw std::runtime_error("Unterminated JSON string");
}
void appendJsonString(std::string& output, std::string_view text) {
    output.push_back('"');
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20u) {
                constexpr char hex[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hex[ch >> 4]);
                output.push_back(hex[ch & 0x0Fu]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    output.push_back('"');
}
ConnectionConfig loadConfig(const std::filesystem::path& path, bool enrollment) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open connection configuration");
    }
    std::string bytes(65537, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > 65536) {
        throw std::runtime_error("Connection configuration exceeds 64 KiB");
    }
    const auto object = parseStringJsonObject(bytes);
    for (const auto& [key, _] : object) {
        if (key != "host" && key != "port" && key != "ca_file" && key != "credential_file" && key != "device_id" && key != "address") {
            throw std::runtime_error("Unknown connection configuration field");
        }
    }
    const auto string = [&](const char* key) {
        const auto found = object.find(key);
        if (found == object.end() || found->second.empty()) {
            throw std::runtime_error("Missing or invalid connection configuration field");
        }
        return found->second;
    };
    ConnectionConfig config;
    config.host = string("host");
    if (object.contains("address")) {
        config.address = string("address");
        if (config.address.size() > 253 || config.address.find_first_of("/\\ \t\r\n") != std::string::npos || config.address.find('\0') != std::string::npos) {
            throw std::runtime_error("Invalid native endpoint address");
        }
    }
    if (object.contains("port")) {
        config.port = string("port");
    }
    if (enrollment) {
        if (object.contains("device_id")) {
            throw std::runtime_error("Enrollment configuration must not contain an existing device identity");
        }
    } else {
        config.deviceId = string("device_id");
    }
    const auto utf8Path = [&](const char* key) {
        const auto text = string(key);
        if (text.find('\0') != std::string::npos) {
            throw std::runtime_error("Invalid configuration path");
        }
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    };
    config.caFile = utf8Path("ca_file");
    config.credentialFile = utf8Path("credential_file");
    if (config.host.size() > 253 || config.host.find_first_of("/\\ \t\r\n") != std::string::npos || config.host.find('\0') != std::string::npos || config.host.empty()) {
        throw std::runtime_error("Invalid gateway host");
    }
    if (config.port.empty() || config.port.size() > 5 || config.port.find_first_not_of("0123456789") != std::string::npos || std::stoul(config.port) == 0 || std::stoul(config.port) > 65535) {
        throw std::runtime_error("Invalid gateway port");
    }
    if (config.deviceId.size() > 128 || config.deviceId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
        throw std::runtime_error("Invalid device identifier");
    }
    const auto base = std::filesystem::absolute(path).parent_path();
    if (config.caFile.is_relative()) {
        config.caFile = base / config.caFile;
    }
    if (config.credentialFile.is_relative()) {
        config.credentialFile = base / config.credentialFile;
    }
    return config;
}
} // namespace
std::map<std::string, std::string> parseStringJsonObject(std::string_view bytes) {
    std::size_t position = 0;
    skipWhitespace(bytes, position);
    if (position >= bytes.size() || bytes[position] != '{') {
        throw std::runtime_error("Expected JSON object");
    }
    ++position;
    skipWhitespace(bytes, position);
    std::map<std::string, std::string> result;
    if (position < bytes.size() && bytes[position] == '}') {
        ++position;
        skipWhitespace(bytes, position);
        if (position != bytes.size()) {
            throw std::runtime_error("Unexpected trailing JSON content");
        }
        return result;
    }
    while (true) {
        skipWhitespace(bytes, position);
        auto key = parseJsonString(bytes, position);
        skipWhitespace(bytes, position);
        if (position >= bytes.size() || bytes[position] != ':') {
            throw std::runtime_error("Expected ':' in JSON object");
        }
        ++position;
        skipWhitespace(bytes, position);
        auto value = parseJsonString(bytes, position);
        if (!result.emplace(std::move(key), std::move(value)).second) {
            throw std::runtime_error("Duplicate JSON field");
        }
        skipWhitespace(bytes, position);
        if (position >= bytes.size()) {
            throw std::runtime_error("Unterminated JSON object");
        }
        if (bytes[position] == '}') {
            ++position;
            break;
        }
        if (bytes[position] != ',') {
            throw std::runtime_error("Expected ',' or '}' in JSON object");
        }
        ++position;
    }
    skipWhitespace(bytes, position);
    if (position != bytes.size()) {
        throw std::runtime_error("Unexpected trailing JSON content");
    }
    return result;
}
ConnectionConfig loadConnectionConfig(const std::filesystem::path& path) { return loadConfig(path, false); }
ConnectionConfig loadEnrollmentConfig(const std::filesystem::path& path) { return loadConfig(path, true); }
std::string connectionConfigJson(const ConnectionConfig& config) {
    const auto utf8 = [](const std::filesystem::path& path) {
        const auto bytes = path.u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    std::string output = "{";
    bool first = true;
    const auto field = [&](std::string_view key, std::string_view value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        appendJsonString(output, key);
        output.push_back(':');
        appendJsonString(output, value);
    };
    field("host", config.host);
    field("port", config.port);
    field("device_id", config.deviceId);
    field("ca_file", utf8(config.caFile));
    field("credential_file", utf8(config.credentialFile));
    if (!config.address.empty()) {
        field("address", config.address);
    }
    output.push_back('}');
    return output;
}
} // namespace ultraterimal::agent
