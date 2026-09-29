#pragma once
#include <ruvia/web/Controller.h>
#include <ruvia/web/RateLimit.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class DeviceInstallerController final : public ruvia::Controller<DeviceInstallerController> {
  public:
    RUVIA_CONTROLLER_GROUP("")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/install.ps1", script);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> script(ruvia::Context& context);
};
class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/install.ps1", install, ruvia::RateLimit<30, 60000>);
    RUVIA_POST("/enrollments", enrollment, BrowserOrigin, UserAuthentication, ruvia::RateLimit<20, 60000>);
    RUVIA_GET("/devices", devices, UserAuthentication);
    RUVIA_POST("/devices/create", create, BrowserOrigin, UserAuthentication, ruvia::RateLimit<20, 60000>);
    RUVIA_POST("/devices/rename", rename, BrowserOrigin, UserAuthentication);
    RUVIA_POST("/devices/transfer-owner", transferOwner, BrowserOrigin, UserAuthentication, ruvia::RateLimit<10, 60000>);
    RUVIA_POST("/devices/delete", remove, BrowserOrigin, UserAuthentication);
    RUVIA_POST("/devices/update", update, BrowserOrigin, UserAuthentication, ruvia::RateLimit<5, 60000>);
    RUVIA_POST("/devices/windows-login", windowsLogin, BrowserOrigin, UserAuthentication, ruvia::RateLimit<20, 60000>);
    RUVIA_GET("/devices/restart-permission", restartPermissionStatus, UserAuthentication);
    RUVIA_POST("/devices/restart", restart, BrowserOrigin, UserAuthentication, ruvia::RateLimit<10, 60000>);
    RUVIA_GET("/devices/restart/:id", restartStatus, UserAuthentication);
    RUVIA_GET("/devices/:id/grants", grants, UserAuthentication);
    RUVIA_POST("/devices/:id/grants", saveGrant, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> grants(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> saveGrant(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> install(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> enrollment(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> rename(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> transferOwner(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> windowsLogin(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> restartPermissionStatus(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> restart(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> restartStatus(ruvia::Context& context);
};
} // namespace ultraterimal::server
