#include "gateway.runtime.h"

#include <app.h>
#include <chrono>
#include <stdexcept>
#include <worker_timer.h>

#include "config/storage.h"
#include "modules/devices/device.service.h"
#include "modules/identity/identity.service.h"
#include "modules/sessions/session.service.h"

namespace ultraterimal::server {
GatewayRuntime::GatewayRuntime(const DatabaseConfig& database) : ownerId_(randomToken()) {
    pool_.start();
    const auto loop = pool_.loop(0);
    client_ = std::make_shared<ruvia::db_client>(loop, toRuviaDbConfig(database, 1));
    auto submitted = loop.submit([](std::shared_ptr<ruvia::db_client> client, std::string ownerId) -> ruvia::task<void> {
        co_await client->connect();
        const auto db = client->db();
        if (!(co_await acquireGatewayLease(db, ownerId))) {
            co_await client->shutdown();
            throw std::runtime_error("A gateway already owns this database");
        }
        co_await SessionService().recoverAfterRestart(db);
        co_await DeviceService().recoverRestarts(db);
    }(client_, ownerId_));
    if (!submitted.accepted()) {
        pool_.join();
        throw std::runtime_error("Cannot initialize gateway database lease");
    }
    try {
        std::move(submitted).take_value().take_value();
    } catch (...) {
        pool_.join();
        throw;
    }
}
GatewayRuntime::~GatewayRuntime() { stop(); }
void GatewayRuntime::start() {
    if (started_) {
        throw std::logic_error("Gateway lease watcher already started");
    }
    started_ = true;
    const auto loop = pool_.loop(0);
    auto submitted = loop.submit([](std::shared_ptr<ruvia::db_client> client, std::string ownerId, std::shared_ptr<std::atomic<bool>> failed, ruvia::stop_token stopToken, ruvia::worker_ref worker) -> ruvia::task<void> {
        const auto db = client->db();
        while (co_await ruvia::sleep_for(worker, std::chrono::seconds(2), stopToken) == ruvia::sleep_status::elapsed) {
            if (!(co_await renewGatewayLease(db, ownerId))) {
                *failed = true;
                ruvia::app().request_stop();
                break;
            }
        }
    }(client_, ownerId_, failed_, stop_.token(), loop.worker()));
    if (!submitted.accepted()) {
        throw std::runtime_error("Cannot watch gateway database lease");
    }
    watcher_.emplace(std::move(submitted).take_value());
}
void GatewayRuntime::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    stop_.request_stop();
    if (watcher_) {
        try {
            std::move(*watcher_).take_value();
        } catch (...) {
        }
        watcher_.reset();
    }
    if (client_) {
        const auto loop = pool_.loop(0);
        auto submitted = loop.submit([](std::shared_ptr<ruvia::db_client> client, std::string ownerId) -> ruvia::task<void> {
            co_await releaseGatewayLease(client->db(), ownerId);
            co_await client->shutdown();
        }(client_, ownerId_));
        if (submitted.accepted()) {
            try {
                std::move(submitted).take_value().take_value();
            } catch (...) {
            }
        }
        client_.reset();
    }
    pool_.join();
}
bool GatewayRuntime::failed() const { return failed_->load(); }
} // namespace ultraterimal::server
