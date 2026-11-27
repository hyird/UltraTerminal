#include "telegram.controller.h"

#include "modules/identity/security.protocol.h"
#include "telegram.transport.h"

namespace ultraterimal::server {
namespace {
ruvia::http_response rejected(ruvia::context& context) {
    return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "telegram_rejected", .message_ = "Telegram operation rejected; check binding and permissions"});
}
TelegramSendMessageRequest buildInlinePrompt(std::int64_t chatId, std::string_view text, std::string_view acceptText, std::string_view acceptData, std::string_view rejectText, std::string_view rejectData) {
    TelegramSendMessageRequest request;
    request.set<"chat_id">(chatId).set<"text">(text);
    auto& keyboard = request.ensure<"reply_markup">().ensure<"inline_keyboard">();
    ruvia::array<TelegramInlineButton> row;
    TelegramInlineButton accept;
    accept.set<"text">(acceptText).set<"callback_data">(acceptData);
    TelegramInlineButton reject;
    reject.set<"text">(rejectText).set<"callback_data">(rejectData);
    row.push_back(std::move(accept));
    row.push_back(std::move(reject));
    keyboard.push_back(std::move(row));
    return request;
}
} // namespace
ruvia::task<ruvia::http_response> TelegramController::state(ruvia::context& context) {
    const auto result = co_await context.worker_state<ServerRuntime>().telegram.state(context.db(), context.request_state<UserIdentity>().id);
    TelegramStateResponse response;
    response.set<"bot_username">(result.botUsername).set<"telegram_id">(result.telegramId);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> TelegramController::configure(ruvia::context& context, const TelegramConfigureRequest& body) {
    const auto user = context.request_state<UserIdentity>();
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto db = context.db();
    const bool allowed = user.role == "admin" && (co_await runtime.identities.reauthenticate(db, user, optionalTextValue(body.get<"password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"telegram_proof">()), &context));
    if (!allowed || runtime.shared->telegramConfiguring->exchange(true, std::memory_order_acq_rel)) {
        co_return rejected(context);
    }
    struct Guard {
        std::atomic_bool& flag;
        ~Guard() { flag.store(false, std::memory_order_release); }
    } guard{*runtime.shared->telegramConfiguring};
    if (body.get<"disable">()) {
        const auto previous = co_await runtime.telegram.configuration(db);
        if (previous) {
            TelegramDeleteWebhookRequest req;
            req.set<"drop_pending_updates">(true);
            const auto removed = co_await telegramCall(context, previous->token, "deleteWebhook", req);
            if (!removed || *removed != "true") {
                co_return rejected(context);
            }
        }
        const bool disabled = co_await runtime.telegram.disable(db, user);
        if (!disabled) {
            co_return rejected(context);
        }
        publishClusterStatus(runtime, "u:*");
        co_return jsonResponse(context, okResponse());
    }
    const auto token = optionalTextValue(body.get<"token">());
    if (token.empty()) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Invalid request field"});
    }
    TelegramConfig config{token, {}, randomToken(), randomToken()};
    const auto me = co_await telegramCall(context, config.token, "getMe", ruvia::json_object{});
    if (!me) {
        co_return rejected(context);
    }
    auto parsedMe = ruvia::parse_json<TelegramBotUser>(*me);
    if (!parsedMe.has_value()) {
        co_return rejected(context);
    }
    const auto botUser = std::move(parsedMe).take_value();
    if (!botUser.get<"is_bot">()) {
        co_return rejected(context);
    }
    config.username = std::string(botUser.get<"username">().view());
    if (config.username.empty() || config.username.size() > 64 || config.username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
        co_return rejected(context);
    }
    TelegramSetWebhookRequest webhookReq;
    webhookReq.set<"url">(runtime.config.origin + "/api/telegram/webhook")
        .set<"secret_token">(config.webhookSecret)
        .set<"drop_pending_updates">(true);
    auto& updates = webhookReq.ensure<"allowed_updates">();
    updates.push_back(ruvia::string("message"));
    updates.push_back(ruvia::string("callback_query"));
    const auto installed = co_await telegramCall(context, config.token, "setWebhook", webhookReq);
    if (!installed || *installed != "true") {
        co_return rejected(context);
    }
    const bool saved = co_await runtime.telegram.configure(db, user, config);
    if (!saved) {
        co_return rejected(context);
    }
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, okResponse());
}
ruvia::task<ruvia::http_response> TelegramController::bind(ruvia::context& context, const TelegramReauthRequest& body) {
    const auto user = context.request_state<UserIdentity>();
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto db = context.db();
    if (!(co_await runtime.identities.reauthenticate(db, user, optionalTextValue(body.get<"password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"telegram_proof">()), &context))) {
        co_return rejected(context);
    }
    const auto link = co_await runtime.telegram.beginBinding(db, user);
    if (!link) {
        co_return rejected(context);
    }
    TelegramBindResponse response;
    response.set<"url">(*link).set<"expires_in">(600);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> TelegramController::unbind(ruvia::context& context, const TelegramReauthRequest& body) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const auto db = context.db();
    if (!(co_await runtime.identities.reauthenticate(db, user, optionalTextValue(body.get<"password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"telegram_proof">()), &context))) {
        co_return rejected(context);
    }
    co_await runtime.telegram.unbind(db, user);
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, okResponse());
}
ruvia::task<ruvia::http_response> TelegramController::verify(ruvia::context& context) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const auto db = context.db();
    const auto config = co_await runtime.telegram.configuration(db);
    if (!config) {
        co_return rejected(context);
    }
    const auto proof = co_await runtime.identities.requestTelegramVerification(db, user, config->generation);
    if (!proof) {
        co_return rejected(context);
    }
    const auto prompt = buildInlinePrompt(proof->telegramId, "身份确认\n账号：" + user.username + "\n浏览器正在请求验证身份。仅在您本人正在操作时确认；确认仅限当前登录会话使用一次，5 分钟后失效。", "确认本人操作", "v:" + proof->id, "拒绝", "x:" + proof->id);
    const auto sent = co_await telegramCall(context, config->token, "sendMessage", prompt);
    if (!sent) {
        co_await runtime.identities.failTelegramVerification(db, proof->id);
        co_return rejected(context);
    }
    TelegramVerifyResponse response;
    response.set<"id">(proof->id);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> TelegramController::webhook(ruvia::context& context) {
    const auto bytes = co_await context.request().body_text();
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto db = context.db();
    const auto config = co_await runtime.telegram.configuration(db);
    if (!config || !constantEqual(context.request().header("X-Telegram-Bot-Api-Secret-Token").value_or(""), config->webhookSecret)) {
        co_return rejected(context);
    }
    auto parsedUpdate = ruvia::parse_json<TelegramWebhookUpdate>(bytes);
    if (!parsedUpdate.has_value()) {
        // Ignore unsupported or malformed events without retrying a poison update.
        co_return jsonResponse(context, okResponse());
    }
    const auto update = std::move(parsedUpdate).take_value();
    if (const auto& message = update.get<"message">()) {
        const auto& sender = message->get<"from">();
        if (!sender || sender->get<"is_bot">()) {
            co_return jsonResponse(context, okResponse());
        }
        const auto id = sender->get<"id">();
        const auto& chat = message->get<"chat">();
        if (chat.get<"type">().view() != "private" || chat.get<"id">() != id) {
            co_return jsonResponse(context, okResponse());
        }
        const auto text = optionalTextValue(message->get<"text">());
        if (!text.starts_with("/start ")) {
            co_return jsonResponse(context, okResponse());
        }
        const auto user = co_await runtime.telegram.bind(db, config->generation, text.substr(7), id);
        if (user) {
            publishClusterStatus(runtime, "u:" + *user);
        }
        TelegramWebhookSendMessage reply;
        reply.set<"method">("sendMessage").set<"chat_id">(id).set<"text">(user ? "Telegram 已绑定到平台账号。" : "绑定未成功：链接已过期、已使用或 Telegram 已绑定其他账号，请回平台重新操作。");
        co_return jsonResponse(context, reply);
    }
    if (const auto& callback = update.get<"callback_query">()) {
        const auto& sender = callback->get<"from">();
        if (sender.get<"is_bot">()) {
            co_return jsonResponse(context, okResponse());
        }
        const auto id = sender.get<"id">();
        const auto& chat = callback->get<"message">().get<"chat">();
        if (chat.get<"type">().view() != "private" || chat.get<"id">() != id) {
            co_return jsonResponse(context, okResponse());
        }
        const auto data = textValue(callback->get<"data">());
        if (data.size() != 34 || data[1] != ':' || (data[0] != 'y' && data[0] != 'n' && data[0] != 'v' && data[0] != 'x') || data.substr(2).find_first_not_of("0123456789abcdef") != std::string::npos) {
            co_return jsonResponse(context, okResponse());
        }
        if (data[0] == 'v' || data[0] == 'x') {
            const auto verified = co_await runtime.identities.decideTelegramVerification(db, config->generation, data.substr(2), id, data[0] == 'v');
            TelegramWebhookAnswerCallback reply;
            reply.set<"method">("answerCallbackQuery").set<"callback_query_id">(callback->get<"id">().view()).set<"text">(verified ? (data[0] == 'v' ? "已确认，请返回浏览器继续" : "已拒绝身份确认") : "验证已失效或已处理");
            co_return jsonResponse(context, reply);
        }
        const auto user = co_await runtime.telegram.decide(db, config->generation, data.substr(2), id, data[0] == 'y');
        if (user) {
            publishClusterStatus(runtime, "u:" + *user);
        }
        TelegramWebhookAnswerCallback reply;
        reply.set<"method">("answerCallbackQuery").set<"callback_query_id">(callback->get<"id">().view()).set<"text">(user ? (data[0] == 'y' ? "已允许本次连接" : "已拒绝连接") : "申请已失效或已处理");
        co_return jsonResponse(context, reply);
    }
    co_return jsonResponse(context, okResponse());
}
ruvia::task<ruvia::http_response> TelegramController::request(ruvia::context& context, const TelegramApprovalRequest& body) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const auto db = context.db();
    const auto dispatch = co_await runtime.telegram.requestApproval(db, user, textValue(body.get<"device_id">()), body.get<"write_control">());
    if (!dispatch) {
        co_return rejected(context);
    }
    if (!dispatch->botToken.empty()) {
        const auto text = "连接二级验证\n账号：" + dispatch->username + "\n设备：" + dispatch->deviceName + "\n权限：" + std::string("控制") + "\n请确认这是您本人发起的连接。验证在 5 分钟后失效。";
        const auto prompt = buildInlinePrompt(dispatch->requesterTelegramId, text, "确认本人连接", "y:" + dispatch->approval.id, "拒绝", "n:" + dispatch->approval.id);
        const auto sent = co_await telegramCall(context, dispatch->botToken, "sendMessage", prompt);
        if (!sent) {
            co_await runtime.telegram.deliveryFailed(db, dispatch->approval.id);
            publishClusterStatus(runtime, "u:" + user.id);
            co_return rejected(context);
        }
    }
    publishClusterStatus(runtime, "u:" + user.id);
    TelegramApprovalCreatedResponse response;
    response.set<"id">(dispatch->approval.id).set<"expires_at">(dispatch->approval.expiresAt).set<"write_control">(dispatch->approval.writeControl);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> TelegramController::approvals(ruvia::context& context) {
    const auto approvals = co_await context.worker_state<ServerRuntime>().telegram.approvals(context.db(), context.request_state<UserIdentity>());
    ruvia::array<TelegramApprovalItem> result;
    for (const auto& a : approvals) {
        TelegramApprovalItem item;
        item.set<"id">(a.id).set<"device_id">(a.deviceId).set<"state">(a.state).set<"write_control">(a.writeControl).set<"expires_at">(a.expiresAt);
        result.push_back(std::move(item));
    }
    co_return jsonResponse(context, result);
}
ruvia::task<ruvia::http_response> TelegramController::cancel(ruvia::context& context, const TelegramCancelRequest& body) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    co_await runtime.telegram.cancel(context.db(), user, textValue(body.get<"id">()));
    publishClusterStatus(runtime, "u:" + user.id);
    co_return jsonResponse(context, okResponse());
}
} // namespace ultraterimal::server
