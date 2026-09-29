#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace ultraterimal::server {
struct DatabaseConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string username, password, database, socket, tlsCa;
};
DatabaseConfig parseDatabaseConfig(std::string_view json);
} // namespace ultraterimal::server
