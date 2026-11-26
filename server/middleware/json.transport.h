#pragma once
#include <context.h>

#include <boost/json.hpp>
namespace ultraterimal::server {
inline ruvia::http_response jsonResponse(ruvia::context& context, const boost::json::value& value, ruvia::response_options options = {}) {
    context.policy().header("Cache-Control", "no-store");
    return context.json_raw(boost::json::serialize(value), options);
}
} // namespace ultraterimal::server
