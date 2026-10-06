#pragma once
#include <initializer_list>
#include <string>
#include <string_view>

#include <boost/json.hpp>
#include <ruvia/web/error.h>
namespace ultraterimal::server {
inline boost::json::object requestObject(std::string_view bytes, std::initializer_list<std::string_view> names) {
    boost::system::error_code error;
    auto value = boost::json::parse(bytes, error);
    if (error || !value.is_object()) {
        throw ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_json", .message_ = "Expected a JSON object"});
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
            throw ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "unknown_field", .message_ = "Unexpected request field"});
        }
    }
    return result;
}
inline std::string requestText(const boost::json::object& object, std::string_view name, std::size_t maximum) {
    const auto* value = object.if_contains(name);
    if (!value || !value->is_string() || value->as_string().empty() || value->as_string().size() > maximum || value->as_string().find('\0') != boost::json::string::npos) {
        throw ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Invalid request field"});
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
        throw ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Expected a boolean"});
    }
    return value->as_bool();
}
} // namespace ultraterimal::server
