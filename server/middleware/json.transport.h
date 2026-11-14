#pragma once
#include <boost/json.hpp>
#include <ruvia/web/context.h>
namespace ultraterimal::server {
inline ruvia::http_response jsonResponse(ruvia::context& context, const boost::json::value& value) {
    context.draft().header("Content-Type", "application/json; charset=utf-8");
    context.policy().header("Cache-Control", "no-store");
    return context.body(boost::json::serialize(value));
}
} // namespace ultraterimal::server
