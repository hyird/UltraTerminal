#include <iostream>
#include <json_codec.h>
#include <stdexcept>
#include <string>

#include "features/mcp/mcp.protocol.h"
#include "modules/tokens/token.service.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    using namespace ultraterimal::server;
    try {
        const auto initReq = parseMcpRequest(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
        require(initReq.has_value() && !initReq->isNotification && initReq->idJson == "1" && initReq->method == "initialize",
                "Valid MCP initialize failed to parse");

        const auto notifReq = parseMcpRequest(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        require(notifReq.has_value() && notifReq->isNotification && notifReq->method == "notifications/initialized",
                "Valid MCP notification failed to parse");

        require(!parseMcpRequest("not json"), "Invalid JSON accepted");
        require(!parseMcpRequest(R"({"jsonrpc":"1.0","id":1,"method":"ping"})"), "Wrong JSON-RPC version accepted");
        require(!parseMcpRequest(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":123})"), "Non-object params accepted");

        require(base64Encode("Man") == "TWFu", "Base64 encode mismatch");
        require(base64Encode("hi") == "aGk=", "Base64 padding mismatch");
        require(jsonEscape("a\"b\nc") == "\"a\\\"b\\nc\"", "JSON string escape mismatch");

        const auto vkEnter = resolveVirtualKeyCode("Enter");
        const auto vkCtrl = resolveVirtualKeyCode("ctrl");
        const auto vkF12 = resolveVirtualKeyCode("F12");
        const auto vkA = resolveVirtualKeyCode("a");
        require(vkEnter && *vkEnter == 0x0d, "Enter key code mismatch");
        require(vkCtrl && *vkCtrl == 0xa2, "Ctrl key code mismatch");
        require(vkF12 && *vkF12 == 0x7b, "F12 key code mismatch");
        require(vkA && *vkA == 'A', "Letter key code mismatch");
        require(!resolveVirtualKeyCode("unknown_key_xyz"), "Unknown key accepted");

        const auto slice = sliceTextLines("line1\nline2\nline3\nline4\n", 2, 2, false);
        require(slice.totalLines == 4 && slice.startLine == 2 && slice.endLine == 3 && slice.truncated && slice.content == "line2\nline3\n",
                "Text line slice mismatch");

        const auto initJson = mcpInitializeResultJson();
        auto initParsed = ruvia::json_object::parse(initJson);
        require(initParsed.has_value(), "MCP initialize JSON is invalid");
        const auto initObj = std::move(initParsed).take_value();
        const auto protoVer = initObj.get<ruvia::string>("protocolVersion");
        const auto instr = initObj.get<ruvia::string>("instructions");
        require(protoVer.status() == ruvia::unique_lookup_status::found && protoVer.value() && protoVer.value()->view() == "2025-03-26",
                "MCP initialize protocolVersion mismatch");
        require(instr.status() == ruvia::unique_lookup_status::found && instr.value() &&
                    instr.value()->view().find("devices_list") != std::string_view::npos &&
                    instr.value()->view().find("exec_command") != std::string_view::npos &&
                    instr.value()->view().find("desktop_screenshot") != std::string_view::npos &&
                    instr.value()->view().find("0..1000") != std::string_view::npos,
                "MCP initialize instructions missing required guidance");

        const auto tools = mcpToolsListJson();
        require(tools.find("\"exec_command\"") != std::string_view::npos &&
                    tools.find("\"desktop_screenshot\"") != std::string_view::npos &&
                    tools.find("\"desktop_action\"") != std::string_view::npos,
                "Expected MCP tools missing");

        require(validApiTokenFormat("utm_" + std::string(64, 'a')), "Valid API token format rejected");
        require(!validApiTokenFormat("bad_" + std::string(64, 'a')), "Wrong token prefix accepted");
        require(!validApiTokenFormat("utm_short"), "Short API token accepted");
        require(validTokenScope("desktop:input") && !validTokenScope("admin:all"), "Token scope allowlist failed");

        std::cout << "MCP JSON-RPC, encoding, key resolution and token format checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
