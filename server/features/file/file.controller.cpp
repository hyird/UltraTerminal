#include "features/file/file.controller.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <model.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "common/http.h"
#include "config/server.runtime.h"
#include "ultraterimal/protocol/file.protocol.h"

namespace ultraterimal::server {
namespace {
RUVIA_MODEL(FileEntryResponse,
            RUVIA_REQUIRED_FIELD(name, ruvia::string),
            RUVIA_REQUIRED_FIELD(directory, bool),
            RUVIA_REQUIRED_FIELD(size, std::uint64_t),
            RUVIA_REQUIRED_FIELD(modified_unix_seconds, std::int64_t));

RUVIA_MODEL(FileListResponse,
            RUVIA_REQUIRED_FIELD(normalized_path, ruvia::string),
            RUVIA_REQUIRED_FIELD(parent_path, ruvia::string),
            RUVIA_REQUIRED_FIELD(truncated, bool),
            RUVIA_REQUIRED_FIELD(entries, ruvia::array<FileEntryResponse>));

RUVIA_MODEL(FileStatResponse,
            RUVIA_REQUIRED_FIELD(exists, bool),
            RUVIA_REQUIRED_FIELD(directory, bool),
            RUVIA_REQUIRED_FIELD(size, std::uint64_t),
            RUVIA_REQUIRED_FIELD(name, ruvia::string));

RUVIA_MODEL(FileUploadResponse,
            RUVIA_REQUIRED_FIELD(filename, ruvia::string),
            RUVIA_REQUIRED_FIELD(bytes_transferred, std::uint64_t));

ruvia::http_status_code fileErrorStatus(wire::FileErrorCode error) {
    switch (error) {
    case wire::FILE_ERROR_INVALID_PATH:
    case wire::FILE_ERROR_IS_DIRECTORY:
    case wire::FILE_ERROR_NOT_DIRECTORY:
        return ruvia::http_status::bad_request;
    case wire::FILE_ERROR_NOT_FOUND:
        return ruvia::http_status::not_found;
    case wire::FILE_ERROR_PERMISSION_DENIED:
        return ruvia::http_status::forbidden;
    case wire::FILE_ERROR_FILE_LOCKED:
    case wire::FILE_ERROR_ALREADY_EXISTS:
        return ruvia::http_status::conflict;
    case wire::FILE_ERROR_LIMIT_EXCEEDED:
        return ruvia::http_status::content_too_large;
    default:
        return ruvia::http_status::internal_server_error;
    }
}

std::string_view fileErrorMessage(wire::FileErrorCode error) {
    switch (error) {
    case wire::FILE_ERROR_INVALID_PATH:
        return "路径无效";
    case wire::FILE_ERROR_NOT_FOUND:
        return "文件或目录不存在";
    case wire::FILE_ERROR_PERMISSION_DENIED:
        return "无权访问目标文件或目录";
    case wire::FILE_ERROR_FILE_LOCKED:
        return "目标文件正在被其他程序占用";
    case wire::FILE_ERROR_ALREADY_EXISTS:
        return "目标文件已存在";
    case wire::FILE_ERROR_IS_DIRECTORY:
        return "目标路径是目录，无法覆盖为文件";
    case wire::FILE_ERROR_NOT_DIRECTORY:
        return "目标路径不是目录";
    case wire::FILE_ERROR_LIMIT_EXCEEDED:
        return "目录文件数或总大小超过打包上限";
    case wire::FILE_ERROR_CANCELLED:
        return "文件传输已取消";
    default:
        return "文件读写失败";
    }
}

std::string percentEncodeRfc5987(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

std::string safeAsciiFallbackName(std::string_view filename) {
    std::string out;
    out.reserve(filename.size());
    for (unsigned char c : filename) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\' && c != ';') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "download" : out;
}
} // namespace

ruvia::task<ruvia::http_response> FileController::list(ruvia::context& context) {
    const auto deviceId = std::string(context.request().query("device_id").value_or(""));
    const auto sessionId = std::string(context.request().query("session_id").value_or(""));
    const auto path = std::string(context.request().query("path").value_or(""));
    if (deviceId.empty() || deviceId.size() > 128 || sessionId.empty() || sessionId.size() > 128 || !protocol::validFilePathText(path, true)) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_request", .message_ = "Invalid device_id, session_id or path"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto& user = context.request_state<UserIdentity>();
    if (!(co_await runtime.sessions.authorizeDesktopFileTransfer(context.db(), user, deviceId, sessionId, false))) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "需要有效的远程桌面会话才能访问文件"});
    }

    wire::FileListRequest req;
    req.set_path(path);
    const auto result = co_await requestDeviceFileList(context.worker(), runtime, deviceId, std::move(req));
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "device_unavailable", .message_ = "设备离线或当前 Agent 版本不支持文件传输"});
    }
    if (!result->success()) {
        co_return context.error({.status_ = fileErrorStatus(result->error()), .code_ = protocol::fileErrorCodeName(result->error()), .message_ = fileErrorMessage(result->error())});
    }

    FileListResponse response;
    response.set<"normalized_path">(result->normalized_path());
    response.set<"parent_path">(result->parent_path());
    response.set<"truncated">(result->truncated());
    auto& entries = response.ensure<"entries">();
    entries.reserve(static_cast<std::size_t>(result->entries_size()));
    for (const auto& item : result->entries()) {
        FileEntryResponse entry;
        entry.set<"name">(item.name());
        entry.set<"directory">(item.directory());
        entry.set<"size">(item.size());
        entry.set<"modified_unix_seconds">(item.modified_unix_seconds());
        entries.push_back(std::move(entry));
    }
    co_return jsonResponse(context, response);
}

ruvia::task<ruvia::http_response> FileController::stat(ruvia::context& context) {
    const auto deviceId = std::string(context.request().query("device_id").value_or(""));
    const auto sessionId = std::string(context.request().query("session_id").value_or(""));
    const auto path = std::string(context.request().query("path").value_or(""));
    if (deviceId.empty() || deviceId.size() > 128 || sessionId.empty() || sessionId.size() > 128 || !protocol::validFilePathText(path, false)) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_request", .message_ = "Invalid device_id, session_id or path"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto& user = context.request_state<UserIdentity>();
    if (!(co_await runtime.sessions.authorizeDesktopFileTransfer(context.db(), user, deviceId, sessionId, false))) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "需要有效的远程桌面会话才能访问文件"});
    }

    wire::FileStatRequest req;
    req.set_path(path);
    const auto result = co_await requestDeviceFileStat(context.worker(), runtime, deviceId, std::move(req));
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "device_unavailable", .message_ = "设备离线或当前 Agent 版本不支持文件传输"});
    }
    if (!result->success()) {
        co_return context.error({.status_ = fileErrorStatus(result->error()), .code_ = protocol::fileErrorCodeName(result->error()), .message_ = fileErrorMessage(result->error())});
    }

    FileStatResponse response;
    response.set<"exists">(result->exists());
    response.set<"directory">(result->directory());
    response.set<"size">(result->size());
    response.set<"name">(result->name());
    co_return jsonResponse(context, response);
}

ruvia::task<void> FileController::download(ruvia::context& context) {
    const auto deviceId = std::string(context.request().query("device_id").value_or(""));
    const auto sessionId = std::string(context.request().query("session_id").value_or(""));
    const auto path = std::string(context.request().query("path").value_or(""));
    if (deviceId.empty() || deviceId.size() > 128 || sessionId.empty() || sessionId.size() > 128 || !protocol::validFilePathText(path, false)) {
        context.respond(context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_request", .message_ = "Invalid device_id, session_id or path"}));
        co_return;
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto& user = context.request_state<UserIdentity>();
    if (!(co_await runtime.sessions.authorizeDesktopFileTransfer(context.db(), user, deviceId, sessionId, false))) {
        context.respond(context.error({.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "需要有效的远程桌面会话才能下载文件"}));
        co_return;
    }

    auto bridge = runtime.shared->fileTransfers->create(deviceId, wire::FileTransferStart::DOWNLOAD, context.worker());
    if (!bridge) {
        context.respond(context.error({.status_ = ruvia::http_status::too_many_requests, .code_ = "too_many_transfers", .message_ = "并发文件传输数已达上限"}));
        co_return;
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
        context.respond(context.error({.status_ = ruvia::http_status::conflict, .code_ = "device_unavailable", .message_ = "设备离线或当前 Agent 版本不支持文件传输"}));
        co_return;
    }

    auto connected = co_await bridge->connectedReceiver->wait({.timeout_ = std::chrono::seconds(15), .stop_token_ = context.stop_token()});
    if (!connected.has_value() || !connected.value()) {
        context.respond(context.error({.status_ = ruvia::http_status::gateway_timeout, .code_ = "transfer_timeout", .message_ = "等待设备建立文件传输连接超时"}));
        co_return;
    }

    // First frame from Agent is either FileTransferMeta (success) or FileTransferFinish (open failed).
    auto firstRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(30), .stop_token_ = context.stop_token()});
    if (!firstRaw.has_value()) {
        context.respond(context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "transfer_aborted", .message_ = "设备连接中断"}));
        co_return;
    }
    const auto firstFrame = protocol::decodeFileTransferFrame(firstRaw.value());
    if (!firstFrame) {
        context.respond(context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "protocol_error", .message_ = "文件传输协议错误"}));
        co_return;
    }
    if (firstFrame->has_finish()) {
        const auto err = firstFrame->finish().error();
        context.respond(context.error({.status_ = fileErrorStatus(err), .code_ = protocol::fileErrorCodeName(err), .message_ = fileErrorMessage(err)}));
        co_return;
    }
    if (!firstFrame->has_meta()) {
        context.respond(context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "protocol_error", .message_ = "文件传输协议错误"}));
        co_return;
    }

    const auto& meta = firstFrame->meta();
    const std::string contentType = meta.archive_zip() ? "application/zip" : "application/octet-stream";
    const std::string disposition = "attachment; filename=\"" + safeAsciiFallbackName(meta.filename()) + "\"; filename*=UTF-8''" + percentEncodeRfc5987(meta.filename());
    const std::string contentLength = (!meta.archive_zip() && meta.size() > 0) ? std::to_string(meta.size()) : std::string{};
    std::vector<ruvia::http_header_view> headers;
    headers.push_back({"Content-Type", contentType});
    headers.push_back({"Content-Disposition", disposition});
    headers.push_back({"Cache-Control", "no-store"});
    if (!contentLength.empty()) {
        headers.push_back({"Content-Length", contentLength});
    }

    auto& writer = context.stream({.status_ = ruvia::http_status::ok, .headers_ = headers});
    const auto commitResult = co_await writer.commit();
    if (commitResult != ruvia::stream_commit_result::body) {
        co_return;
    }

    while (!writer.aborted()) {
        auto nextRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(60), .stop_token_ = context.stop_token()});
        if (!nextRaw.has_value()) {
            throw std::runtime_error("file_download_interrupted");
        }
        const auto frame = protocol::decodeFileTransferFrame(nextRaw.value());
        if (!frame) {
            throw std::runtime_error("file_download_protocol_error");
        }
        if (frame->has_chunk()) {
            if (!frame->chunk().data().empty()) {
                co_await writer.write(std::string(frame->chunk().data()));
            }
        } else if (frame->has_finish()) {
            if (!frame->finish().success()) {
                spdlog::warn("file_download_failed device_id={} user_id={} error={}", deviceId, user.id, protocol::fileErrorCodeName(frame->finish().error()));
                co_await runtime.sessions.auditFileTransfer(context.db(), user.id, deviceId, sessionId, "file.download", false, std::string(protocol::fileErrorCodeName(frame->finish().error())));
                throw std::runtime_error("file_download_agent_error");
            }
            co_await writer.finish();
            co_await runtime.sessions.auditFileTransfer(context.db(), user.id, deviceId, sessionId, "file.download", true, meta.archive_zip() ? "zip" : "file");
            spdlog::info("file_download_completed device_id={} user_id={} zip={} bytes={}", deviceId, user.id, meta.archive_zip(), frame->finish().bytes_transferred());
            co_return;
        } else {
            throw std::runtime_error("file_download_unexpected_frame");
        }
    }
}

ruvia::task<ruvia::http_response> FileController::upload(ruvia::context& context) {
    const auto deviceId = std::string(context.request().query("device_id").value_or(""));
    const auto sessionId = std::string(context.request().query("session_id").value_or(""));
    const auto path = std::string(context.request().query("path").value_or(""));
    const bool overwrite = context.request().query("overwrite").value_or("0") == "1";
    if (deviceId.empty() || deviceId.size() > 128 || sessionId.empty() || sessionId.size() > 128 || !protocol::validFilePathText(path, false)) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_request", .message_ = "Invalid device_id, session_id or path"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto& user = context.request_state<UserIdentity>();
    if (!(co_await runtime.sessions.authorizeDesktopFileTransfer(context.db(), user, deviceId, sessionId, true))) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "需要有效的可控制远程桌面会话才能上传文件"});
    }

    auto bridge = runtime.shared->fileTransfers->create(deviceId, wire::FileTransferStart::UPLOAD, context.worker());
    if (!bridge) {
        co_return context.error({.status_ = ruvia::http_status::too_many_requests, .code_ = "too_many_transfers", .message_ = "并发文件传输数已达上限"});
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
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "device_unavailable", .message_ = "设备离线或当前 Agent 版本不支持文件传输"});
    }

    auto connected = co_await bridge->connectedReceiver->wait({.timeout_ = std::chrono::seconds(15), .stop_token_ = context.stop_token()});
    if (!connected.has_value() || !connected.value()) {
        co_return context.error({.status_ = ruvia::http_status::gateway_timeout, .code_ = "transfer_timeout", .message_ = "等待设备建立文件传输连接超时"});
    }

    // Wait for Agent ready meta or immediate failure (e.g. ALREADY_EXISTS / PERMISSION_DENIED).
    auto readyRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(15), .stop_token_ = context.stop_token()});
    if (!readyRaw.has_value()) {
        co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "transfer_aborted", .message_ = "设备连接中断"});
    }
    const auto readyFrame = protocol::decodeFileTransferFrame(readyRaw.value());
    if (!readyFrame) {
        co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "protocol_error", .message_ = "文件传输协议错误"});
    }
    if (readyFrame->has_finish()) {
        const auto err = readyFrame->finish().error();
        co_return context.error({.status_ = fileErrorStatus(err), .code_ = protocol::fileErrorCodeName(err), .message_ = fileErrorMessage(err)});
    }
    if (!readyFrame->has_meta()) {
        co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "protocol_error", .message_ = "文件传输协议错误"});
    }
    const std::string filename = readyFrame->meta().filename();

    auto& body = context.request_body();
    std::uint64_t totalSent = 0;
    while (true) {
        auto piece = co_await body.read({.stop_token_ = context.stop_token()});
        if (!piece.has_value()) {
            break;
        }
        const auto bytes = *piece;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t take = std::min(protocol::kFileTransferChunkBytes, bytes.size() - offset);
            wire::FileTransferFrame chunk;
            chunk.mutable_chunk()->set_data(reinterpret_cast<const char*>(bytes.data() + offset), take);
            const auto sent = co_await bridge->toAgent->first.send_wait(chunk.SerializeAsString(), {.timeout_ = std::chrono::seconds(60), .stop_token_ = context.stop_token()});
            if (!sent.accepted()) {
                // Agent may have sent an error finish frame before closing.
                auto errRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(2)});
                if (errRaw.has_value()) {
                    if (const auto errFrame = protocol::decodeFileTransferFrame(errRaw.value()); errFrame && errFrame->has_finish()) {
                        const auto err = errFrame->finish().error();
                        co_return context.error({.status_ = fileErrorStatus(err), .code_ = protocol::fileErrorCodeName(err), .message_ = fileErrorMessage(err)});
                    }
                }
                co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "transfer_aborted", .message_ = "文件上传中断"});
            }
            offset += take;
            totalSent += take;
        }
    }

    wire::FileTransferFrame done;
    done.mutable_finish()->set_success(true);
    done.mutable_finish()->set_bytes_transferred(totalSent);
    if (!(co_await bridge->toAgent->first.send_wait(done.SerializeAsString(), {.timeout_ = std::chrono::seconds(30), .stop_token_ = context.stop_token()})).accepted()) {
        co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "transfer_aborted", .message_ = "文件上传确认失败"});
    }

    auto finalRaw = co_await bridge->toHttp.second.receive({.timeout_ = std::chrono::seconds(30), .stop_token_ = context.stop_token()});
    if (!finalRaw.has_value()) {
        co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "transfer_aborted", .message_ = "等待设备落盘确认失败"});
    }
    const auto finalFrame = protocol::decodeFileTransferFrame(finalRaw.value());
    if (!finalFrame || !finalFrame->has_finish()) {
        co_return context.error({.status_ = ruvia::http_status::bad_gateway, .code_ = "protocol_error", .message_ = "文件传输协议错误"});
    }
    if (!finalFrame->finish().success()) {
        const auto err = finalFrame->finish().error();
        co_await runtime.sessions.auditFileTransfer(context.db(), user.id, deviceId, sessionId, "file.upload", false, std::string(protocol::fileErrorCodeName(err)));
        co_return context.error({.status_ = fileErrorStatus(err), .code_ = protocol::fileErrorCodeName(err), .message_ = fileErrorMessage(err)});
    }

    co_await runtime.sessions.auditFileTransfer(context.db(), user.id, deviceId, sessionId, "file.upload", true, overwrite ? "overwrite" : "create");
    spdlog::info("file_upload_completed device_id={} user_id={} bytes={}", deviceId, user.id, finalFrame->finish().bytes_transferred());
    FileUploadResponse response;
    response.set<"filename">(filename);
    response.set<"bytes_transferred">(finalFrame->finish().bytes_transferred());
    co_return jsonResponse(context, response);
}
} // namespace ultraterimal::server
