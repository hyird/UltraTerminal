#pragma once
#include <context.h>
#include <http_client.h>
#include <model.h>
#include <optional>
#include <string>
#include <string_view>
#include <task.h>

namespace ultraterimal::server {
RUVIA_MODEL(TelegramApiResponse,
            RUVIA_REQUIRED_FIELD(ok, bool),
            RUVIA_OPTIONAL_FIELD(result, ruvia::json_value));
inline bool validBotToken(std::string_view token) {
    const auto colon = token.find(':');
    return colon >= 1 && colon <= 20 && token.size() >= colon + 21 && token.size() <= 160 && token.substr(0, colon).find_first_not_of("0123456789") == std::string_view::npos && token.substr(colon + 1).find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos;
}
template <typename T>
inline ruvia::task<std::optional<std::string>> telegramCall(ruvia::context& context, std::string token, std::string method, const T& body) {
    if (!validBotToken(token)) {
        co_return std::nullopt;
    }
    const auto target = "/bot" + token + "/" + method;
    try {
        auto response = co_await context.http_client("telegram").post(target).json(body).send();
        if (response.status() != ruvia::http_status::ok) {
            co_return std::nullopt;
        }
        auto parsedResult = co_await response.json<TelegramApiResponse>(65536);
        if (!parsedResult.has_value()) {
            co_return std::nullopt;
        }
        const auto parsed = std::move(parsedResult).take_value();
        const auto& resultField = parsed.get<"result">();
        if (!parsed.get<"ok">() || !resultField.has_value()) {
            co_return std::nullopt;
        }
        co_return std::string(resultField->view());
    } catch (const std::exception&) {
        // Upstream errors can contain a URL with the token; never expose them.
        co_return std::nullopt;
    }
}
} // namespace ultraterimal::server
