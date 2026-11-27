#pragma once
#include <context.h>
#include <model.h>
#include <optional>
#include <string>
#include <string_view>

namespace ultraterimal::server {
inline constexpr bool noNul(const ruvia::string& value) noexcept {
    return value.view().find('\0') == std::string_view::npos;
}
inline std::string textValue(const ruvia::string& value) {
    return std::string(value.view());
}
inline std::string optionalTextValue(const std::optional<ruvia::string>& value) {
    return value ? std::string(value->view()) : std::string{};
}
RUVIA_MODEL(EmptyRequest);
RUVIA_MODEL(OkResponse, RUVIA_REQUIRED_FIELD(ok, bool));
inline OkResponse okResponse(bool value = true) {
    OkResponse response;
    response.set<"ok">(value);
    return response;
}
template <typename T>
inline ruvia::http_response jsonResponse(ruvia::context& context, const T& value, ruvia::response_options options = {}) {
    context.policy().header("Cache-Control", "no-store");
    return context.json(value, options);
}
} // namespace ultraterimal::server
