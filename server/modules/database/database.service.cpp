#include "database.service.h"

#include <array>

#include "migration.schema.h"

namespace ultraterimal::server {
DatabaseConnection openDatabase(const DatabaseConfig& settings) { return DatabaseConnection(settings); }
void migrateDatabase(const DatabaseConfig& settings) {
    auto connection = openDatabase(settings);
    if (!connection.exec("SELECT GET_LOCK(CONCAT(DATABASE(),':migration'),2)")[0][0].as<bool>()) {
        throw std::runtime_error("Database migration is locked");
    }
    connection.exec("CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY,completed BOOLEAN NOT NULL DEFAULT FALSE,applied_at DATETIME(6) NOT NULL DEFAULT now(6)) ENGINE=InnoDB");
    constexpr std::array migrations{kMariaDbMigration, kControlOnlyMigration, kIdentityTelegramMigration, kGrantVerificationMigration, kSessionReconnectMigration, kRequesterTelegramMigration, kDeviceAgentVersionMigration};
    const auto versions = connection.exec("SELECT version,completed FROM schema_migrations ORDER BY version");
    for (std::size_t index = 0; index < versions.size(); ++index) {
        if (index >= migrations.size() || versions[index][0].as<int>() != static_cast<int>(index + 1) || !versions[index][1].as<bool>()) {
            throw std::runtime_error("Unsupported or incomplete MariaDB migration; restore the backup before retrying");
        }
    }
    // MariaDB DDL commits implicitly. Keep an incomplete marker until every
    // statement succeeds so a partial migration cannot be silently retried.
    for (std::size_t index = versions.size(); index < migrations.size(); ++index) {
        const auto version = static_cast<int>(index + 1);
        connection.exec("INSERT INTO schema_migrations(version) VALUES(?)", version);
        const auto migration = migrations[index];
        std::size_t offset = 0;
        while (offset < migration.size()) {
            const auto end = migration.find(';', offset);
            const auto statement = migration.substr(offset, end == std::string_view::npos ? migration.size() - offset : end - offset);
            if (statement.find_first_not_of(" \n\r\t") != std::string_view::npos) {
                connection.exec(statement);
            }
            if (end == std::string_view::npos) {
                break;
            }
            offset = end + 1;
        }
        connection.exec("UPDATE schema_migrations SET completed=TRUE WHERE version=?", version);
    }
}
DatabaseConnection acquireGatewayLease(const DatabaseConfig& settings) {
    auto connection = openDatabase(settings);
    if (!connection.exec("SELECT GET_LOCK(CONCAT(DATABASE(),':gateway'),0)")[0][0].as<bool>()) {
        throw std::runtime_error("A gateway already owns this database");
    }
    return connection;
}
bool gatewayLeaseAlive(DatabaseConnection& lease) {
    try {
        return lease.exec("SELECT COALESCE(IS_USED_LOCK(CONCAT(DATABASE(),':gateway'))=CONNECTION_ID(),FALSE)")[0][0].as<bool>();
    } catch (...) {
        return false;
    }
}
} // namespace ultraterimal::server
