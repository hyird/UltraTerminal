#include "status.controller.h"

#include <chrono>
#include <exception>
#include <model.h>
#include <set>
#include <stdexcept>
#include <streaming.h>
#include <task_scope.h>

namespace ultraterimal::server {
namespace {
RUVIA_MODEL(StatusDeviceItem,
            RUVIA_OPTIONAL_FIELD(agent_version, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(name, ruvia::string),
            RUVIA_REQUIRED_FIELD(owner_username, ruvia::string),
            RUVIA_REQUIRED_FIELD(online, bool),
            RUVIA_REQUIRED_FIELD(enrolled, bool),
            RUVIA_REQUIRED_FIELD(can_manage, bool),
            RUVIA_REQUIRED_FIELD(desktop, bool),
            RUVIA_REQUIRED_FIELD(write_control, bool),
            RUVIA_REQUIRED_FIELD(verification_method, ruvia::string),
            RUVIA_REQUIRED_FIELD(manage_credentials, bool),
            RUVIA_OPTIONAL_FIELD(last_seen, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL));
RUVIA_MODEL(StatusApprovalItem,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string),
            RUVIA_REQUIRED_FIELD(state, ruvia::string),
            RUVIA_REQUIRED_FIELD(write_control, bool),
            RUVIA_REQUIRED_FIELD(expires_at, std::int64_t));
RUVIA_MODEL(StatusTelegramState,
            RUVIA_REQUIRED_FIELD(bot_username, ruvia::string),
            RUVIA_REQUIRED_FIELD(telegram_id, ruvia::string));
RUVIA_MODEL(AccountEvent,
            RUVIA_REQUIRED_FIELD(type, ruvia::string),
            RUVIA_REQUIRED_FIELD(telegram, StatusTelegramState),
            RUVIA_REQUIRED_FIELD(approvals, ruvia::array<StatusApprovalItem>));
RUVIA_MODEL(SnapshotEvent,
            RUVIA_REQUIRED_FIELD(type, ruvia::string),
            RUVIA_REQUIRED_FIELD(devices, ruvia::array<StatusDeviceItem>));
RUVIA_MODEL(PingEvent,
            RUVIA_REQUIRED_FIELD(type, ruvia::string));
RUVIA_MODEL(UpsertEvent,
            RUVIA_REQUIRED_FIELD(type, ruvia::string),
            RUVIA_REQUIRED_FIELD(device, StatusDeviceItem));
RUVIA_MODEL(RemoveEvent,
            RUVIA_REQUIRED_FIELD(type, ruvia::string),
            RUVIA_REQUIRED_FIELD(id, ruvia::string));

StatusDeviceItem deviceModel(const DeviceInfo& device, const DeviceOnlineState& state) {
    StatusDeviceItem item;
    if (state.online && !state.agentVersion.empty()) {
        item.set<"agent_version">(state.agentVersion);
    } else if (device.agentVersion && !device.agentVersion->empty()) {
        item.set<"agent_version">(*device.agentVersion);
    } else {
        item.set_null<"agent_version">();
    }
    item.set<"id">(device.id)
        .set<"name">(device.name)
        .set<"owner_username">(device.ownerUsername)
        .set<"online">(state.online)
        .set<"enrolled">(device.enrolled)
        .set<"can_manage">(device.canManage)
        .set<"desktop">(device.desktop)
        .set<"write_control">(device.writeControl)
        .set<"verification_method">(device.verificationMethod)
        .set<"manage_credentials">(device.manageCredentials);
    if (device.lastSeen) {
        item.set<"last_seen">(*device.lastSeen);
    } else {
        item.set_null<"last_seen">();
    }
    return item;
}
template <typename T>
ruvia::task<void> send(ruvia::context& context, const T& event) {
    const auto bytes = ruvia::to_json(event);
    if (bytes.size() > 1024 * 1024) {
        throw std::runtime_error("Device snapshot exceeds limit");
    }
    co_await context.websocket().text(std::string_view(bytes));
}
ruvia::task<void> sendAccount(ruvia::context& context, const UserIdentity& user) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto telegram = co_await runtime.telegram.state(context.db(), user.id);
    const auto approvals = co_await runtime.telegram.approvals(context.db(), user);
    AccountEvent event;
    event.set<"type">("account");
    auto& tg = event.ensure<"telegram">();
    tg.set<"bot_username">(telegram.botUsername).set<"telegram_id">(telegram.telegramId);
    auto& items = event.ensure<"approvals">();
    for (const auto& a : approvals) {
        StatusApprovalItem item;
        item.set<"id">(a.id).set<"device_id">(a.deviceId).set<"state">(a.state).set<"write_control">(a.writeControl).set<"expires_at">(a.expiresAt);
        items.push_back(std::move(item));
    }
    co_await send(context, event);
}
ruvia::task<void> writeStatus(ruvia::context& context, StatusSubscription& subscription, ruvia::task_scope& scope, bool& awaitingPong) {
    try {
        auto& runtime = context.worker_state<ServerRuntime>();
        const auto token = std::string(context.request().cookie("__Host-ultraterimal").value_or(""));
        const bool administration = context.request().query("scope").value_or("") == "administration";
        if (administration && context.request_state<UserIdentity>().role != "admin") {
            throw std::runtime_error("Administrator required");
        }
        const auto devices = co_await runtime.devices.list(context.db(), subscription.userId, administration);
        std::vector<std::string> deviceIds;
        deviceIds.reserve(devices.size());
        for (const auto& device : devices) {
            deviceIds.push_back(device.id);
        }
        const auto onlineStates = co_await queryDeviceOnlineStates(context.worker(), runtime, deviceIds);
        std::set<std::string> visible;
        SnapshotEvent snapshot;
        snapshot.set<"type">("snapshot");
        auto& list = snapshot.ensure<"devices">();
        for (const auto& device : devices) {
            visible.insert(device.id);
            const auto foundState = onlineStates.find(device.id);
            list.push_back(deviceModel(device, foundState != onlineStates.end() ? foundState->second : DeviceOnlineState{}));
        }
        co_await send(context, snapshot);
        co_await sendAccount(context, context.request_state<UserIdentity>());
        std::uint64_t heartbeatTicks = 0;
        while (!scope.stop_requested()) {
            const auto event = co_await subscription.receive(scope.token());
            if (!event || scope.stop_requested() || subscription.closed()) {
                break;
            }
            if (*event == "h") {
                if (++heartbeatTicks % 10 == 0) {
                    const auto user = co_await runtime.identities.authenticate(context.db(), token);
                    if (!user || user->id != subscription.userId || (administration && user->role != "admin") || subscription.closed()) {
                        break;
                    }
                }
                awaitingPong = true;
                PingEvent ping;
                ping.set<"type">("ping");
                co_await send(context, ping);
                continue;
            }
            const auto user = co_await runtime.identities.authenticate(context.db(), token);
            if (!user || user->id != subscription.userId || (administration && user->role != "admin") || subscription.closed()) {
                break;
            }
            if (event->starts_with("u:")) {
                co_await sendAccount(context, *user);
            } else if (event->starts_with("d:")) {
                const auto id = event->substr(2);
                const auto device = co_await runtime.devices.find(context.db(), subscription.userId, id, administration);
                if (scope.stop_requested() || subscription.closed()) {
                    break;
                }
                if (device) {
                    if (!visible.contains(id) && visible.size() >= 1000) {
                        break;
                    }
                    visible.insert(id);
                    const auto state = co_await queryDeviceOnlineState(context.worker(), runtime, id);
                    UpsertEvent upsert;
                    upsert.set<"type">("upsert").set<"device">(deviceModel(*device, state));
                    co_await send(context, upsert);
                } else if (visible.erase(id)) {
                    RemoveEvent remove;
                    remove.set<"type">("remove").set<"id">(id);
                    co_await send(context, remove);
                }
            }
        }
    } catch (const std::exception&) {
    }
    subscription.close();
    context.websocket().abort();
}
ruvia::task<void> watchStatus(ruvia::context& context, StatusSubscription& subscription, ruvia::task_scope& scope, bool& awaitingPong) {
    while (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(15), scope.token()) == ruvia::sleep_status::elapsed) {
        if (awaitingPong || !subscription.enqueue("h")) {
            subscription.close();
            context.websocket().abort();
            break;
        }
    }
}
} // namespace
ruvia::task<void> StatusController::events(ruvia::context& context) {
    auto subscription = context.worker_state<ServerRuntime>().status.subscribe(context.worker(), context.request_state<UserIdentity>().id);
    if (!subscription) {
        co_await context.websocket().close({.code_ = ruvia::websocket_close_code(1008), .reason_ = "Status subscription limit"});
        co_return;
    }
    {
        const auto scopeWorker = context.worker();
        ruvia::task_scope scope(scopeWorker);
        bool scopeJoined = false;
        std::exception_ptr scopeFailure;
        bool awaitingPong = false;
        try {
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
            scopeJoined = true;
            co_await scope.join();

        } catch (...) {
            scopeFailure = std::current_exception();
            subscription->close();
            context.websocket().abort();
        }
        if (!scopeJoined) {
            scope.request_stop();
            try {
                co_await scope.join();
            } catch (...) {
                if (!scopeFailure) {
                    scopeFailure = std::current_exception();
                }
            }
        }
        if (scopeFailure) {
            std::rethrow_exception(scopeFailure);
        }
    }
}
} // namespace ultraterimal::server
