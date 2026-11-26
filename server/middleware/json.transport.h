#pragma once
#include <context.h>

#include <boost/json.hpp>
namespace ultraterimal::server {
inline ruvia::http_response jsonResponse(ruvia::context& context, const boost::json::value& value, ruvia::response_options options = {}) {
    context.policy().header("Cache-Control", "no-store");
    auto response = context.body(boost::json::serialize(value), options);
    response.header("Content-Type", "application/json; charset=utf-8");
    return response;
}
} // namespace ultraterimal::server
