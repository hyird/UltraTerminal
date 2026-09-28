#include "status.controller.h"

#include <chrono>
#include <set>
#include <stdexcept>

#include <boost/json.hpp>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>

namespace ultraterimal::server {
namespace {
boost::json::object deviceJson(const DeviceInfo& device, const ServerRuntime& runtime) {
    const auto connection = runtime.controlConnections.find(device.id);
    return {{"id", device.id}, {"name", device.name}, {"online", connection != runtime.controlConnections.end() && connection->second->ready},
            {"last_seen", device.lastSeen ? boost::json::value(*device.lastSeen) : boost::json::value(nullptr)}};
}
ruvia::Task<void> send(ruvia::Context& context, boost::json::object event) {
    const auto bytes = boost::json::serialize(event);
    if (bytes.size() > 1024 * 1024) throw std::runtime_error("Device snapshot exceeds limit");
    co_await context.webSocket().text(bytes);
}
ruvia::Task<void> writeStatus(ruvia::Context& context, StatusSubscription& subscription, ruvia::TaskScope& scope, bool& awaitingPong) {
    try {
        auto& runtime = context.workerState<ServerRuntime>();
        const auto token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""));
        const auto devices = co_await context.runBlocking([service = runtime.devices, user = subscription.userId] { return service.list(user); });
        std::set<std::string> visible;
        boost::json::array snapshot;
        for (const auto& device : devices) {
            visible.insert(device.id);
            snapshot.push_back(deviceJson(device, runtime));
        }
        co_await send(context, {{"type", "snapshot"}, {"devices", std::move(snapshot)}});
        while (!scope.stopRequested()) {
            const auto event = co_await subscription.receive(scope.stopToken());
            if (!event || scope.stopRequested()) break;
            const auto user = co_await context.runBlocking([service = runtime.identities, token] { return service.authenticate(token); });
            if (!user || user->id != subscription.userId || subscription.closed()) break;
            if (*event == "h") {
                awaitingPong = true;
                co_await send(context, {{"type", "ping"}});
            } else if (event->starts_with("d:")) {
                const auto id = event->substr(2);
                const auto device = co_await context.runBlocking([service = runtime.devices, user = subscription.userId, id] { return service.find(user, id); });
                if (scope.stopRequested() || subscription.closed()) break;
                if (device) {
                    if (!visible.contains(id) && visible.size() >= 1000) break;
                    visible.insert(id);
                    co_await send(context, {{"type", "upsert"}, {"device", deviceJson(*device, runtime)}});
                } else if (visible.erase(id)) {
                    co_await send(context, {{"type", "remove"}, {"id", id}});
                }
            }
        }
    } catch (const std::exception&) { }
    subscription.close();
    context.webSocket().abort();
}
ruvia::Task<void> watchStatus(ruvia::Context& context, StatusSubscription& subscription, ruvia::TaskScope& scope, bool& awaitingPong) {
    while (co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(15), scope.stopToken()) == ruvia::TimerSleepResult::kElapsed) {
        if (awaitingPong || !subscription.enqueue("h")) {
            subscription.close();
            context.webSocket().abort();
            break;
        }
    }
}
} // namespace
ruvia::Task<void> StatusController::events(ruvia::Context& context) {
    auto subscription = context.workerState<ServerRuntime>().status.subscribe(context.worker(), context.requestState<UserIdentity>().id);
    if (!subscription) {
        co_await context.webSocket().close({.code = 1008, .reason = "Status subscription limit"});
        co_return;
    }
    ruvia::TaskScope scope(context.worker());
    bool awaitingPong = false;
    scope.spawn(writeStatus(context, *subscription, scope, awaitingPong));
    scope.spawn(watchStatus(context, *subscription, scope, awaitingPong));
    try {
        while (const auto message = co_await context.webSocket().read()) {
            if (message->binary() || message->payload() != "{\"type\":\"pong\"}" || !awaitingPong) break;
            awaitingPong = false;
        }
    } catch (const std::exception&) { }
    subscription->close();
    context.webSocket().abort();
    scope.requestStop();
    co_await scope.join();
}
} // namespace ultraterimal::server
