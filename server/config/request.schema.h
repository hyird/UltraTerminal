#pragma once
#include <initializer_list>
#include <string>
#include <string_view>

#include <boost/json.hpp>
#include <ruvia/web/Error.h>
namespace ultraterimal::server {
inline boost::json::object requestObject(std::string_view bytes, std::initializer_list<std::string_view> names) {
    boost::system::error_code error;
    auto value = boost::json::parse(bytes, error);
    if (error || !value.is_object()) {
        throw ruvia::HttpError({.status = ruvia::http_status::kBadRequest, .code = "invalid_json", .message = "Expected a JSON object"});
    }
    auto result = std::move(value.as_object());
    for (const auto& field : result) {
        bool allowed = false;
        for (const auto name : names) {
            if (field.key() == name) {
                allowed = true;
            }
        }
        if (!allowed) {
            throw ruvia::HttpError({.status = ruvia::http_status::kBadRequest, .code = "unknown_field", .message = "Unexpected request field"});
        }
    }
    return result;
}
inline std::string requestText(const boost::json::object& object, std::string_view name, std::size_t maximum) {
    const auto* value = object.if_contains(name);
    if (!value || !value->is_string() || value->as_string().empty() || value->as_string().size() > maximum || value->as_string().find('\0') != boost::json::string::npos) {
        throw ruvia::HttpError({.status = ruvia::http_status::kBadRequest, .code = "invalid_field", .message = "Invalid request field"});
    }
    return std::string(value->as_string());
}
inline std::string optionalText(const boost::json::object& object, std::string_view name, std::size_t maximum) {
    const auto* value = object.if_contains(name);
    if (!value || (value->is_string() && value->as_string().empty())) {
        return {};
    }
    return requestText(object, name, maximum);
}
inline bool requestBool(const boost::json::object& object, std::string_view name) {
    const auto* value = object.if_contains(name);
    if (!value || !value->is_bool()) {
        throw ruvia::HttpError({.status = ruvia::http_status::kBadRequest, .code = "invalid_field", .message = "Expected a boolean"});
    }
    return value->as_bool();
}
} // namespace ultraterimal::server
