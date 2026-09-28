#pragma once
#include <boost/json.hpp>
#include <ruvia/web/Context.h>
namespace ultraterimal::server {
inline ruvia::HttpResponse jsonResponse(ruvia::Context& context, const boost::json::value& value) {
    context.header("Content-Type", "application/json; charset=utf-8");
    context.header("Cache-Control", "no-store");
    const auto bytes = boost::json::serialize(value);
    return context.body(std::pmr::string(bytes, context.arena()));
}
} // namespace ultraterimal::server
