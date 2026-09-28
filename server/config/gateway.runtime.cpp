#include "gateway.runtime.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include <ruvia/web/App.h>

#include "modules/sessions/session.service.h"

namespace ultraterimal::server {
GatewayRuntime::GatewayRuntime(const std::string& database) : lease_(std::make_shared<pqxx::connection>(acquireGatewayLease(database))) {
    SessionService(database).recoverAfterRestart();
    DeviceService(database).recoverRestarts();
}
GatewayRuntime::~GatewayRuntime() { stop(); }
void GatewayRuntime::start() {
    if (started_) {
        throw std::logic_error("Gateway lease watcher already started");
    }
    started_ = true;
    const auto status = pool_.submit([lease = lease_, failed = failed_, stop = stop_.get_token()] {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            wake.wait_for(lock, stop, std::chrono::seconds(2), [] { return false; });
            if (!stop.stop_requested() && !gatewayLeaseAlive(*lease)) {
                *failed = true;
                ruvia::app().stop();
                break;
            }
        }
    });
    if (status != ruvia::BlockingSubmitStatus::kAccepted) {
        throw std::runtime_error("Cannot watch gateway database lease");
    }
}
void GatewayRuntime::stop() {
    stop_.request_stop();
    pool_.join();
}
bool GatewayRuntime::failed() const { return failed_->load(); }
} // namespace ultraterimal::server
