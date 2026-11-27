#pragma once
#include <controller.h>
#include <streaming.h>
#include <task.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class FileController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api/devices/files")
    RUVIA_ROUTES_BEGIN(FileController)
    RUVIA_GET("/list", list, UserAuthentication);
    RUVIA_GET("/stat", stat, UserAuthentication);
    RUVIA_GET("/download", download, UserAuthentication);
    RUVIA_POST("/upload", upload, ruvia::stream_body, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> list(ruvia::context& context);
    ruvia::task<ruvia::http_response> stat(ruvia::context& context);
    ruvia::task<void> download(ruvia::context& context);
    ruvia::task<ruvia::http_response> upload(ruvia::context& context);
};
} // namespace ultraterimal::server
