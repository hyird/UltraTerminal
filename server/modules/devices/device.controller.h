#pragma once
#include <controller.h>
#include <middleware.h>
#include <model_binding.h>
#include <security_middleware.h>
#include <task.h>

#include "common/http.h"
#include "device.types.h"
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
    RUVIA_POST("/install/manifest", installManifest, ruvia::rate_limit<20, 60000>, ruvia::strict_json_body<InstallManifestRequest>);
    RUVIA_POST("/install/package", installPackage, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<InstallPackageRequest>);
    RUVIA_POST("/enrollments", enrollment, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>, ruvia::strict_json_body<DeviceEnrollmentRequest>);
    RUVIA_GET("/devices", devices, UserAuthentication);
    RUVIA_POST("/devices/create", create, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>, ruvia::strict_json_body<DeviceCreateRequest>);
    RUVIA_POST("/devices/rename", rename, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::strict_json_body<DeviceRenameRequest>);
    RUVIA_POST("/devices/transfer-owner", transferOwner, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<DeviceTransferOwnerRequest>);
    RUVIA_POST("/devices/delete", remove, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::strict_json_body<DeviceIdRequest>);
    RUVIA_POST("/devices/update", update, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>, ruvia::strict_json_body<DeviceIdRequest>);
    RUVIA_POST("/devices/windows-login", windowsLogin, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>, ruvia::strict_json_body<WindowsLoginApiRequest>);
    RUVIA_GET("/devices/restart-permission", restartPermissionStatus, UserAuthentication, ruvia::query_model<RestartPermissionQuery>);
    RUVIA_POST("/devices/restart", restart, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<DeviceRestartRequest>);
    RUVIA_GET("/devices/restart/:id", restartStatus, UserAuthentication);
    RUVIA_GET("/devices/:id/grants", grants, UserAuthentication);
    RUVIA_POST("/devices/:id/grants", saveGrant, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::strict_json_body<DeviceGrantRequest>);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> grants(ruvia::context& context);
    ruvia::task<ruvia::http_response> saveGrant(ruvia::context& context, const DeviceGrantRequest& body);
    ruvia::task<ruvia::http_response> install(ruvia::context& context);
    ruvia::task<ruvia::http_response> installManifest(ruvia::context& context, const InstallManifestRequest& body);
    ruvia::task<ruvia::http_response> installPackage(ruvia::context& context, const InstallPackageRequest& body);
    ruvia::task<ruvia::http_response> enrollment(ruvia::context& context, const DeviceEnrollmentRequest& body);
    ruvia::task<ruvia::http_response> devices(ruvia::context& context);
    ruvia::task<ruvia::http_response> create(ruvia::context& context, const DeviceCreateRequest& body);
    ruvia::task<ruvia::http_response> rename(ruvia::context& context, const DeviceRenameRequest& body);
    ruvia::task<ruvia::http_response> transferOwner(ruvia::context& context, const DeviceTransferOwnerRequest& body);
    ruvia::task<ruvia::http_response> remove(ruvia::context& context, const DeviceIdRequest& body);
    ruvia::task<ruvia::http_response> update(ruvia::context& context, const DeviceIdRequest& body);
    ruvia::task<ruvia::http_response> windowsLogin(ruvia::context& context, const WindowsLoginApiRequest& body);
    ruvia::task<ruvia::http_response> restartPermissionStatus(ruvia::context& context, const RestartPermissionQuery& query);
    ruvia::task<ruvia::http_response> restart(ruvia::context& context, const DeviceRestartRequest& body);
    ruvia::task<ruvia::http_response> restartStatus(ruvia::context& context);
};
} // namespace ultraterimal::server
