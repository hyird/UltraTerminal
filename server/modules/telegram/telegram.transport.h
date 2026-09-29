#pragma once
#include <array>
#include <optional>
#include <string>

#include <boost/json.hpp>
#include <ruvia/web/Context.h>

namespace ultraterimal::server {
inline bool validBotToken(std::string_view token) {
    const auto colon = token.find(':');
    return colon >= 1 && colon <= 20 && token.size() >= colon + 21 && token.size() <= 160 && token.substr(0, colon).find_first_not_of("0123456789") == std::string_view::npos && token.substr(colon + 1).find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos;
}
inline ruvia::Task<std::optional<boost::json::value>> telegramCall(ruvia::Context& context, std::string token, std::string method, boost::json::object body) {
    if (!validBotToken(token)) {
        co_return std::nullopt;
    }
    const auto target = "/bot" + token + "/" + method;
    const auto bytes = boost::json::serialize(body);
    const std::array<ruvia::HttpHeaderView, 1> headers{{{"Content-Type", "application/json"}}};
    try {
        auto response = co_await context.httpClient("telegram").send({.method = "POST", .target = target, .headers = headers, .content = ruvia::HttpClientRequestContentView::bytes(bytes)});
        const auto result = co_await response.body().readAll(65536);
        if (response.status() != ruvia::http_status::kOk) {
            co_return std::nullopt;
        }
        boost::system::error_code error;
        const auto responseBytes = result.bytes();
        auto value = boost::json::parse(std::string_view(reinterpret_cast<const char*>(responseBytes.data()), responseBytes.size()), error);
        if (error || !value.is_object()) {
            co_return std::nullopt;
        }
        const auto* ok = value.as_object().if_contains("ok");
        const auto* data = value.as_object().if_contains("result");
        if (!ok || !ok->is_bool() || !ok->as_bool() || !data) {
            co_return std::nullopt;
        }
        co_return *data;
    } catch (const std::exception&) {
        // Upstream errors can contain a URL with the token; never expose them.
        co_return std::nullopt;
    }
}
} // namespace ultraterimal::server
