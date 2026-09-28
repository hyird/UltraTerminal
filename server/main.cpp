#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

#include <ruvia/web/App.h>
#include <ruvia/web/Controller.h>
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
        const auto gateway = std::make_shared<GatewayRuntime>(config.database);
        const auto native = std::make_shared<NativeServer>(config);
        auto& app = ruvia::app();
        if (!config.webRoot.empty()) {
            app.documentRoot({.root = config.webRoot, .staticOptions = {.cacheControl = "no-cache", .indexFile = "index.html", .fileTypes = {.kind = ruvia::StaticFileTypePolicy::Kind::kOnly, .extensions = {"html", "js", "css", "json", "svg", "ico", "png", "woff", "woff2", "txt", "md", "msi", "gz"}}}, .precompressGzip = true});
        }
        app.listen({.address = config.address, .http = config.port})
            .server({.workerCount = 1, .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall, .idleTimeout = std::chrono::seconds(45), .requestHeaderTimeout = std::chrono::seconds(10), .requestBodyTimeout = std::chrono::seconds(10), .writeTimeout = std::chrono::seconds(10), .maxConnectionsPerWorker = 512, .maxBufferedBodyBytes = 65536, .maxWebSocketMessageBytes = ultraterimal::protocol::kMaximumMessageBytes})
            .blockingPool({.threadCount = 4, .queueCapacity = 64})
            .rateLimit({.rule = {.maxRequests = 300, .window = std::chrono::minutes(1)}})
            .useWorkerState<ServerRuntime>([config] { return ServerRuntime(config); })
            .onError([](ruvia::Context& context, ruvia::HttpErrorInfo error) -> ruvia::Task<ruvia::HttpResponse> {
                co_return context.error({.status = error.status(), .code = "request_failed", .message = "Request failed"});
            })
            .onStart([gateway, native] {
                gateway->start();
                const auto workers = ruvia::app().workers();
                if (workers.size() != 1) {
                    throw std::runtime_error("Native gateway requires one business Worker");
                }
                native->start(workers.front());
                spdlog::info("Gateway listening with HTTP behind reverse proxy");
            })
            .onStop([gateway, native] { native->stop(); gateway->stop(); })
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
