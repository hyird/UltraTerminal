#include "features/mcp/mcp.protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <json_codec.h>
#include <model.h>

#include <openssl/evp.h>

#include "ultraterimal/protocol/automation.protocol.h"

namespace ultraterimal::server {
namespace {
constexpr std::string_view kInitializeResultJson =
    R"JSON({"protocolVersion":"2025-03-26","capabilities":{"tools":{}},"serverInfo":{"name":"ultraterimal","version":"1.0.0"},"instructions":"Workflow for operating UltraTerminal remote devices:\n1. Always call devices_list first to discover authorized device_id values, OS platform (Windows or Linux), online status, and capabilities (exec_command, file_transfer, desktop_automation). Only devices inside your API token allowlist and required scopes are accessible.\n2. Prefer exec_command and file_list/file_stat/file_read/file_write for shell commands, diagnostics, logs, and file edits instead of driving GUI clicks.\n3. Use desktop_screenshot and desktop_action only when interacting with a Windows GUI is necessary (requires Windows Agent >= 1.0.65 and desktop_automation=true). First call desktop_screenshot to inspect the current screen and normalized cursor position. Coordinates in desktop_action default to 0..1000 normalized space (x=0..1000, y=0..1000) matching desktop_screenshot; set normalized=false only for raw physical pixels. After critical GUI actions, take another desktop_screenshot to verify the outcome.\n4. For desktop_action key/hotkey, supported key names include: enter, tab, escape, backspace, delete, space, insert, home, end, pageup, pagedown, up, down, left, right, ctrl, alt, shift, win, rctrl, ralt, rshift, rwin, f1..f24, a..z, 0..9, and ASCII punctuation. For arbitrary Unicode or multi-character text entry, use action=\"type_text\" with text.\n5. Confirm target device_id and path carefully before file_write, destructive shell commands, or sensitive desktop actions."})JSON";

constexpr std::string_view kToolsListJson = R"JSON({"tools":[)JSON"
                                            R"JSON({"name":"devices_list","description":"List remote devices accessible to the current scoped API token along with their online status and capabilities.","inputSchema":{"type":"object","properties":{},"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"exec_command","description":"Execute a shell/cmd command synchronously on an online Windows or Linux device and return stdout, stderr, and exit_code.","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"command":{"type":"string"},"cwd":{"type":"string"},"timeout_ms":{"type":"integer","minimum":100,"maximum":120000},"max_output_bytes":{"type":"integer","minimum":256,"maximum":49152}},"required":["device_id","command"],"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"file_list","description":"List files and subdirectories in a directory on an online device (empty path lists Windows drive letters).","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"path":{"type":"string"}},"required":["device_id"],"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"file_stat","description":"Get metadata (exists, directory, size, name) for a path on an online device.","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"path":{"type":"string"}},"required":["device_id","path"],"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"file_read","description":"Read UTF-8/text file content from an online device (capped at 256 KiB), with optional 1-based offset_line and limit_lines.","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"path":{"type":"string"},"offset_line":{"type":"integer","minimum":1},"limit_lines":{"type":"integer","minimum":1}},"required":["device_id","path"],"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"file_write","description":"Write UTF-8/text content (up to 128 KiB) to a file on an online device.","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"path":{"type":"string"},"content":{"type":"string"},"overwrite":{"type":"boolean"}},"required":["device_id","path","content"],"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"desktop_screenshot","description":"Capture a live on-demand PNG screenshot of an online Windows device desktop (supports physical GPUs and Ultraterimal IDD virtual displays).","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"display_index":{"type":"integer","minimum":0,"maximum":63},"max_width":{"type":"integer","minimum":64,"maximum":4096},"max_height":{"type":"integer","minimum":64,"maximum":4096}},"required":["device_id"],"additionalProperties":false}},)JSON"
                                            R"JSON({"name":"desktop_action","description":"Perform mouse or keyboard input on an online Windows device desktop. Coordinates default to normalized 0..1000 space matching desktop_screenshot (or physical pixels if normalized=false).","inputSchema":{"type":"object","properties":{"device_id":{"type":"string"},"action":{"type":"string","enum":["move","click","double_click","mouse_down","mouse_up","drag","scroll","key","hotkey","type_text"]},"display_index":{"type":"integer","minimum":0,"maximum":63},"x":{"type":"integer"},"y":{"type":"integer"},"to_x":{"type":"integer"},"to_y":{"type":"integer"},"normalized":{"type":"boolean"},"button":{"type":"string","enum":["left","middle","right","back","forward"]},"wheel_delta":{"type":"integer"},"wheel_horizontal":{"type":"boolean"},"keys":{"type":"array","items":{"type":"string"},"maxItems":8},"text":{"type":"string"}},"required":["device_id","action"],"additionalProperties":false}})JSON"
                                            R"JSON(]})JSON";

std::string asciiLower(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}
} // namespace

std::string jsonEscape(std::string_view text) {
    return ruvia::to_json(ruvia::string(text));
}

std::string base64Encode(std::string_view binary) {
    if (binary.empty()) {
        return {};
    }
    const std::size_t outLen = ((binary.size() + 2) / 3) * 4;
    std::string out(outLen, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                        reinterpret_cast<const unsigned char*>(binary.data()),
                                        static_cast<int>(binary.size()));
    if (written < 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::expected<McpRequest, McpParseError> parseMcpRequest(std::string_view body) {
    auto parsed = ruvia::json_object::parse(body);
    if (!parsed.has_value()) {
        return std::unexpected(McpParseError{-32700, "null", "Parse error: invalid JSON object"});
    }
    const auto obj = std::move(parsed).take_value();

    McpRequest req;
    const auto idLookup = obj.get<ruvia::json_value>("id");
    if (idLookup.status() == ruvia::unique_lookup_status::repeated) {
        return std::unexpected(McpParseError{-32600, "null", "Duplicate id field"});
    }
    if (idLookup.status() == ruvia::unique_lookup_status::found) {
        if (!idLookup.value() || (!idLookup.value()->is_string() && !idLookup.value()->is_number() && !idLookup.value()->is_null())) {
            return std::unexpected(McpParseError{-32600, "null", "Invalid id field"});
        }
        req.idJson = std::string(idLookup.value()->view());
        req.isNotification = false;
    } else {
        req.isNotification = true;
    }

    const auto replyId = req.idJson.empty() ? std::string("null") : req.idJson;
    const auto versionLookup = obj.get<ruvia::string>("jsonrpc");
    if (versionLookup.status() != ruvia::unique_lookup_status::found || !versionLookup.value() || versionLookup.value()->view() != "2.0") {
        return std::unexpected(McpParseError{-32600, replyId, "Invalid Request: jsonrpc must be 2.0"});
    }

    const auto methodLookup = obj.get<ruvia::string>("method");
    if (methodLookup.status() != ruvia::unique_lookup_status::found || !methodLookup.value() || methodLookup.value()->view().empty()) {
        return std::unexpected(McpParseError{-32600, replyId, "Invalid Request: missing method"});
    }
    req.method = std::string(methodLookup.value()->view());

    const auto paramsLookup = obj.get<ruvia::json_value>("params");
    if (paramsLookup.status() == ruvia::unique_lookup_status::repeated) {
        return std::unexpected(McpParseError{-32602, replyId, "Duplicate params field"});
    }
    if (paramsLookup.status() == ruvia::unique_lookup_status::found) {
        if (!paramsLookup.value() || !paramsLookup.value()->is_object()) {
            return std::unexpected(McpParseError{-32602, replyId, "Invalid params: expected object"});
        }
        req.hasParams = true;
        req.paramsJson = std::string(paramsLookup.value()->view());
    }
    return req;
}

std::string formatJsonRpcResult(std::string_view idJson, std::string_view resultJson) {
    std::string out = "{\"jsonrpc\":\"2.0\",\"id\":";
    out.append(idJson.empty() ? "null" : idJson);
    out.append(",\"result\":");
    out.append(resultJson);
    out.push_back('}');
    return out;
}

std::string formatJsonRpcError(std::string_view idJson, int code, std::string_view message) {
    std::string out = "{\"jsonrpc\":\"2.0\",\"id\":";
    out.append(idJson.empty() ? "null" : idJson);
    out.append(",\"error\":{\"code\":");
    out.append(std::to_string(code));
    out.append(",\"message\":");
    out.append(jsonEscape(message));
    out.append("}}");
    return out;
}

std::string formatMcpToolTextResult(std::string_view text, bool isError) {
    std::string out = "{\"content\":[{\"type\":\"text\",\"text\":";
    out.append(jsonEscape(text));
    out.append("}],\"isError\":");
    out.append(isError ? "true" : "false");
    out.push_back('}');
    return out;
}

std::string formatMcpToolImageAndTextResult(std::string_view base64Png, std::string_view metadataText) {
    std::string out = "{\"content\":[{\"type\":\"image\",\"data\":";
    out.append(jsonEscape(base64Png));
    out.append(",\"mimeType\":\"image/png\"},{\"type\":\"text\",\"text\":");
    out.append(jsonEscape(metadataText));
    out.append("}],\"isError\":false}");
    return out;
}

std::string_view mcpInitializeResultJson() {
    return kInitializeResultJson;
}

std::string_view mcpToolsListJson() {
    return kToolsListJson;
}

std::optional<std::uint32_t> resolveVirtualKeyCode(std::string_view name) {
    const std::string key = asciiLower(name);
    if (key.empty()) {
        return std::nullopt;
    }
    if (key.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(key[0]);
        if (c >= 'a' && c <= 'z') {
            return static_cast<std::uint32_t>(std::toupper(c));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint32_t>(c);
        }
        switch (c) {
        case ';':
            return 0xba;
        case '=':
            return 0xbb;
        case ',':
            return 0xbc;
        case '-':
            return 0xbd;
        case '.':
            return 0xbe;
        case '/':
            return 0xbf;
        case '`':
            return 0xc0;
        case '[':
            return 0xdb;
        case '\\':
            return 0xdc;
        case ']':
            return 0xdd;
        case '\'':
            return 0xde;
        default:
            break;
        }
    }

    if (key.size() >= 2 && key[0] == 'f') {
        unsigned num = 0;
        bool digitsOnly = true;
        for (std::size_t i = 1; i < key.size(); ++i) {
            if (key[i] < '0' || key[i] > '9') {
                digitsOnly = false;
                break;
            }
            num = num * 10 + static_cast<unsigned>(key[i] - '0');
        }
        if (digitsOnly && num >= 1 && num <= 24) {
            return 0x70 + (num - 1);
        }
    }

    struct KeyEntry {
        std::string_view name;
        std::uint32_t vk;
    };
    static constexpr std::array<KeyEntry, 34> kKeys{{
        {"enter", 0x0d},
        {"return", 0x0d},
        {"tab", 0x09},
        {"esc", 0x1b},
        {"escape", 0x1b},
        {"backspace", 0x08},
        {"space", 0x20},
        {"ctrl", 0xa2},
        {"control", 0xa2},
        {"lctrl", 0xa2},
        {"rctrl", 0xa3},
        {"shift", 0xa0},
        {"lshift", 0xa0},
        {"rshift", 0xa1},
        {"alt", 0xa4},
        {"lalt", 0xa4},
        {"ralt", 0xa5},
        {"win", 0x5b},
        {"meta", 0x5b},
        {"super", 0x5b},
        {"lwin", 0x5b},
        {"rwin", 0x5c},
        {"left", 0x25},
        {"up", 0x26},
        {"right", 0x27},
        {"down", 0x28},
        {"home", 0x24},
        {"end", 0x23},
        {"pageup", 0x21},
        {"pagedown", 0x22},
        {"insert", 0x2d},
        {"delete", 0x2e},
        {"del", 0x2e},
        {"capslock", 0x14},
    }};
    for (const auto& entry : kKeys) {
        if (entry.name == key) {
            return entry.vk;
        }
    }
    return std::nullopt;
}

ToolTextSlice sliceTextLines(std::string_view text, std::uint64_t offsetLine, std::uint64_t limitLines, bool alreadyTruncated) {
    ToolTextSlice result;
    result.startLine = offsetLine ? offsetLine : 1;
    if (text.empty()) {
        result.truncated = alreadyTruncated;
        return result;
    }

    std::uint64_t lineNo = 0;
    std::size_t pos = 0;
    bool slicedTruncated = false;
    while (pos < text.size()) {
        ++lineNo;
        const std::size_t nl = text.find('\n', pos);
        const std::size_t lineEnd = nl == std::string_view::npos ? text.size() : nl + 1;
        const std::string_view line = text.substr(pos, lineEnd - pos);

        const bool afterStart = lineNo >= result.startLine;
        const bool withinLimit = limitLines == 0 || (lineNo - result.startLine) < limitLines;
        if (afterStart && withinLimit) {
            result.content.append(line);
            result.endLine = lineNo;
        } else if (afterStart && !withinLimit) {
            slicedTruncated = true;
        }
        pos = lineEnd;
    }

    result.totalLines = lineNo;
    result.truncated = alreadyTruncated || slicedTruncated;
    return result;
}
} // namespace ultraterimal::server
