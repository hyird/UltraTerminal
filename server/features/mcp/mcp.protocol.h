#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ultraterimal::server {
struct McpRequest {
    bool isNotification = false;
    std::string idJson;
    std::string method;
    bool hasParams = false;
    std::string paramsJson;
};

struct McpParseError {
    int code = -32600;
    std::string idJson = "null";
    std::string message;
};

struct ToolTextSlice {
    std::string content;
    std::uint64_t totalLines = 0;
    std::uint64_t startLine = 1;
    std::uint64_t endLine = 0;
    bool truncated = false;
};

std::expected<McpRequest, McpParseError> parseMcpRequest(std::string_view body);
std::string jsonEscape(std::string_view text);
std::string base64Encode(std::string_view binary);
std::string formatJsonRpcResult(std::string_view idJson, std::string_view resultJson);
std::string formatJsonRpcError(std::string_view idJson, int code, std::string_view message);
std::string formatMcpToolTextResult(std::string_view text, bool isError = false);
std::string formatMcpToolImageAndTextResult(std::string_view base64Png, std::string_view metadataText);
std::string_view mcpInitializeResultJson();
std::string_view mcpToolsListJson();
std::optional<std::uint32_t> resolveVirtualKeyCode(std::string_view name);
ToolTextSlice sliceTextLines(std::string_view text, std::uint64_t offsetLine, std::uint64_t limitLines, bool alreadyTruncated);
} // namespace ultraterimal::server
