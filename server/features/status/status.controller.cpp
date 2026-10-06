#include "status.controller.h"

#include <chrono>
#include <set>
#include <stdexcept>

#include <boost/json.hpp>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>

namespace ultraterimal::server {
namespace {
boost::json::object deviceJson(const DeviceInfo& device, const ServerRuntime& runtime) {
    const auto connection = runtime.controlConnections.find(device.id);
    return {{"agent_version", connection != runtime.controlConnections.end() && connection->second->ready ? boost::json::value(connection->second->agentVersion) : boost::json::value(nullptr)}, {"id", device.id}, {"name", device.name}, {"owner_username", device.ownerUsername}, {"online", connection != runtime.controlConnections.end() && connection->second->ready}, {"enrolled", device.enrolled}, {"can_manage", device.canManage}, {"desktop", device.desktop}, {"write_control", device.writeControl}, {"verification_method", device.verificationMethod}, {"manage_credentials", device.manageCredentials}, {"last_seen", device.lastSeen ? boost::json::value(*device.lastSeen) : boost::json::value(nullptr)}};
}
ruvia::task<void> send(ruvia::context& context, boost::json::object event) {
    const auto bytes = boost::json::serialize(event);
    if (bytes.size() > 1024 * 1024) {
        throw std::runtime_error("Device snapshot exceeds limit");
    }
    co_await context.websocket().text(bytes);
}
ruvia::task<void> sendAccount(ruvia::context& context, const UserIdentity& user) {
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const auto telegram = (co_await context.run_blocking([service = runtime.telegram, user] { return service.state(user.id); })).value();
    const auto approvals = (co_await context.run_blocking([service = runtime.telegram, user] { return service.approvals(user); })).value();
    boost::json::array items;
    for (const auto& a : approvals) {
        items.push_back(boost::json::object{{"id", a.id}, {"device_id", a.deviceId}, {"state", a.state}, {"write_control", a.writeControl}, {"expires_at", a.expiresAt}});
    }
    co_await send(context, {{"type", "account"}, {"telegram", boost::json::object{{"bot_username", telegram.botUsername}, {"telegram_id", telegram.telegramId}}}, {"approvals", items}});
}
ruvia::task<void> writeStatus(ruvia::context& context, StatusSubscription& subscription, ruvia::task_scope& scope, bool& awaitingPong) {
    try {
        auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
        const auto token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""));
        const bool administration = context.req().query("scope").value_or("") == "administration";
        if (administration && context.request_state<UserIdentity>().role != "admin") {
            throw std::runtime_error("Administrator required");
        }
        const auto devices = (co_await context.run_blocking([service = runtime.devices, user = subscription.userId, administration] { return service.list(user, administration); })).value();
        std::set<std::string> visible;
        boost::json::array snapshot;
        for (const auto& device : devices) {
            visible.insert(device.id);
            snapshot.push_back(deviceJson(device, runtime));
        }
        co_await send(context, {{"type", "snapshot"}, {"devices", std::move(snapshot)}});
        co_await sendAccount(context, context.request_state<UserIdentity>());
        while (!scope.stop_requested()) {
            const auto event = co_await subscription.receive(scope.stop_token());
            if (!event || scope.stop_requested()) {
                break;
            }
            const auto user = (co_await context.run_blocking([service = runtime.identities, token] { return service.authenticate(token); })).value();
            if (!user || user->id != subscription.userId || (administration && user->role != "admin") || subscription.closed()) {
                break;
            }
            if (*event == "h") {
                awaitingPong = true;
                co_await send(context, {{"type", "ping"}});
            } else if (event->starts_with("u:")) {
                co_await sendAccount(context, *user);
            } else if (event->starts_with("d:")) {
                const auto id = event->substr(2);
                const auto device = (co_await context.run_blocking([service = runtime.devices, user = subscription.userId, id, administration] { return service.find(user, id, administration); })).value();
                if (scope.stop_requested() || subscription.closed()) {
                    break;
                }
                if (device) {
                    if (!visible.contains(id) && visible.size() >= 1000) {
                        break;
                    }
                    visible.insert(id);
                    co_await send(context, {{"type", "upsert"}, {"device", deviceJson(*device, runtime)}});
                } else if (visible.erase(id)) {
                    co_await send(context, {{"type", "remove"}, {"id", id}});
                }
            }
        }
    } catch (const std::exception&) {
    }
    subscription.close();
    context.websocket().abort();
}
ruvia::task<void> watchStatus(ruvia::context& context, StatusSubscription& subscription, ruvia::task_scope& scope, bool& awaitingPong) {
    while (co_await ruvia::sleep_for(context.execution(), std::chrono::seconds(15), scope.stop_token()) == ruvia::timer_sleep_result::elapsed) {
        if (awaitingPong || !subscription.enqueue("h")) {
            subscription.close();
            context.websocket().abort();
            break;
        }
    }
}
} // namespace
ruvia::task<void> StatusController::events(ruvia::context& context) const {
    auto subscription = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).status.subscribe(context.execution(), context.request_state<UserIdentity>().id);
    if (!subscription) {
        co_await context.websocket().close({.code_ = 1008, .reason_ = "Status subscription limit"});
        co_return;
    }
    co_await ruvia::with_task_scope(context.execution(), [&](ruvia::task_scope& scope) -> ruvia::task<void> {
        bool awaitingPong = false;
        scope.spawn(writeStatus(context, *subscription, scope, awaitingPong));
        scope.spawn(watchStatus(context, *subscription, scope, awaitingPong));
        try {
            while (const auto message = co_await context.websocket().read()) {
                if (message->binary() || message->payload() != "{\"type\":\"pong\"}" || !awaitingPong) {
                    break;
                }
                awaitingPong = false;
            }
        } catch (const std::exception&) {
        }
        subscription->close();
        context.websocket().abort();
        scope.request_stop();
        co_await scope.join();
    });
}
} // namespace ultraterimal::server
