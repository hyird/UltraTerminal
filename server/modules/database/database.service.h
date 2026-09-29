#pragma once
#include "database.transport.h"
namespace ultraterimal::server {
DatabaseConnection openDatabase(const DatabaseConfig& settings);
void migrateDatabase(const DatabaseConfig& settings);
DatabaseConnection acquireGatewayLease(const DatabaseConfig& settings);
bool gatewayLeaseAlive(DatabaseConnection& lease);
} // namespace ultraterimal::server
