#pragma once
#include <cstdint>
#include <db_executor.h>
#include <string>
#include <string_view>

namespace ultraterimal::server {
struct DatabaseConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string username, password, database, tlsCa;
};
DatabaseConfig parseDatabaseConfig(std::string_view json);
ruvia::db_config toRuviaDbConfig(const DatabaseConfig& config, std::size_t poolSizePerWorker = 4);
} // namespace ultraterimal::server
