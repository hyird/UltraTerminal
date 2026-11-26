#include "telegram.controller.h"

#include "config/request.schema.h"
#include "middleware/json.transport.h"
#include "modules/identity/security.protocol.h"
#include "telegram.transport.h"

namespace ultraterimal::server {
namespace {
ruvia::http_response rejected(ruvia::context& context) {
    return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "telegram_rejected", .message_ = "Telegram operation rejected; check binding and permissions"});
}
const boost::json::object& objectAt(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (!value || !value->is_object()) {
        throw std::invalid_argument("Invalid Telegram update");
    }
    return value->as_object();
}
std::int64_t idAt(const boost::json::object& object) {
    const auto* id = object.if_contains("id");
    if (!id || !id->is_int64() || id->as_int64() <= 0 || id->as_int64() > 9007199254740991LL) {
        throw std::invalid_argument("Invalid Telegram identity");
    }
    return id->as_int64();
}
} // namespace
ruvia::task<ruvia::http_response> TelegramController::state(ruvia::context& context) {
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().telegram, user = context.request_state<UserIdentity>().id] { return service.state(user); })).take_value());
    co_return jsonResponse(context, {{"bot_username", result.botUsername}, {"telegram_id", result.telegramId}});
}
ruvia::task<ruvia::http_response> TelegramController::configure(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"token", "password", "code", "disable", "telegram_proof"});
    const auto user = context.request_state<UserIdentity>();
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool allowed = ((co_await context.run_blocking([service = runtime.identities, user, password = optionalText(body, "password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, password, code, telegramProof); })).take_value());
    if (!allowed || runtime.shared->telegramConfiguring->exchange(true, std::memory_order_acq_rel)) {
        co_return rejected(context);
    }
    struct Guard {
        std::atomic_bool& flag;
        ~Guard() { flag.store(false, std::memory_order_release); }
    } guard{*runtime.shared->telegramConfiguring};
    const bool disabling = requestBool(body, "disable");
    if (disabling) {
        const auto previous = ((co_await context.run_blocking([service = runtime.telegram] { return service.configuration(); })).take_value());
        if (previous) {
            const auto removed = co_await telegramCall(context, previous->token, "deleteWebhook", {{"drop_pending_updates", true}});
            if (!removed || !removed->is_bool() || !removed->as_bool()) {
                co_return rejected(context);
            }
        }
        const bool disabled = ((co_await context.run_blocking([service = runtime.telegram, user] { return service.disable(user); })).take_value());
        if (!disabled) {
            co_return rejected(context);
        }
        publishClusterStatus(runtime, "u:*");
        co_return jsonResponse(context, {{"ok", true}});
    }
    TelegramConfig config{requestText(body, "token", 160), {}, randomToken(), randomToken()};
    const auto me = co_await telegramCall(context, config.token, "getMe", {});
    if (!me || !me->is_object()) {
        co_return rejected(context);
    }
    const auto* username = me->as_object().if_contains("username");
    const auto* bot = me->as_object().if_contains("is_bot");
    if (!username || !username->is_string() || !bot || !bot->is_bool() || !bot->as_bool()) {
        co_return rejected(context);
    }
    config.username = std::string(username->as_string());
    if (config.username.empty() || config.username.size() > 64 || config.username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
        co_return rejected(context);
    }
    const auto installed = co_await telegramCall(context, config.token, "setWebhook", {{"url", runtime.config.origin + "/api/telegram/webhook"}, {"secret_token", config.webhookSecret}, {"allowed_updates", boost::json::array{"message", "callback_query"}}, {"drop_pending_updates", true}});
    if (!installed || !installed->is_bool() || !installed->as_bool()) {
        co_return rejected(context);
    }
    const bool saved = ((co_await context.run_blocking([service = runtime.telegram, user, config] { return service.configure(user, config); })).take_value());
    if (!saved) {
        co_return rejected(context);
    }
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::task<ruvia::http_response> TelegramController::bind(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"password", "code", "telegram_proof"});
    const auto user = context.request_state<UserIdentity>();
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto link = ((co_await context.run_blocking([identities = runtime.identities, service = runtime.telegram, user, password = optionalText(body, "password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)]() -> std::optional<std::string> { if(!identities.reauthenticate(user,password,code,telegramProof)) { return std::nullopt; } return service.beginBinding(user); })).take_value());
    if (!link) {
        co_return rejected(context);
    }
    co_return jsonResponse(context, {{"url", *link}, {"expires_in", 600}});
}
ruvia::task<ruvia::http_response> TelegramController::unbind(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"password", "code", "telegram_proof"});
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const bool ok = ((co_await context.run_blocking([identities = runtime.identities, service = runtime.telegram, user, password = optionalText(body, "password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { if(!identities.reauthenticate(user,password,code,telegramProof)) { return false; } service.unbind(user); return true; })).take_value());
    if (!ok) {
        co_return rejected(context);
    }
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::task<ruvia::http_response> TelegramController::verify(ruvia::context& context) {
    (void)requestObject(co_await context.request().body_text(), {});
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const auto config = ((co_await context.run_blocking([service = runtime.telegram] { return service.configuration(); })).take_value());
    if (!config) {
        co_return rejected(context);
    }
    const auto proof = ((co_await context.run_blocking([service = runtime.identities, user, generation = config->generation] { return service.requestTelegramVerification(user, generation); })).take_value());
    if (!proof) {
        co_return rejected(context);
    }
    const boost::json::array buttons{boost::json::array{boost::json::object{{"text", "确认本人操作"}, {"callback_data", "v:" + proof->id}}, boost::json::object{{"text", "拒绝"}, {"callback_data", "x:" + proof->id}}}};
    const auto sent = co_await telegramCall(context, config->token, "sendMessage", {{"chat_id", proof->telegramId}, {"text", "身份确认\n账号：" + user.username + "\n浏览器正在请求验证身份。仅在您本人正在操作时确认；确认仅限当前登录会话使用一次，5 分钟后失效。"}, {"reply_markup", boost::json::object{{"inline_keyboard", buttons}}}});
    if (!sent) {
        ((co_await context.run_blocking([service = runtime.identities, id = proof->id] { service.failTelegramVerification(id); })).take_value());
        co_return rejected(context);
    }
    co_return jsonResponse(context, {{"id", proof->id}});
}
ruvia::task<ruvia::http_response> TelegramController::webhook(ruvia::context& context) {
    const auto bytes = co_await context.request().body_text();
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto config = ((co_await context.run_blocking([service = runtime.telegram] { return service.configuration(); })).take_value());
    if (!config || !constantEqual(context.request().header("X-Telegram-Bot-Api-Secret-Token").value_or(""), config->webhookSecret)) {
        co_return rejected(context);
    }
    boost::system::error_code parseError;
    const auto value = boost::json::parse(bytes, parseError);
    if (parseError || !value.is_object()) {
        co_return rejected(context);
    }
    const auto& update = value.as_object();
    // Ignore unsupported or malformed events without retrying a poison update.
    try {
        if (update.contains("message")) {
            const auto& message = objectAt(update, "message");
            const auto& chat = objectAt(message, "chat");
            const auto& sender = objectAt(message, "from");
            if (requestBool(sender, "is_bot")) {
                co_return jsonResponse(context, {{"ok", true}});
            }
            const auto id = idAt(sender);
            if (requestText(chat, "type", 16) != "private" || idAt(chat) != id) {
                co_return jsonResponse(context, {{"ok", true}});
            }
            const auto text = optionalText(message, "text", 4096);
            if (!text.starts_with("/start ")) {
                co_return jsonResponse(context, {{"ok", true}});
            }
            const auto user = ((co_await context.run_blocking([service = runtime.telegram, generation = config->generation, token = text.substr(7), id] { return service.bind(generation, token, id); })).take_value());
            if (user) {
                publishClusterStatus(runtime, "u:" + *user);
            }
            co_return jsonResponse(context, {{"method", "sendMessage"}, {"chat_id", id}, {"text", user ? "Telegram 已绑定到平台账号。" : "绑定未成功：链接已过期、已使用或 Telegram 已绑定其他账号，请回平台重新操作。"}});
        }
        if (update.contains("callback_query")) {
            const auto& callback = objectAt(update, "callback_query");
            const auto& sender = objectAt(callback, "from");
            if (requestBool(sender, "is_bot")) {
                co_return jsonResponse(context, {{"ok", true}});
            }
            const auto id = idAt(sender);
            const auto& chat = objectAt(objectAt(callback, "message"), "chat");
            if (requestText(chat, "type", 16) != "private" || idAt(chat) != id) {
                co_return jsonResponse(context, {{"ok", true}});
            }
            const auto data = requestText(callback, "data", 64);
            if (data.size() != 34 || data[1] != ':' || (data[0] != 'y' && data[0] != 'n' && data[0] != 'v' && data[0] != 'x') || data.substr(2).find_first_not_of("0123456789abcdef") != std::string::npos) {
                co_return jsonResponse(context, {{"ok", true}});
            }
            if (data[0] == 'v' || data[0] == 'x') {
                const auto verified = ((co_await context.run_blocking([service = runtime.identities, generation = config->generation, proof = data.substr(2), id, accept = data[0] == 'v'] { return service.decideTelegramVerification(generation, proof, id, accept); })).take_value());
                co_return jsonResponse(context, {{"method", "answerCallbackQuery"}, {"callback_query_id", requestText(callback, "id", 256)}, {"text", verified ? (data[0] == 'v' ? "已确认，请返回浏览器继续" : "已拒绝身份确认") : "验证已失效或已处理"}});
            }
            const auto user = ((co_await context.run_blocking([service = runtime.telegram, generation = config->generation, approval = data.substr(2), id, accept = data[0] == 'y'] { return service.decide(generation, approval, id, accept); })).take_value());
            if (user) {
                publishClusterStatus(runtime, "u:" + *user);
            }
            co_return jsonResponse(context, {{"method", "answerCallbackQuery"}, {"callback_query_id", requestText(callback, "id", 256)}, {"text", user ? (data[0] == 'y' ? "已允许本次连接" : "已拒绝连接") : "申请已失效或已处理"}});
        }
    } catch (const std::invalid_argument&) {
    } catch (const ruvia::http_error&) {
    }
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::task<ruvia::http_response> TelegramController::request(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id", "write_control"});
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const auto dispatch = ((co_await context.run_blocking([service = runtime.telegram, user, device = requestText(body, "device_id", 128), write = requestBool(body, "write_control")] { return service.requestApproval(user, device, write); })).take_value());
    if (!dispatch) {
        co_return rejected(context);
    }
    if (!dispatch->botToken.empty()) {
        const auto text = "连接二级验证\n账号：" + dispatch->username + "\n设备：" + dispatch->deviceName + "\n权限：" + std::string("控制") + "\n请确认这是您本人发起的连接。验证在 5 分钟后失效。";
        const boost::json::array buttons{boost::json::array{boost::json::object{{"text", "确认本人连接"}, {"callback_data", "y:" + dispatch->approval.id}}, boost::json::object{{"text", "拒绝"}, {"callback_data", "n:" + dispatch->approval.id}}}};
        const auto sent = co_await telegramCall(context, dispatch->botToken, "sendMessage", {{"chat_id", dispatch->requesterTelegramId}, {"text", text}, {"reply_markup", boost::json::object{{"inline_keyboard", buttons}}}});
        if (!sent) {
            ((co_await context.run_blocking([service = runtime.telegram, id = dispatch->approval.id] { service.deliveryFailed(id); })).take_value());
            publishClusterStatus(runtime, "u:" + user.id);
            co_return rejected(context);
        }
    }
    publishClusterStatus(runtime, "u:" + user.id);
    co_return jsonResponse(context, {{"id", dispatch->approval.id}, {"expires_at", dispatch->approval.expiresAt}, {"write_control", dispatch->approval.writeControl}});
}
ruvia::task<ruvia::http_response> TelegramController::approvals(ruvia::context& context) {
    const auto approvals = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().telegram, user = context.request_state<UserIdentity>()] { return service.approvals(user); })).take_value());
    boost::json::array result;
    for (const auto& a : approvals) {
        result.push_back(boost::json::object{{"id", a.id}, {"device_id", a.deviceId}, {"state", a.state}, {"write_control", a.writeControl}, {"expires_at", a.expiresAt}});
    }
    co_return jsonResponse(context, result);
}
ruvia::task<ruvia::http_response> TelegramController::cancel(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"id"});
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    ((co_await context.run_blocking([service = runtime.telegram, user, id = requestText(body, "id", 32)] { service.cancel(user, id); })).take_value());
    publishClusterStatus(runtime, "u:" + user.id);
    co_return jsonResponse(context, {{"ok", true}});
}
} // namespace ultraterimal::server
