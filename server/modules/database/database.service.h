#pragma once
#include <string>

#include <pqxx/pqxx>
namespace ultraterimal::server {
pqxx::connection openDatabase(const std::string& settings);
void migrateDatabase(const std::string& settings);
pqxx::connection acquireGatewayLease(const std::string& settings);
bool gatewayLeaseAlive(pqxx::connection& lease);
} // namespace ultraterimal::server
