#pragma once
#include <db_executor.h>
#include <string_view>
#include <task.h>

#include "config/database.config.h"

namespace ultraterimal::server {
void migrateDatabase(const DatabaseConfig& settings);
ruvia::task<bool> acquireGatewayLease(ruvia::db_executor db, std::string_view ownerId);
ruvia::task<bool> renewGatewayLease(ruvia::db_executor db, std::string_view ownerId);
ruvia::task<void> releaseGatewayLease(ruvia::db_executor db, std::string_view ownerId);
ruvia::task<void> pruneExpiredHistory(ruvia::db_executor db);
} // namespace ultraterimal::server
