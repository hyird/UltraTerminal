#include "features/desktop/tool.runtime.h"

#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio_bridge.h>
#include <blocking_pool.h>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

#include "features/connection/connection.transport.h"
#include "platform/platform.transport.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/handle.types.h"
#include "platform/windows/pipe.transport.h"

namespace ultraterimal::agent {
namespace {
struct ToolResources {
    std::uint32_t sessionId = 0;
    std::unique_ptr<NamedPipeTransport> pipe;
    std::unique_ptr<DesktopProcess> child;
};

asio::awaitable<wire::DesktopToolMessage> invokeDesktopTool(ruvia::event_loop loop, const std::filesystem::path& desktopExecutable, wire::DesktopToolMessage request) {
    if (desktopExecutable.empty()) {
        throw std::runtime_error("Desktop tool executable unavailable");
    }
    auto resources = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [executor = loop.executor(), executable = desktopExecutable] {
                          const auto selected = currentDesktopSession();
                          if (!selected || selected == 0xffffffff) {
                              throw std::runtime_error("No active desktop session");
                          }
                          DesktopIdentity identity(selected);
                          ToolResources out;
                          out.sessionId = identity.session();
                          out.pipe = std::make_unique<NamedPipeTransport>(executor, identity.pipeSid());
                          out.child = std::make_unique<DesktopProcess>(executor, identity, executable, out.pipe->name(), DesktopWorkerRole::kTool);
                          return out;
                      }))).take_value());

    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::seconds(15));
    deadline.async_wait([pipe = resources.pipe.get(), child = resources.child.get()](std::error_code error) {
        if (!error) {
            if (pipe) {
                pipe->cancel();
            }
            if (child) {
                try {
                    child->terminate();
                } catch (...) {
                }
            }
        }
    });

    wire::DesktopToolMessage reply;
    std::exception_ptr failure;
    try {
        co_await resources.pipe->accept(resources.child->id(), resources.sessionId);
        co_await resources.pipe->send(request.SerializeAsString());
        const auto rawReply = co_await resources.pipe->receive();
        auto decoded = protocol::decodeDesktopToolMessage(rawReply);
        if (!decoded) {
            throw std::runtime_error("Invalid desktop tool reply");
        }
        reply = std::move(*decoded);
    } catch (...) {
        failure = std::current_exception();
    }

    deadline.cancel();
    resources.pipe->cancel();
    resources.pipe.reset();
    if (resources.child) {
        asio::steady_timer reapGrace(loop.executor());
        reapGrace.expires_after(std::chrono::seconds(2));
        reapGrace.async_wait([child = resources.child.get()](std::error_code error) {
            if (!error && child) {
                try {
                    child->terminate();
                } catch (...) {
                }
            }
        });
        try {
            (void)co_await resources.child->wait();
        } catch (...) {
        }
        reapGrace.cancel();
        resources.child.reset();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return reply;
}
} // namespace

asio::awaitable<void> runDesktopSnapshot(ruvia::event_loop loop, ConnectionConfig config, std::filesystem::path desktopExecutable, wire::DesktopSnapshotStart request) {
    if (!loop.worker().is_current()) {
        throw std::logic_error("Desktop tool must run on its bound event loop");
    }
    wire::DesktopSnapshotResult result;
    result.set_snapshot_id(request.snapshot_id());
    try {
        wire::DesktopToolMessage toolRequest;
        *toolRequest.mutable_snapshot_request() = request;
        const auto reply = co_await invokeDesktopTool(loop, desktopExecutable, std::move(toolRequest));
        if (reply.has_snapshot_result()) {
            result = reply.snapshot_result();
        } else {
            result.set_success(false);
            result.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
        }
    } catch (...) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_UNAVAILABLE);
    }

    auto transport = std::make_shared<DeviceTransport>(loop, config, protocol::kMaximumSnapshotBytes + 4096, wire::NativeConnect::DESKTOP_SNAPSHOT);
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::seconds(20));
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
        co_await transport->send(result.SerializeAsString());
        deadline.cancel();
        co_await transport->close();
    } catch (...) {
        deadline.cancel();
        transport->cancel();
    }
}

asio::awaitable<wire::DesktopActionResult> runDesktopAction(ruvia::event_loop loop, std::filesystem::path desktopExecutable, wire::DesktopActionRequest request) {
    if (!loop.worker().is_current()) {
        throw std::logic_error("Desktop tool must run on its bound event loop");
    }
    wire::DesktopActionResult result;
    result.set_request_id(request.request_id() ? request.request_id() : 1);
    try {
        wire::DesktopToolMessage toolRequest;
        *toolRequest.mutable_action_request() = request;
        const auto reply = co_await invokeDesktopTool(loop, desktopExecutable, std::move(toolRequest));
        if (reply.has_action_result()) {
            co_return reply.action_result();
        }
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
        co_return result;
    } catch (...) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_UNAVAILABLE);
        co_return result;
    }
}
} // namespace ultraterimal::agent
