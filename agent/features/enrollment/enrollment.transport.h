#pragma once
#include <asio/awaitable.hpp>
#include <string>

#include <ruvia/core/execution_handle.h>

#include "features/connection/connection.config.h"

namespace ultraterimal::agent {
struct EnrollmentIdentity {
    std::string deviceId;
    std::string credential;
};
asio::awaitable<EnrollmentIdentity> exchangeEnrollment(ruvia::execution_handle loop, ConnectionConfig config, std::string code, std::string name);
} // namespace ultraterimal::agent
