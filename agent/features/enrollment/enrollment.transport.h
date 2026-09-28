#pragma once
#include <asio/awaitable.hpp>
#include <string>

#include <ruvia/core/EventLoop.h>

#include "features/connection/connection.config.h"

namespace ultraterimal::agent {
struct EnrollmentIdentity {
    std::string deviceId;
    std::string credential;
};
asio::awaitable<EnrollmentIdentity> exchangeEnrollment(ruvia::EventLoop loop, ConnectionConfig config, std::string code, std::string name);
} // namespace ultraterimal::agent
