#pragma once
#include <boost/json.hpp>
#include <ruvia/web/context.h>
namespace ultraterimal::server {
inline ruvia::http_response jsonResponse(ruvia::context& context, const boost::json::value& value) {
    context.draft().header("Content-Type", "application/json; charset=utf-8");
    context.policy().header("Cache-Control", "no-store");
    const auto bytes = boost::json::serialize(value);
    return context.body(std::pmr::string(bytes, context.arena()));
}
} // namespace ultraterimal::server
