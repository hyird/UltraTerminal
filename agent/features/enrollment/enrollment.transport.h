#pragma once
#include <asio/awaitable.hpp>
#include <event_loop.h>
#include <string>

#include "features/connection/connection.config.h"

namespace ultraterimal::agent {
struct EnrollmentIdentity {
    std::string deviceId;
    std::string credential;
};
asio::awaitable<EnrollmentIdentity> exchangeEnrollment(ruvia::event_loop loop, ConnectionConfig config, std::string code, std::string name);
} // namespace ultraterimal::agent
