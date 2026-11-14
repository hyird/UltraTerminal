#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/middleware.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class DeviceInstallerController final {
  public:
    RUVIA_CONTROLLER_GROUP("")
    RUVIA_ROUTES_BEGIN(DeviceInstallerController)
    RUVIA_GET("/install.ps1", script);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> script(ruvia::context& context);
};
class DeviceController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN(DeviceController)
    RUVIA_GET("/install.ps1", install, ruvia::rate_limit<30, 60000>);
    RUVIA_POST("/enrollments", enrollment, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>);
    RUVIA_GET("/devices", devices, UserAuthentication);
    RUVIA_POST("/devices/create", create, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>);
    RUVIA_POST("/devices/rename", rename, BrowserOrigin, UserAuthentication);
    RUVIA_POST("/devices/transfer-owner", transferOwner, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>);
    RUVIA_POST("/devices/delete", remove, BrowserOrigin, UserAuthentication);
    RUVIA_POST("/devices/update", update, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>);
    RUVIA_POST("/devices/windows-login", windowsLogin, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>);
    RUVIA_GET("/devices/restart-permission", restartPermissionStatus, UserAuthentication);
    RUVIA_POST("/devices/restart", restart, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>);
    RUVIA_GET("/devices/restart/:id", restartStatus, UserAuthentication);
    RUVIA_GET("/devices/:id/grants", grants, UserAuthentication);
    RUVIA_POST("/devices/:id/grants", saveGrant, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> grants(ruvia::context& context);
    ruvia::task<ruvia::http_response> saveGrant(ruvia::context& context);
    ruvia::task<ruvia::http_response> install(ruvia::context& context);
    ruvia::task<ruvia::http_response> enrollment(ruvia::context& context);
    ruvia::task<ruvia::http_response> devices(ruvia::context& context);
    ruvia::task<ruvia::http_response> create(ruvia::context& context);
    ruvia::task<ruvia::http_response> rename(ruvia::context& context);
    ruvia::task<ruvia::http_response> transferOwner(ruvia::context& context);
    ruvia::task<ruvia::http_response> remove(ruvia::context& context);
    ruvia::task<ruvia::http_response> update(ruvia::context& context);
    ruvia::task<ruvia::http_response> windowsLogin(ruvia::context& context);
    ruvia::task<ruvia::http_response> restartPermissionStatus(ruvia::context& context);
    ruvia::task<ruvia::http_response> restart(ruvia::context& context);
    ruvia::task<ruvia::http_response> restartStatus(ruvia::context& context);
};
} // namespace ultraterimal::server
