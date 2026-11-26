#include <app.h>
#include <array>
#include <controller.h>
#include <iostream>
#include <stdexcept>
#include <test_app.h>

#include "middleware/json.transport.h"

namespace {
class ResponseController final {
  public:
    RUVIA_CONTROLLER_GROUP("/response-contract")
    RUVIA_ROUTES_BEGIN(ResponseController)
    RUVIA_GET("/created", created);
    RUVIA_GET("/replaced", replaced);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> created(ruvia::context& context) {
        const std::array<ruvia::set_cookie_options, 1> cookies{{{.name_ = "contract", .value_ = "value", .attributes_ = {.path_ = "/", .same_site_ = ruvia::cookie_same_site::strict, .http_only_ = ruvia::cookie_attribute_policy::emit, .secure_ = ruvia::cookie_attribute_policy::emit}}}};
        co_return ultraterimal::server::jsonResponse(context, {{"created", true}}, {.status_ = ruvia::http_status::created, .cookies_ = cookies});
    }
    ruvia::task<ruvia::http_response> replaced(ruvia::context& context) {
        auto discarded = ultraterimal::server::jsonResponse(context, {{"discarded", true}});
        (void)discarded;
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "denied", .message_ = "Denied"});
    }
};
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    try {
        ruvia::test_app app;
        app.controllers<ResponseController>();
        const auto created = app.dispatch(ruvia::test_request::get("/response-contract/created"));
        require(created.status() == ruvia::http_status::created, "JSON builder lost the selected creation status");
        require(created.header("Content-Type") == "application/json", "JSON media type changed");
        require(created.header("Cache-Control") == "no-store", "JSON response became cacheable");
        require(created.header("Set-Cookie") == "contract=value; Path=/; HttpOnly; Secure; SameSite=Strict", "Cookie attributes changed");
        require(created.body() == "{\"created\":true}", "JSON body changed");
        const auto replaced = app.dispatch(ruvia::test_request::get("/response-contract/replaced"));
        require(replaced.status() == ruvia::http_status::forbidden, "Replacement error inherited the previous status");
        require(replaced.header("Cache-Control") == "no-store", "Replacement error lost the cache policy");
        require(!replaced.header("Set-Cookie"), "Response draft leaked between requests");
        std::cout << "HTTP status, JSON, cookie and replacement cache contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
