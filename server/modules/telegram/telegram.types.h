#pragma once
#include <cstdint>
#include <model.h>
#include <string>

#include "common/http.h"

namespace ultraterimal::server {
struct TelegramState {
    std::string botUsername;
    std::string telegramId;
};
struct TelegramConfig {
    std::string token, username, webhookSecret, generation;
};
struct ApprovalInfo {
    std::string id, deviceId, state;
    bool writeControl = false;
    std::int64_t expiresAt = 0;
};
struct ApprovalDispatch {
    ApprovalInfo approval;
    std::string botToken, username, deviceName;
    std::int64_t requesterTelegramId = 0;
};

RUVIA_MODEL(TelegramConfigureRequest,
            RUVIA_OPTIONAL_FIELD(token, ruvia::string, RUVIA_MAX_BYTES(160, "token too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(password, ruvia::string, RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(disable, bool),
            RUVIA_OPTIONAL_FIELD(telegram_proof, ruvia::string, RUVIA_MAX_BYTES(32, "telegram_proof too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(TelegramReauthRequest,
            RUVIA_OPTIONAL_FIELD(password, ruvia::string, RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(telegram_proof, ruvia::string, RUVIA_MAX_BYTES(32, "telegram_proof too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(TelegramApprovalRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(write_control, bool));
RUVIA_MODEL(TelegramCancelRequest,
            RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_NOT_EMPTY("id required"), RUVIA_MAX_BYTES(32, "id too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(TelegramStateResponse,
            RUVIA_REQUIRED_FIELD(bot_username, ruvia::string),
            RUVIA_REQUIRED_FIELD(telegram_id, ruvia::string));
RUVIA_MODEL(TelegramDeleteWebhookRequest,
            RUVIA_REQUIRED_FIELD(drop_pending_updates, bool));
RUVIA_MODEL(TelegramSetWebhookRequest,
            RUVIA_REQUIRED_FIELD(url, ruvia::string),
            RUVIA_REQUIRED_FIELD(secret_token, ruvia::string),
            RUVIA_REQUIRED_FIELD(allowed_updates, ruvia::array<ruvia::string>),
            RUVIA_REQUIRED_FIELD(drop_pending_updates, bool));
RUVIA_MODEL(TelegramBindResponse,
            RUVIA_REQUIRED_FIELD(url, ruvia::string),
            RUVIA_REQUIRED_FIELD(expires_in, std::int32_t));
RUVIA_MODEL(TelegramVerifyResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string));
RUVIA_MODEL(TelegramInlineButton,
            RUVIA_REQUIRED_FIELD(text, ruvia::string),
            RUVIA_REQUIRED_FIELD(callback_data, ruvia::string));
RUVIA_MODEL(TelegramReplyMarkup,
            RUVIA_REQUIRED_FIELD(inline_keyboard, ruvia::array<ruvia::array<TelegramInlineButton>>));
RUVIA_MODEL(TelegramSendMessageRequest,
            RUVIA_REQUIRED_FIELD(chat_id, std::int64_t),
            RUVIA_REQUIRED_FIELD(text, ruvia::string),
            RUVIA_REQUIRED_FIELD(reply_markup, TelegramReplyMarkup));
RUVIA_MODEL(TelegramWebhookSendMessage,
            RUVIA_REQUIRED_FIELD(method, ruvia::string),
            RUVIA_REQUIRED_FIELD(chat_id, std::int64_t),
            RUVIA_REQUIRED_FIELD(text, ruvia::string));
RUVIA_MODEL(TelegramWebhookAnswerCallback,
            RUVIA_REQUIRED_FIELD(method, ruvia::string),
            RUVIA_REQUIRED_FIELD(callback_query_id, ruvia::string),
            RUVIA_REQUIRED_FIELD(text, ruvia::string));
RUVIA_MODEL(TelegramApprovalCreatedResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(expires_at, std::int64_t),
            RUVIA_REQUIRED_FIELD(write_control, bool));
RUVIA_MODEL(TelegramBotUser,
            RUVIA_REQUIRED_FIELD(is_bot, bool),
            RUVIA_REQUIRED_FIELD(username, ruvia::string));
RUVIA_MODEL(TelegramApprovalItem,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string),
            RUVIA_REQUIRED_FIELD(state, ruvia::string),
            RUVIA_REQUIRED_FIELD(write_control, bool),
            RUVIA_REQUIRED_FIELD(expires_at, std::int64_t));
RUVIA_MODEL(TelegramWebhookSender,
            RUVIA_REQUIRED_FIELD(id, std::int64_t, RUVIA_MIN(1, "invalid id"), RUVIA_MAX(9007199254740991LL, "invalid id")),
            RUVIA_REQUIRED_FIELD(is_bot, bool));
RUVIA_MODEL(TelegramWebhookChat,
            RUVIA_REQUIRED_FIELD(id, std::int64_t, RUVIA_MIN(1, "invalid id"), RUVIA_MAX(9007199254740991LL, "invalid id")),
            RUVIA_REQUIRED_FIELD(type, ruvia::string, RUVIA_NOT_EMPTY("type required"), RUVIA_MAX_BYTES(16, "type too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(TelegramWebhookMessage,
            RUVIA_REQUIRED_FIELD(chat, TelegramWebhookChat),
            RUVIA_OPTIONAL_FIELD(from, TelegramWebhookSender),
            RUVIA_OPTIONAL_FIELD(text, ruvia::string, RUVIA_MAX_BYTES(4096, "text too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(TelegramWebhookCallbackQuery,
            RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_NOT_EMPTY("id required"), RUVIA_MAX_BYTES(256, "id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(from, TelegramWebhookSender),
            RUVIA_REQUIRED_FIELD(message, TelegramWebhookMessage),
            RUVIA_REQUIRED_FIELD(data, ruvia::string, RUVIA_NOT_EMPTY("data required"), RUVIA_MAX_BYTES(64, "data too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(TelegramWebhookUpdate,
            RUVIA_OPTIONAL_FIELD(message, TelegramWebhookMessage),
            RUVIA_OPTIONAL_FIELD(callback_query, TelegramWebhookCallbackQuery));
} // namespace ultraterimal::server
