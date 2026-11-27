#include <algorithm>
#include <app.h>
#include <chrono>
#include <controller.h>
#include <db_executor.h>
#include <event_loop.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <security_middleware.h>
#include <string_view>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "common/http.h"
#include "config/gateway.runtime.h"
#include "config/server.config.h"
#include "config/server.runtime.h"
#include "config/storage.h"
#include "features/native/native.runtime.h"

namespace {
RUVIA_MODEL(BootstrapRequest,
            RUVIA_REQUIRED_FIELD(username, ruvia::string, RUVIA_NOT_EMPTY("username required"), RUVIA_MAX_BYTES(64, "username too long"), RUVIA_CUSTOM(ultraterimal::server::noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(password, ruvia::string, RUVIA_NOT_EMPTY("password required"), RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(ultraterimal::server::noNul, "invalid character")));
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    using namespace ultraterimal::server;
    try {
        if (argc != 3) {
            std::cerr << "Usage: gateway --serve|--migrate|--bootstrap <config.json>\n";
            return 2;
        }
        const auto config = loadServerConfig(argv[2]);
        const auto command = std::filesystem::path(argv[1]).string();
        if (command == "--migrate") {
            migrateDatabase(config.database);
            return 0;
        }
        if (command == "--bootstrap") {
            std::string input(4097, '\0');
            std::cin.read(input.data(), static_cast<std::streamsize>(input.size()));
            input.resize(static_cast<std::size_t>(std::cin.gcount()));
            if (input.size() > 4096) {
                throw std::invalid_argument("Bootstrap input too large");
            }
            auto parsed = ruvia::parse_json<BootstrapRequest>(input, {.reject_unknown_fields_ = true});
            if (!parsed.has_value()) {
                throw std::invalid_argument("Invalid bootstrap input");
            }
            const auto request = std::move(parsed).take_value();
            ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 16, .blocking_pool_ = ruvia::blocking_pool_options{.thread_count_ = 1}});
            pool.start();
            const auto loop = pool.loop(0);
            auto client = std::make_shared<ruvia::db_client>(loop, toRuviaDbConfig(config.database, 1));
            auto submitted = loop.submit([client, username = textValue(request.get<"username">()), password = textValue(request.get<"password">())]() -> ruvia::task<void> {
                co_await client->connect();
                std::exception_ptr failure;
                try {
                    co_await IdentityService().bootstrap(client->db(), username, password);
                } catch (...) {
                    failure = std::current_exception();
                }
                co_await client->shutdown();
                if (failure) {
                    std::rethrow_exception(failure);
                }
            }());
            if (!submitted.accepted()) {
                pool.join();
                throw std::runtime_error("Cannot run bootstrap task");
            }
            try {
                std::move(submitted).take_value().take_value();
            } catch (...) {
                pool.join();
                throw;
            }
            pool.join();
            return 0;
        }
        if (command != "--serve") {
            return 2;
        }
        if (config.securityKey.empty()) {
            throw std::runtime_error("security_key_file is required for serving");
        }
        const auto gateway = std::make_shared<GatewayRuntime>(config.database);
        const auto native = std::make_shared<NativeServer>(config);
        const auto shared = std::make_shared<ServerSharedState>();
        const auto workers = std::max<std::size_t>(1u, std::thread::hardware_concurrency());
        ruvia::http_client_config telegramClient{};
        telegramClient.alias_ = "telegram";
        telegramClient.base_url_ = "https://api.telegram.org";
        telegramClient.connection_limits_ = {.max_open_ = 2, .max_idle_http1_ = 2, .max_idle_multiplexed_ = 2};
        telegramClient.max_buffered_requests_ = 32;
        telegramClient.request_timeout_ = std::chrono::seconds(10);
        telegramClient.max_response_bytes_ = 65536;
        ruvia::security_headers_config securityHeaders{};
        securityHeaders.strict_transport_security_header_ = ruvia::default_security_header_policy::omit;
        securityHeaders.content_security_policy_ = "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' wss: https:";
        securityHeaders.existing_headers_ = ruvia::security_header_conflict_policy::preserve_existing;
        auto& app = ruvia::app();
        if (!config.webRoot.empty()) {
            app.document_root({.root_ = config.webRoot, .static_options_ = {.cache_control_ = "no-cache", .index_file_ = "index.html", .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::only, .extensions_ = {"html", "js", "css", "svg", "ico", "png", "woff", "woff2", "txt", "md"}}}, .precompress_gzip_ = true});
        }
        // Homogeneous multi-core Ruvia workers; mutable state is strictly partitioned per worker.
        app.listen({.address_ = config.address, .http_ = config.port})
            .workers({.worker_count_ = workers, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .server({.idle_timeout_ = std::chrono::seconds(45), .request_header_timeout_ = std::chrono::seconds(10), .request_body_timeout_ = std::chrono::seconds(10), .write_timeout_ = std::chrono::seconds(10), .max_connections_per_worker_ = 512, .max_buffered_body_bytes_ = 65536, .max_websocket_message_bytes_ = ultraterimal::protocol::kMaximumMessageBytes})
            .db({.config_ = toRuviaDbConfig(config.database)})
            .http_client(std::move(telegramClient))
            .blocking_pool({.thread_count_ = workers, .queue_capacity_ = std::max<std::size_t>(64u, workers * 16u)})
            .rate_limit({.rule_ = {.max_requests_ = 300, .window_ = std::chrono::minutes(1)}})
            .use<ruvia::security_headers_middleware>(securityHeaders)
            .use_worker_state<ServerRuntime>([config, shared] { return ServerRuntime(config, shared); })
            .on_error([](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
                spdlog::warn("http_request_failed status={} code={}", error.status().value(), error.code());
                co_return context.error({.status_ = error.status(), .code_ = "request_failed", .message_ = "Request failed"});
            })
            .on_start([gateway, native, workers] {
                gateway->start();
                native->start();
                spdlog::info("Gateway listening with HTTP behind reverse proxy workers={}", workers);
            })
            .on_stop([gateway, native] { native->stop(); gateway->stop(); })
            .run();
        if (gateway->failed()) {
            throw std::runtime_error("Gateway database lease lost");
        }
        return 0;
    } catch (const std::exception&) {
        spdlog::error("Gateway operation failed; check configuration, database availability and requested operation");
        return 1;
    }
}
