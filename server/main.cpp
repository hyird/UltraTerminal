#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

#include <ruvia/web/app.h>
#include <ruvia/web/controller.h>
#include <spdlog/spdlog.h>

#include "config/gateway.runtime.h"
#include "config/request.schema.h"
#include "config/server.config.h"
#include "config/server.runtime.h"
#include "features/native/native.runtime.h"
#include "modules/database/database.service.h"

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
            const auto object = requestObject(input, {"username", "password"});
            IdentityService(config.database).bootstrap(requestText(object, "username", 64), requestText(object, "password", 1024));
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
        const auto runtime = std::make_shared<ServerRuntime>(config);
        ruvia::http_client_config telegramClient{};
        telegramClient.alias_ = "telegram";
        telegramClient.base_url_ = "https://api.telegram.org";
        telegramClient.connection_limits_ = {.max_open_ = 2, .max_idle_http1_ = 2, .max_idle_multiplexed_ = 2};
        telegramClient.max_buffered_requests_ = 32;
        telegramClient.request_timeout_ = std::chrono::seconds(10);
        telegramClient.max_response_bytes_ = 65536;
        auto& app = ruvia::app();
        if (!config.webRoot.empty()) {
            app.document_root({.root_ = config.webRoot, .static_options_ = {.cache_control_ = "no-cache", .index_file_ = "index.html", .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::only, .extensions_ = {"html", "js", "css", "json", "svg", "ico", "png", "woff", "woff2", "txt", "md", "msi", "gz"}}}, .precompress_gzip_ = true});
        }
        // Shared business state is confined to one application Worker.
        app.listen({.address_ = config.address, .http_ = config.port})
            .server({.worker_count_ = 1, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install, .idle_timeout_ = std::chrono::seconds(45), .request_header_timeout_ = std::chrono::seconds(10), .request_body_timeout_ = std::chrono::seconds(10), .write_timeout_ = std::chrono::seconds(10), .max_connections_per_worker_ = 512, .max_buffered_body_bytes_ = 65536, .max_websocket_message_bytes_ = ultraterimal::protocol::kMaximumMessageBytes})
            .client(std::move(telegramClient))
            .blocking_pool({.thread_count_ = 4, .queue_capacity_ = 64})
            .rate_limit({.rule_ = {.max_requests_ = 300, .window_ = std::chrono::minutes(1)}})
            .use_worker_state<std::shared_ptr<ServerRuntime>>([runtime] { return runtime; })
            .on_error([](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
                spdlog::warn("http_request_failed status={} code={}", error.status().value(), error.code());
                co_return context.error({.status_ = error.status(), .code_ = "request_failed", .message_ = "Request failed"});
            })
            .on_start([gateway, native] {
                gateway->start();
                native->start(ruvia::app().workers().at(0));
                spdlog::info("Gateway listening with HTTP behind reverse proxy");
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
