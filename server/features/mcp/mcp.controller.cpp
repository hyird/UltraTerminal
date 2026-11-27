#include "features/mcp/mcp.controller.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <json_codec.h>
#include <memory>
#include <model.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/http.h"
#include "config/server.runtime.h"
#include "features/mcp/mcp.protocol.h"
#include "modules/tokens/token.service.h"
#include "ultraterimal/protocol/automation.protocol.h"
#include "ultraterimal/protocol/file.protocol.h"

namespace ultraterimal::server {
namespace {
constexpr std::size_t kMaximumMcpFileReadBytes = 256 * 1024;
constexpr std::size_t kMaximumMcpFileWriteBytes = 128 * 1024;

RUVIA_MODEL(ToolCallParams,
            RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_NOT_EMPTY("tool name required"), RUVIA_MAX_BYTES(128, "tool name too long")),
            RUVIA_OPTIONAL_FIELD(arguments, ruvia::json_object));

RUVIA_MODEL(ExecCommandArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_REQUIRED_FIELD(command, ruvia::string, RUVIA_NOT_EMPTY("command required"), RUVIA_MAX_BYTES(16384, "command too long")),
            RUVIA_OPTIONAL_FIELD(cwd, ruvia::string, RUVIA_MAX_BYTES(4096, "cwd too long")),
            RUVIA_OPTIONAL_FIELD(timeout_ms, std::int64_t),
            RUVIA_OPTIONAL_FIELD(max_output_bytes, std::int64_t));

RUVIA_MODEL(FileListArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_OPTIONAL_FIELD(path, ruvia::string, RUVIA_MAX_BYTES(4096, "path too long")));

RUVIA_MODEL(FileStatArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_REQUIRED_FIELD(path, ruvia::string, RUVIA_NOT_EMPTY("path required"), RUVIA_MAX_BYTES(4096, "path too long")));

RUVIA_MODEL(FileReadArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_REQUIRED_FIELD(path, ruvia::string, RUVIA_NOT_EMPTY("path required"), RUVIA_MAX_BYTES(4096, "path too long")),
            RUVIA_OPTIONAL_FIELD(offset_line, std::int64_t),
            RUVIA_OPTIONAL_FIELD(limit_lines, std::int64_t));

RUVIA_MODEL(FileWriteArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_REQUIRED_FIELD(path, ruvia::string, RUVIA_NOT_EMPTY("path required"), RUVIA_MAX_BYTES(4096, "path too long")),
            RUVIA_REQUIRED_FIELD(content, ruvia::string, RUVIA_MAX_BYTES(131072, "content too large")),
            RUVIA_OPTIONAL_FIELD(overwrite, bool));

RUVIA_MODEL(DesktopScreenshotArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_OPTIONAL_FIELD(display_index, std::int64_t),
            RUVIA_OPTIONAL_FIELD(max_width, std::int64_t),
            RUVIA_OPTIONAL_FIELD(max_height, std::int64_t));

RUVIA_MODEL(DesktopActionArgs,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long")),
            RUVIA_REQUIRED_FIELD(action, ruvia::string, RUVIA_NOT_EMPTY("action required"), RUVIA_MAX_BYTES(32, "action too long")),
            RUVIA_OPTIONAL_FIELD(display_index, std::int64_t),
            RUVIA_OPTIONAL_FIELD(x, std::int64_t),
            RUVIA_OPTIONAL_FIELD(y, std::int64_t),
            RUVIA_OPTIONAL_FIELD(to_x, std::int64_t),
            RUVIA_OPTIONAL_FIELD(to_y, std::int64_t),
            RUVIA_OPTIONAL_FIELD(normalized, bool),
            RUVIA_OPTIONAL_FIELD(button, ruvia::string, RUVIA_MAX_BYTES(16, "button too long")),
            RUVIA_OPTIONAL_FIELD(wheel_delta, std::int64_t),
            RUVIA_OPTIONAL_FIELD(wheel_horizontal, bool),
            RUVIA_OPTIONAL_FIELD(keys, ruvia::array<ruvia::string>),
            RUVIA_OPTIONAL_FIELD(text, ruvia::string, RUVIA_MAX_BYTES(4096, "text too long")));

ruvia::http_response mcpJsonResponse(ruvia::context& context, std::string payload) {
    context.policy().header("Cache-Control", "no-store");
    return context.json_raw(std::move(payload));
}

std::optional<wire::DesktopActionRequest::Action> parseDesktopActionKind(std::string_view action) {
    if (action == "move") {
        return wire::DesktopActionRequest::ACTION_MOVE;
    }
    if (action == "click") {
        return wire::DesktopActionRequest::ACTION_CLICK;
    }
    if (action == "double_click") {
        return wire::DesktopActionRequest::ACTION_DOUBLE_CLICK;
    }
    if (action == "mouse_down") {
        return wire::DesktopActionRequest::ACTION_MOUSE_DOWN;
    }
    if (action == "mouse_up") {
        return wire::DesktopActionRequest::ACTION_MOUSE_UP;
    }
    if (action == "drag") {
        return wire::DesktopActionRequest::ACTION_DRAG;
    }
    if (action == "scroll") {
        return wire::DesktopActionRequest::ACTION_SCROLL;
    }
    if (action == "key") {
        return wire::DesktopActionRequest::ACTION_KEY;
    }
    if (action == "hotkey") {
        return wire::DesktopActionRequest::ACTION_HOTKEY;
    }
    if (action == "type_text") {
        return wire::DesktopActionRequest::ACTION_TYPE_TEXT;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> parseMouseButton(std::string_view button) {
    if (button.empty() || button == "left") {
        return 0;
    }
    if (button == "middle") {
        return 1;
    }
    if (button == "right") {
        return 2;
    }
    if (button == "back") {
        return 3;
    }
    if (button == "forward") {
        return 4;
    }
    return std::nullopt;
}

ruvia::task<std::string> runToolDevicesList(ruvia::context& context, const AuthenticatedToken& token) {
    if (!token.hasScope(kTokenScopeDevicesRead)) {
        co_await context.worker_state<ServerRuntime>().tokens.auditToolCall(context.db(), token, {}, "devices_list", false, "scope_denied");
        co_return formatMcpToolTextResult("Missing scope devices:read", true);
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto devices = co_await runtime.devices.list(context.db(), token.user.id);
    std::vector<std::string> allowedIds;
    std::vector<DeviceInfo> visibleDevices;
    for (const auto& d : devices) {
        if (token.allowsDevice(d.id)) {
            allowedIds.push_back(d.id);
            visibleDevices.push_back(d);
        }
    }
    const auto onlineStates = co_await queryDeviceOnlineStates(context.worker(), runtime, allowedIds);

    std::string out = "{\"devices\":[";
    for (std::size_t i = 0; i < visibleDevices.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const auto& dev = visibleDevices[i];
        const auto it = onlineStates.find(dev.id);
        const auto st = it != onlineStates.end() ? it->second : DeviceOnlineState{};
        const std::string& platform = st.online && !st.platform.empty() ? st.platform : dev.platform;
        out.append("{\"id\":");
        out.append(jsonEscape(dev.id));
        out.append(",\"name\":");
        out.append(jsonEscape(dev.name));
        out.append(",\"platform\":");
        out.append(jsonEscape(platform));
        out.append(",\"online\":");
        out.append(st.online ? "true" : "false");
        out.append(",\"write_control\":");
        out.append(dev.writeControl ? "true" : "false");
        out.append(",\"capabilities\":{\"terminal\":");
        out.append(st.online && st.terminalAvailable ? "true" : "false");
        out.append(",\"exec_command\":");
        out.append(st.online && st.execCommandAvailable ? "true" : "false");
        out.append(",\"file_transfer\":");
        out.append(st.online ? "true" : "false");
        out.append(",\"desktop_automation\":");
        out.append(st.online && st.desktopAutomationAvailable ? "true" : "false");
        out.append("}}");
    }
    out.append("]}");
    co_await runtime.tokens.auditToolCall(context.db(), token, {}, "devices_list", true);
    co_return formatMcpToolTextResult(out, false);
}

ruvia::task<std::string> runToolExecCommand(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<ExecCommandArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for exec_command", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());
    auto& runtime = context.worker_state<ServerRuntime>();

    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeTerminalExec, true))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "exec_command", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing terminal:exec scope", true);
    }

    wire::ExecCommandRequest req;
    req.set_command(textValue(args.get<"command">()));
    if (const auto& cwd = args.get<"cwd">()) {
        req.set_cwd(textValue(*cwd));
    }
    if (const auto& timeout = args.get<"timeout_ms">()) {
        if (*timeout < protocol::kMinimumExecTimeoutMs || *timeout > protocol::kMaximumExecTimeoutMs) {
            co_return formatMcpToolTextResult("timeout_ms must be between 100 and 120000", true);
        }
        req.set_timeout_ms(static_cast<std::uint32_t>(*timeout));
    }
    if (const auto& maxOut = args.get<"max_output_bytes">()) {
        if (*maxOut < 256 || *maxOut > static_cast<std::int64_t>(protocol::kMaximumExecOutputBytes)) {
            co_return formatMcpToolTextResult("max_output_bytes must be between 256 and 49152", true);
        }
        req.set_max_output_bytes(static_cast<std::uint32_t>(*maxOut));
    }
    req.set_request_id(1);
    if (!protocol::validExecCommandRequest(req)) {
        co_return formatMcpToolTextResult("Invalid command or cwd", true);
    }

    const auto result = co_await requestDeviceExecCommand(context.worker(), runtime, deviceId, std::move(req));
    if (!result) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "exec_command", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or Agent version does not support exec_command", true);
    }

    const bool ok = result->success() && result->error() == wire::EXEC_ERROR_NONE;
    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "exec_command", ok, std::string(protocol::execErrorCodeName(result->error())));

    std::string out = "{";
    out.append("\"exit_code\":").append(std::to_string(result->exit_code()));
    out.append(",\"timed_out\":").append(result->timed_out() ? "true" : "false");
    out.append(",\"truncated\":").append(result->truncated() ? "true" : "false");
    out.append(",\"error\":").append(jsonEscape(protocol::execErrorCodeName(result->error())));
    out.append(",\"stdout\":").append(jsonEscape(result->stdout_data()));
    out.append(",\"stderr\":").append(jsonEscape(result->stderr_data()));
    out.push_back('}');
    co_return formatMcpToolTextResult(out, !ok && !result->timed_out());
}

ruvia::task<std::string> runToolFileList(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<FileListArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for file_list", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());
    const auto path = optionalTextValue(args.get<"path">());
    if (!protocol::validFilePathText(path, true)) {
        co_return formatMcpToolTextResult("Invalid path", true);
    }

    auto& runtime = context.worker_state<ServerRuntime>();
    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeFilesRead, false))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_list", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing files:read scope", true);
    }

    wire::FileListRequest req;
    req.set_path(path);
    const auto result = co_await requestDeviceFileList(context.worker(), runtime, deviceId, std::move(req));
    if (!result) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_list", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or does not support file listing", true);
    }
    if (!result->success()) {
        const auto err = std::string(protocol::fileErrorCodeName(result->error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_list", false, err);
        co_return formatMcpToolTextResult("File list failed: " + err, true);
    }

    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_list", true);
    std::string out = "{\"normalized_path\":";
    out.append(jsonEscape(result->normalized_path()));
    out.append(",\"parent_path\":");
    out.append(jsonEscape(result->parent_path()));
    out.append(",\"truncated\":");
    out.append(result->truncated() ? "true" : "false");
    out.append(",\"entries\":[");
    for (int i = 0; i < result->entries_size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const auto& e = result->entries(i);
        out.append("{\"name\":").append(jsonEscape(e.name()));
        out.append(",\"directory\":").append(e.directory() ? "true" : "false");
        out.append(",\"size\":").append(std::to_string(e.size()));
        out.append(",\"modified_unix_seconds\":").append(std::to_string(e.modified_unix_seconds())).push_back('}');
    }
    out.append("]}");
    co_return formatMcpToolTextResult(out, false);
}

ruvia::task<std::string> runToolFileStat(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<FileStatArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for file_stat", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());
    const auto path = textValue(args.get<"path">());
    if (!protocol::validFilePathText(path, false)) {
        co_return formatMcpToolTextResult("Invalid path", true);
    }

    auto& runtime = context.worker_state<ServerRuntime>();
    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeFilesRead, false))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_stat", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing files:read scope", true);
    }

    wire::FileStatRequest req;
    req.set_path(path);
    const auto result = co_await requestDeviceFileStat(context.worker(), runtime, deviceId, std::move(req));
    if (!result) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_stat", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or does not support file stat", true);
    }
    if (!result->success()) {
        const auto err = std::string(protocol::fileErrorCodeName(result->error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_stat", false, err);
        co_return formatMcpToolTextResult("File stat failed: " + err, true);
    }

    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_stat", true);
    std::string out = "{\"exists\":";
    out.append(result->exists() ? "true" : "false");
    out.append(",\"directory\":").append(result->directory() ? "true" : "false");
    out.append(",\"size\":").append(std::to_string(result->size()));
    out.append(",\"name\":").append(jsonEscape(result->name())).push_back('}');
    co_return formatMcpToolTextResult(out, false);
}

ruvia::task<std::string> runToolFileRead(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<FileReadArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for file_read", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());
    const auto path = textValue(args.get<"path">());
    if (!protocol::validFilePathText(path, false)) {
        co_return formatMcpToolTextResult("Invalid path", true);
    }
    const std::uint64_t offsetLine = args.get<"offset_line">() && *args.get<"offset_line">() > 0 ? static_cast<std::uint64_t>(*args.get<"offset_line">()) : 1;
    const std::uint64_t limitLines = args.get<"limit_lines">() && *args.get<"limit_lines">() > 0 ? static_cast<std::uint64_t>(*args.get<"limit_lines">()) : 0;

    auto& runtime = context.worker_state<ServerRuntime>();
    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeFilesRead, false))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing files:read scope", true);
    }

    auto bridge = runtime.shared->fileTransfers->create(deviceId, wire::FileTransferStart::DOWNLOAD, context.worker());
    if (!bridge) {
        co_return formatMcpToolTextResult("Too many concurrent file transfers", true);
    }
    struct TransferGuard {
        std::shared_ptr<FileTransferHub> hub;
        std::shared_ptr<FileTransferBridge> bridge;
        ~TransferGuard() {
            hub->remove(bridge->transferId);
            bridge->close();
        }
    } guard{runtime.shared->fileTransfers, bridge};

    wire::FileTransferStart start;
    start.set_transfer_id(bridge->transferId);
    start.set_direction(wire::FileTransferStart::DOWNLOAD);
    start.set_path(path);
    if (!(co_await dispatchDeviceFileTransfer(context.worker(), runtime, deviceId, std::move(start)))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or does not support file transfer", true);
    }

    auto connected = co_await bridge->connectedReceiver->wait({.timeout_ = std::chrono::seconds(15), .stop_token_ = context.stop_token()});
    if (!connected.has_value() || !connected.value()) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, "transfer_timeout");
        co_return formatMcpToolTextResult("Timed out establishing file transfer connection", true);
    }

    auto firstRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(20), .stop_token_ = context.stop_token()});
    if (!firstRaw.has_value()) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, "transfer_aborted");
        co_return formatMcpToolTextResult("File transfer aborted before metadata", true);
    }
    const auto firstFrame = protocol::decodeFileTransferFrame(firstRaw.value());
    if (!firstFrame) {
        co_return formatMcpToolTextResult("File transfer protocol error", true);
    }
    if (firstFrame->has_finish()) {
        const auto err = std::string(protocol::fileErrorCodeName(firstFrame->finish().error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, err);
        co_return formatMcpToolTextResult("File read failed: " + err, true);
    }
    if (!firstFrame->has_meta()) {
        co_return formatMcpToolTextResult("Unexpected file transfer frame", true);
    }
    if (firstFrame->meta().archive_zip()) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, "is_directory");
        co_return formatMcpToolTextResult("Target path is a directory; use file_list instead", true);
    }

    std::string content;
    content.reserve(std::min<std::size_t>(kMaximumMcpFileReadBytes, firstFrame->meta().size()));
    bool sizeTruncated = false;
    while (true) {
        auto nextRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(30), .stop_token_ = context.stop_token()});
        if (!nextRaw.has_value()) {
            co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, "transfer_aborted");
            co_return formatMcpToolTextResult("File read interrupted", true);
        }
        const auto frame = protocol::decodeFileTransferFrame(nextRaw.value());
        if (!frame) {
            co_return formatMcpToolTextResult("File transfer protocol error", true);
        }
        if (frame->has_chunk()) {
            const auto& data = frame->chunk().data();
            if (content.size() < kMaximumMcpFileReadBytes) {
                const std::size_t take = std::min(data.size(), kMaximumMcpFileReadBytes - content.size());
                content.append(data.data(), take);
                if (take < data.size()) {
                    sizeTruncated = true;
                    break;
                }
            } else {
                sizeTruncated = true;
                break;
            }
        } else if (frame->has_finish()) {
            if (!frame->finish().success()) {
                const auto err = std::string(protocol::fileErrorCodeName(frame->finish().error()));
                co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", false, err);
                co_return formatMcpToolTextResult("File read failed: " + err, true);
            }
            break;
        } else {
            co_return formatMcpToolTextResult("Unexpected file transfer frame", true);
        }
    }

    const auto sliced = sliceTextLines(content, offsetLine, limitLines, sizeTruncated);
    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_read", true);
    std::string out = "{\"path\":";
    out.append(jsonEscape(path));
    out.append(",\"total_lines\":").append(std::to_string(sliced.totalLines));
    out.append(",\"start_line\":").append(std::to_string(sliced.startLine));
    out.append(",\"end_line\":").append(std::to_string(sliced.endLine));
    out.append(",\"truncated\":").append(sliced.truncated ? "true" : "false");
    out.append(",\"content\":").append(jsonEscape(sliced.content)).push_back('}');
    co_return formatMcpToolTextResult(out, false);
}

ruvia::task<std::string> runToolFileWrite(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<FileWriteArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for file_write", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());
    const auto path = textValue(args.get<"path">());
    const auto content = textValue(args.get<"content">());
    const bool overwrite = args.get<"overwrite">().value_or(true);
    if (!protocol::validFilePathText(path, false) || content.size() > kMaximumMcpFileWriteBytes) {
        co_return formatMcpToolTextResult("Invalid path or content exceeds 128 KiB limit", true);
    }

    auto& runtime = context.worker_state<ServerRuntime>();
    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeFilesWrite, true))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing files:write scope", true);
    }

    auto bridge = runtime.shared->fileTransfers->create(deviceId, wire::FileTransferStart::UPLOAD, context.worker());
    if (!bridge) {
        co_return formatMcpToolTextResult("Too many concurrent file transfers", true);
    }
    struct TransferGuard {
        std::shared_ptr<FileTransferHub> hub;
        std::shared_ptr<FileTransferBridge> bridge;
        ~TransferGuard() {
            hub->remove(bridge->transferId);
            bridge->close();
        }
    } guard{runtime.shared->fileTransfers, bridge};

    wire::FileTransferStart start;
    start.set_transfer_id(bridge->transferId);
    start.set_direction(wire::FileTransferStart::UPLOAD);
    start.set_path(path);
    start.set_overwrite(overwrite);
    if (!(co_await dispatchDeviceFileTransfer(context.worker(), runtime, deviceId, std::move(start)))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or does not support file transfer", true);
    }

    auto connected = co_await bridge->connectedReceiver->wait({.timeout_ = std::chrono::seconds(15), .stop_token_ = context.stop_token()});
    if (!connected.has_value() || !connected.value()) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, "transfer_timeout");
        co_return formatMcpToolTextResult("Timed out establishing file transfer connection", true);
    }

    auto readyRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(15), .stop_token_ = context.stop_token()});
    if (!readyRaw.has_value()) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, "transfer_aborted");
        co_return formatMcpToolTextResult("File upload aborted before ready", true);
    }
    const auto readyFrame = protocol::decodeFileTransferFrame(readyRaw.value());
    if (!readyFrame) {
        co_return formatMcpToolTextResult("File transfer protocol error", true);
    }
    if (readyFrame->has_finish()) {
        const auto err = std::string(protocol::fileErrorCodeName(readyFrame->finish().error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, err);
        co_return formatMcpToolTextResult("File write failed: " + err, true);
    }
    if (!readyFrame->has_meta()) {
        co_return formatMcpToolTextResult("Unexpected file transfer frame", true);
    }
    const std::string filename = readyFrame->meta().filename();

    std::size_t offset = 0;
    while (offset < content.size()) {
        const std::size_t take = std::min(protocol::kFileTransferChunkBytes, content.size() - offset);
        wire::FileTransferFrame chunk;
        chunk.mutable_chunk()->set_data(content.data() + offset, take);
        const auto sent = co_await bridge->toAgent->first.send_wait(chunk.SerializeAsString(), {.timeout_ = std::chrono::seconds(30), .stop_token_ = context.stop_token()});
        if (!sent.accepted()) {
            co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, "transfer_aborted");
            co_return formatMcpToolTextResult("File write interrupted", true);
        }
        offset += take;
    }

    wire::FileTransferFrame done;
    done.mutable_finish()->set_success(true);
    done.mutable_finish()->set_bytes_transferred(content.size());
    if (!(co_await bridge->toAgent->first.send_wait(done.SerializeAsString(), {.timeout_ = std::chrono::seconds(20), .stop_token_ = context.stop_token()})).accepted()) {
        co_return formatMcpToolTextResult("File write finish failed", true);
    }

    auto finalRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(20), .stop_token_ = context.stop_token()});
    if (!finalRaw.has_value()) {
        co_return formatMcpToolTextResult("File write commit timeout", true);
    }
    const auto finalFrame = protocol::decodeFileTransferFrame(finalRaw.value());
    if (!finalFrame || !finalFrame->has_finish()) {
        co_return formatMcpToolTextResult("File transfer protocol error", true);
    }
    if (!finalFrame->finish().success()) {
        const auto err = std::string(protocol::fileErrorCodeName(finalFrame->finish().error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", false, err);
        co_return formatMcpToolTextResult("File write failed: " + err, true);
    }

    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "file_write", true);
    std::string out = "{\"path\":";
    out.append(jsonEscape(path));
    out.append(",\"filename\":").append(jsonEscape(filename));
    out.append(",\"bytes_transferred\":").append(std::to_string(finalFrame->finish().bytes_transferred())).push_back('}');
    co_return formatMcpToolTextResult(out, false);
}

ruvia::task<std::string> runToolDesktopScreenshot(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<DesktopScreenshotArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for desktop_screenshot", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());

    auto& runtime = context.worker_state<ServerRuntime>();
    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeDesktopRead, false))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_screenshot", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing desktop:read scope", true);
    }

    auto waiter = runtime.shared->desktopSnapshots->create(deviceId, context.worker());
    if (!waiter) {
        co_return formatMcpToolTextResult("Too many concurrent desktop screenshots", true);
    }
    struct SnapshotGuard {
        std::shared_ptr<DesktopSnapshotHub> hub;
        std::shared_ptr<DesktopSnapshotWaiter> waiter;
        ~SnapshotGuard() {
            hub->remove(waiter->snapshotId);
            waiter->cancel();
        }
    } guard{runtime.shared->desktopSnapshots, waiter};

    wire::DesktopSnapshotStart start;
    start.set_snapshot_id(waiter->snapshotId);
    if (const auto& idx = args.get<"display_index">()) {
        if (*idx < 0 || *idx >= 64) {
            co_return formatMcpToolTextResult("display_index must be 0..63", true);
        }
        start.set_display_index(static_cast<std::uint32_t>(*idx));
    }
    if (const auto& maxW = args.get<"max_width">()) {
        if (*maxW < 64 || *maxW > static_cast<std::int64_t>(protocol::kMaximumSnapshotDimension)) {
            co_return formatMcpToolTextResult("max_width must be 64..4096", true);
        }
        start.set_max_width(static_cast<std::uint32_t>(*maxW));
    } else {
        start.set_max_width(1280);
    }
    if (const auto& maxH = args.get<"max_height">()) {
        if (*maxH < 64 || *maxH > static_cast<std::int64_t>(protocol::kMaximumSnapshotDimension)) {
            co_return formatMcpToolTextResult("max_height must be 64..4096", true);
        }
        start.set_max_height(static_cast<std::uint32_t>(*maxH));
    } else {
        start.set_max_height(720);
    }

    if (!(co_await dispatchDeviceDesktopSnapshot(context.worker(), runtime, deviceId, std::move(start)))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_screenshot", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or does not support desktop_automation (Windows Agent >= 1.0.65 required)", true);
    }

    auto arrived = co_await waiter->receiver->wait({.timeout_ = std::chrono::seconds(18), .stop_token_ = context.stop_token()});
    if (!arrived.has_value()) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_screenshot", false, "timeout");
        co_return formatMcpToolTextResult("Timed out waiting for desktop screenshot", true);
    }
    const auto res = std::move(arrived).take_value();
    if (!res.success()) {
        const auto err = std::string(protocol::desktopToolErrorCodeName(res.error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_screenshot", false, err);
        co_return formatMcpToolTextResult("Desktop screenshot failed: " + err, true);
    }

    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_screenshot", true);
    std::string meta = "{";
    meta.append("\"width\":").append(std::to_string(res.width()));
    meta.append(",\"height\":").append(std::to_string(res.height()));
    meta.append(",\"display_width\":").append(std::to_string(res.display_width()));
    meta.append(",\"display_height\":").append(std::to_string(res.display_height()));
    meta.append(",\"cursor_x\":").append(std::to_string(res.cursor_x()));
    meta.append(",\"cursor_y\":").append(std::to_string(res.cursor_y()));
    meta.append(",\"cursor_visible\":").append(res.cursor_visible() ? "true" : "false");
    meta.append(",\"coordinate_space\":\"normalized_0_to_1000_default\"}");
    co_return formatMcpToolImageAndTextResult(base64Encode(res.png_data()), meta);
}

ruvia::task<std::string> runToolDesktopAction(ruvia::context& context, const AuthenticatedToken& token, std::string_view argsJson) {
    auto parsed = ruvia::parse_json<DesktopActionArgs>(argsJson, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        co_return formatMcpToolTextResult("Invalid arguments for desktop_action", true);
    }
    const auto args = std::move(parsed).take_value();
    const auto deviceId = textValue(args.get<"device_id">());
    const auto actionKind = parseDesktopActionKind(args.get<"action">().view());
    if (!actionKind) {
        co_return formatMcpToolTextResult("Unsupported desktop_action action", true);
    }

    auto& runtime = context.worker_state<ServerRuntime>();
    if (!(co_await runtime.tokens.authorizeDevice(context.db(), token, deviceId, kTokenScopeDesktopInput, true))) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_action", false, "forbidden");
        co_return formatMcpToolTextResult("Access denied for device or missing desktop:input scope", true);
    }

    wire::DesktopActionRequest req;
    req.set_request_id(1);
    req.set_action(*actionKind);
    req.set_normalized(args.get<"normalized">().value_or(true));
    if (const auto& idx = args.get<"display_index">()) {
        if (*idx < 0 || *idx >= 64) {
            co_return formatMcpToolTextResult("display_index must be 0..63", true);
        }
        req.set_display_index(static_cast<std::uint32_t>(*idx));
    }
    if (const auto& x = args.get<"x">()) {
        req.set_x(static_cast<std::int32_t>(*x));
    }
    if (const auto& y = args.get<"y">()) {
        req.set_y(static_cast<std::int32_t>(*y));
    }
    if (const auto& toX = args.get<"to_x">()) {
        req.set_to_x(static_cast<std::int32_t>(*toX));
    }
    if (const auto& toY = args.get<"to_y">()) {
        req.set_to_y(static_cast<std::int32_t>(*toY));
    }
    const auto btn = parseMouseButton(optionalTextValue(args.get<"button">()));
    if (!btn) {
        co_return formatMcpToolTextResult("Invalid mouse button", true);
    }
    req.set_button(*btn);
    if (const auto& delta = args.get<"wheel_delta">()) {
        req.set_wheel_delta(static_cast<std::int32_t>(*delta));
    } else if (*actionKind == wire::DesktopActionRequest::ACTION_SCROLL) {
        req.set_wheel_delta(-120);
    }
    req.set_wheel_horizontal(args.get<"wheel_horizontal">().value_or(false));
    if (const auto& keys = args.get<"keys">()) {
        for (const auto& keyName : *keys) {
            const auto vk = resolveVirtualKeyCode(keyName.view());
            if (!vk) {
                co_return formatMcpToolTextResult("Unknown key name: " + textValue(keyName), true);
            }
            req.add_keys(*vk);
        }
    }
    if (const auto& txt = args.get<"text">()) {
        req.set_text(textValue(*txt));
    }
    if (!protocol::validDesktopActionRequest(req)) {
        co_return formatMcpToolTextResult("Invalid desktop_action parameters", true);
    }

    const auto res = co_await requestDeviceDesktopAction(context.worker(), runtime, deviceId, std::move(req));
    if (!res) {
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_action", false, "device_unavailable");
        co_return formatMcpToolTextResult("Device is offline or does not support desktop_automation", true);
    }
    if (!res->success()) {
        const auto err = std::string(protocol::desktopToolErrorCodeName(res->error()));
        co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_action", false, err);
        co_return formatMcpToolTextResult("Desktop action failed: " + err, true);
    }

    co_await runtime.tokens.auditToolCall(context.db(), token, deviceId, "desktop_action", true);
    std::string out = "{\"ok\":true,\"display_width\":";
    out.append(std::to_string(res->display_width()));
    out.append(",\"display_height\":").append(std::to_string(res->display_height())).push_back('}');
    co_return formatMcpToolTextResult(out, false);
}
} // namespace

ruvia::task<ruvia::http_response> McpController::get(ruvia::context& context) {
    context.policy().header("Allow", "POST");
    context.policy().header("Cache-Control", "no-store");
    co_return context.error({.status_ = ruvia::http_status::method_not_allowed, .code_ = "method_not_allowed", .message_ = "Use POST for MCP requests"});
}

ruvia::task<ruvia::http_response> McpController::post(ruvia::context& context) {
    const auto& token = context.request_state<AuthenticatedToken>();
    const auto body = co_await context.request().collect_body({.stop_token_ = context.stop_token()});
    const auto parsed = parseMcpRequest(body);
    if (!parsed) {
        co_return mcpJsonResponse(context, formatJsonRpcError(parsed.error().idJson, parsed.error().code, parsed.error().message));
    }
    const auto& req = *parsed;

    if (req.isNotification) {
        context.policy().header("Cache-Control", "no-store");
        co_return context.body({.status_ = ruvia::http_status::accepted});
    }

    if (req.method == "initialize") {
        co_return mcpJsonResponse(context, formatJsonRpcResult(req.idJson, mcpInitializeResultJson()));
    }

    if (req.method == "ping") {
        co_return mcpJsonResponse(context, formatJsonRpcResult(req.idJson, "{}"));
    }

    if (req.method == "tools/list") {
        co_return mcpJsonResponse(context, formatJsonRpcResult(req.idJson, mcpToolsListJson()));
    }

    if (req.method == "tools/call") {
        if (!req.hasParams) {
            co_return mcpJsonResponse(context, formatJsonRpcError(req.idJson, -32602, "Missing params for tools/call"));
        }
        auto paramsParsed = ruvia::parse_json<ToolCallParams>(req.paramsJson, {.reject_unknown_fields_ = false});
        if (!paramsParsed.has_value()) {
            co_return mcpJsonResponse(context, formatJsonRpcError(req.idJson, -32602, "Invalid tools/call params"));
        }
        const auto params = std::move(paramsParsed).take_value();
        const std::string toolName = textValue(params.get<"name">());
        const std::string argsJson = params.get<"arguments">() ? std::string(params.get<"arguments">()->view()) : std::string("{}");

        std::string toolResult;
        if (toolName == "devices_list") {
            toolResult = co_await runToolDevicesList(context, token);
        } else if (toolName == "exec_command") {
            toolResult = co_await runToolExecCommand(context, token, argsJson);
        } else if (toolName == "file_list") {
            toolResult = co_await runToolFileList(context, token, argsJson);
        } else if (toolName == "file_stat") {
            toolResult = co_await runToolFileStat(context, token, argsJson);
        } else if (toolName == "file_read") {
            toolResult = co_await runToolFileRead(context, token, argsJson);
        } else if (toolName == "file_write") {
            toolResult = co_await runToolFileWrite(context, token, argsJson);
        } else if (toolName == "desktop_screenshot") {
            toolResult = co_await runToolDesktopScreenshot(context, token, argsJson);
        } else if (toolName == "desktop_action") {
            toolResult = co_await runToolDesktopAction(context, token, argsJson);
        } else {
            co_return mcpJsonResponse(context, formatJsonRpcError(req.idJson, -32602, "Unknown tool: " + toolName));
        }

        co_return mcpJsonResponse(context, formatJsonRpcResult(req.idJson, toolResult));
    }

    co_return mcpJsonResponse(context, formatJsonRpcError(req.idJson, -32601, "Method not found: " + req.method));
}
} // namespace ultraterimal::server
