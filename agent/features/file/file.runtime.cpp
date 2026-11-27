#include "features/file/file.runtime.h"

#include <asio/steady_timer.hpp>
#include <asio_bridge.h>
#include <blocking_pool.h>
#include <chrono>
#include <memory>
#include <string>

#include "features/connection/connection.transport.h"
#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
std::string serializeFrame(const wire::FileTransferFrame& frame) {
    return frame.SerializeAsString();
}

struct OpenDownloadResult {
    std::shared_ptr<FileDownloadStream> stream;
    wire::FileErrorCode error = wire::FILE_ERROR_NONE;
};

struct ReadChunkResult {
    std::string data;
    wire::FileErrorCode error = wire::FILE_ERROR_NONE;
};

struct OpenUploadResult {
    std::shared_ptr<FileUploadSink> sink;
    wire::FileErrorCode error = wire::FILE_ERROR_NONE;
};
} // namespace

asio::awaitable<void> runFileTransfer(ruvia::event_loop loop, ConnectionConfig config, wire::FileTransferStart request) {
    auto transport = std::make_shared<DeviceTransport>(loop, config, protocol::kMaximumFileTransferFrameBytes, wire::NativeConnect::FILE_TRANSFER);
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::hours(2));
    deadline.async_wait([transport](std::error_code error) {
        if (!error) {
            transport->cancel();
        }
    });
    try {
        const auto credential = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [file = config.credentialFile] {
                                     return loadCredential(file);
                                 }))).take_value());
        co_await transport->connect(credential);

        wire::FileTransferFrame hello;
        hello.mutable_hello()->set_transfer_id(request.transfer_id());
        co_await transport->send(serializeFrame(hello));

        if (request.direction() == wire::FileTransferStart::DOWNLOAD) {
            auto opened = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [path = request.path()]() -> OpenDownloadResult {
                               auto result = FileDownloadStream::open(path);
                               if (!result) {
                                   return {nullptr, result.error()};
                               }
                               return {std::shared_ptr<FileDownloadStream>(std::move(*result)), wire::FILE_ERROR_NONE};
                           }))).take_value());
            if (!opened.stream) {
                wire::FileTransferFrame fail;
                fail.mutable_finish()->set_success(false);
                fail.mutable_finish()->set_error(opened.error);
                co_await transport->send(serializeFrame(fail));
                deadline.cancel();
                co_await transport->close();
                co_return;
            }
            auto stream = std::move(opened.stream);
            wire::FileTransferFrame meta;
            meta.mutable_meta()->set_filename(stream->filename());
            meta.mutable_meta()->set_size(stream->declaredSize());
            meta.mutable_meta()->set_archive_zip(stream->isArchiveZip());
            co_await transport->send(serializeFrame(meta));

            while (true) {
                auto chunk = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [stream]() -> ReadChunkResult {
                                  auto result = stream->readChunk(protocol::kFileTransferChunkBytes);
                                  if (!result) {
                                      return {{}, result.error()};
                                  }
                                  return {std::move(*result), wire::FILE_ERROR_NONE};
                              }))).take_value());
                if (chunk.error != wire::FILE_ERROR_NONE) {
                    wire::FileTransferFrame fail;
                    fail.mutable_finish()->set_success(false);
                    fail.mutable_finish()->set_error(chunk.error);
                    fail.mutable_finish()->set_bytes_transferred(stream->bytesProduced());
                    co_await transport->send(serializeFrame(fail));
                    break;
                }
                if (chunk.data.empty()) {
                    wire::FileTransferFrame done;
                    done.mutable_finish()->set_success(true);
                    done.mutable_finish()->set_error(wire::FILE_ERROR_NONE);
                    done.mutable_finish()->set_bytes_transferred(stream->bytesProduced());
                    co_await transport->send(serializeFrame(done));
                    break;
                }
                wire::FileTransferFrame frame;
                frame.mutable_chunk()->set_data(std::move(chunk.data));
                co_await transport->send(serializeFrame(frame));
            }
        } else if (request.direction() == wire::FileTransferStart::UPLOAD) {
            auto opened = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [path = request.path(), overwrite = request.overwrite()]() -> OpenUploadResult {
                               auto result = FileUploadSink::open(path, overwrite);
                               if (!result) {
                                   return {nullptr, result.error()};
                               }
                               return {std::shared_ptr<FileUploadSink>(std::move(*result)), wire::FILE_ERROR_NONE};
                           }))).take_value());
            if (!opened.sink) {
                wire::FileTransferFrame fail;
                fail.mutable_finish()->set_success(false);
                fail.mutable_finish()->set_error(opened.error);
                co_await transport->send(serializeFrame(fail));
                deadline.cancel();
                co_await transport->close();
                co_return;
            }
            auto sink = std::move(opened.sink);
            wire::FileTransferFrame ready;
            ready.mutable_meta()->set_filename(sink->filename());
            co_await transport->send(serializeFrame(ready));

            while (true) {
                const auto raw = co_await transport->receive();
                const auto parsed = protocol::decodeFileTransferFrame(raw);
                if (!parsed) {
                    wire::FileTransferFrame fail;
                    fail.mutable_finish()->set_success(false);
                    fail.mutable_finish()->set_error(wire::FILE_ERROR_IO_FAILED);
                    co_await transport->send(serializeFrame(fail));
                    break;
                }
                if (parsed->has_chunk()) {
                    const auto writeError = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [sink, data = parsed->chunk().data()] {
                                                 return sink->writeChunk(data);
                                             }))).take_value());
                    if (writeError != wire::FILE_ERROR_NONE) {
                        wire::FileTransferFrame fail;
                        fail.mutable_finish()->set_success(false);
                        fail.mutable_finish()->set_error(writeError);
                        fail.mutable_finish()->set_bytes_transferred(sink->bytesWritten());
                        co_await transport->send(serializeFrame(fail));
                        break;
                    }
                } else if (parsed->has_finish()) {
                    if (!parsed->finish().success()) {
                        break;
                    }
                    const auto commitError = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [sink] {
                                                  return sink->commit();
                                              }))).take_value());
                    wire::FileTransferFrame reply;
                    reply.mutable_finish()->set_success(commitError == wire::FILE_ERROR_NONE);
                    reply.mutable_finish()->set_error(commitError);
                    reply.mutable_finish()->set_bytes_transferred(sink->bytesWritten());
                    co_await transport->send(serializeFrame(reply));
                    break;
                } else {
                    break;
                }
            }
        }
        deadline.cancel();
        co_await transport->close();
    } catch (...) {
        deadline.cancel();
        transport->cancel();
    }
}
} // namespace ultraterimal::agent
