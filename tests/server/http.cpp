#include "common/http.h"

#include <app.h>
#include <array>
#include <controller.h>
#include <iostream>
#include <model_binding.h>
#include <security_middleware.h>
#include <stdexcept>
#include <test_app.h>

namespace {
RUVIA_MODEL(CreatedResponse, RUVIA_REQUIRED_FIELD(created, bool));
RUVIA_MODEL(DiscardedResponse, RUVIA_REQUIRED_FIELD(discarded, bool));
RUVIA_MODEL(EchoRequest,
            RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_NOT_EMPTY("name required"), RUVIA_MAX_BYTES(16, "name too long"), RUVIA_CUSTOM(ultraterimal::server::noNul, "invalid character")));
RUVIA_MODEL(EchoResponse, RUVIA_REQUIRED_FIELD(name, ruvia::string));
RUVIA_MODEL(LookupQuery,
            RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_NOT_EMPTY("id required"), RUVIA_MAX_BYTES(16, "id too long"), RUVIA_CUSTOM(ultraterimal::server::noNul, "invalid character")));
class ResponseController final {
  public:
    RUVIA_CONTROLLER_GROUP("/response-contract", ruvia::csrf_middleware)
    RUVIA_ROUTES_BEGIN(ResponseController)
    RUVIA_GET("/created", created);
    RUVIA_GET("/replaced", replaced);
    RUVIA_GET("/lookup", lookup, ruvia::query_model<LookupQuery>);
    RUVIA_POST("/echo", echo, ruvia::strict_json_body<EchoRequest>);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> created(ruvia::context& context) {
        const std::array<ruvia::set_cookie_options, 1> cookies{{{.name_ = "contract", .value_ = "value", .attributes_ = {.path_ = "/", .same_site_ = ruvia::cookie_same_site::strict, .http_only_ = ruvia::cookie_attribute_policy::emit, .secure_ = ruvia::cookie_attribute_policy::emit}}}};
        CreatedResponse response;
        response.set<"created">(true);
        co_return ultraterimal::server::jsonResponse(context, response, {.status_ = ruvia::http_status::created, .cookies_ = cookies});
    }
    ruvia::task<ruvia::http_response> replaced(ruvia::context& context) {
        DiscardedResponse response;
        response.set<"discarded">(true);
        auto discarded = ultraterimal::server::jsonResponse(context, response);
        (void)discarded;
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "denied", .message_ = "Denied"});
    }
    ruvia::task<ruvia::http_response> lookup(ruvia::context& context, const LookupQuery& query) {
        EchoResponse response;
        response.set<"name">(query.get<"id">().view());
        co_return ultraterimal::server::jsonResponse(context, response);
    }
    ruvia::task<ruvia::http_response> echo(ruvia::context& context, const EchoRequest& body) {
        EchoResponse response;
        response.set<"name">(body.get<"name">().view());
        co_return ultraterimal::server::jsonResponse(context, response);
    }
};
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::string extractXsrfCookie(const ruvia::http_response_headers& headers) {
    for (const auto& header : headers) {
        if (header.name() == "Set-Cookie" && header.value().starts_with("XSRF-TOKEN=")) {
            const auto rest = header.value().substr(11);
            const auto end = rest.find(';');
            return std::string(end == std::string_view::npos ? rest : rest.substr(0, end));
        }
    }
    return {};
}
} // namespace

int main() {
    try {
        ruvia::test_app app;
        ruvia::security_headers_config securityHeaders{};
        securityHeaders.strict_transport_security_header_ = ruvia::default_security_header_policy::omit;
        securityHeaders.content_security_policy_ = "default-src 'self'; style-src 'self' 'unsafe-inline'";
        app.use<ruvia::security_headers_middleware>(securityHeaders);
        app.controllers<ResponseController>();
        const auto created = app.dispatch(ruvia::test_request::get("/response-contract/created"));
        require(created.status() == ruvia::http_status::created, "JSON builder lost the selected creation status");
        require(created.header("Content-Type") == "application/json", "JSON media type changed");
        require(created.header("Cache-Control") == "no-store", "JSON response became cacheable");
        require(created.header("X-Content-Type-Options") == "nosniff", "Security headers middleware missing nosniff");
        require(created.body() == "{\"created\":true}", "JSON body changed");
        const auto xsrf = extractXsrfCookie(created.headers());
        require(!xsrf.empty(), "CSRF middleware did not issue XSRF-TOKEN cookie");
        const auto replaced = app.dispatch(ruvia::test_request::get("/response-contract/replaced"));
        require(replaced.status() == ruvia::http_status::forbidden, "Replacement error inherited the previous status");
        require(replaced.header("Cache-Control") == "no-store", "Replacement error lost the cache policy");
        const auto missingCsrf = app.dispatch(ruvia::test_request::post("/response-contract/echo").header("Content-Type", "application/json").body("{\"name\":\"ok\"}"));
        require(missingCsrf.status() == ruvia::http_status::forbidden, "CSRF middleware did not block POST without token");
        const auto validPost = app.dispatch(ruvia::test_request::post("/response-contract/echo").header("Content-Type", "application/json").header("Cookie", "XSRF-TOKEN=" + xsrf).header("X-XSRF-TOKEN", xsrf).body("{\"name\":\"ok\"}"));
        require(validPost.status() == ruvia::http_status::ok && validPost.body() == "{\"name\":\"ok\"}", "Valid strict_json_body + CSRF POST failed");
        const auto unknownField = app.dispatch(ruvia::test_request::post("/response-contract/echo").header("Content-Type", "application/json").header("Cookie", "XSRF-TOKEN=" + xsrf).header("X-XSRF-TOKEN", xsrf).body("{\"name\":\"ok\",\"extra\":1}"));
        require(unknownField.status() == ruvia::http_status::bad_request, "strict_json_body did not reject unknown field");
        const auto validQuery = app.dispatch(ruvia::test_request::get("/response-contract/lookup?id=abc"));
        require(validQuery.status() == ruvia::http_status::ok && validQuery.body() == "{\"name\":\"abc\"}", "query_model failed");
        const auto missingQuery = app.dispatch(ruvia::test_request::get("/response-contract/lookup"));
        require(missingQuery.status() == ruvia::http_status::bad_request, "query_model did not reject missing required param");
        std::cout << "HTTP status, JSON, CSRF, security headers, strict_json_body and query_model contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
