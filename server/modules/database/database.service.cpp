#include "database.service.h"

#include "migration.schema.h"

namespace ultraterimal::server {
DatabaseConnection openDatabase(const DatabaseConfig& settings) { return DatabaseConnection(settings); }
void migrateDatabase(const DatabaseConfig& settings) {
    auto connection = openDatabase(settings);
    if (!connection.exec("SELECT GET_LOCK(CONCAT(DATABASE(),':migration'),2)")[0][0].as<bool>()) {
        throw std::runtime_error("Database migration is locked");
    }
    connection.exec("CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY,completed BOOLEAN NOT NULL DEFAULT FALSE,applied_at DATETIME(6) NOT NULL DEFAULT now(6)) ENGINE=InnoDB");
    const auto versions = connection.exec("SELECT version,completed FROM schema_migrations");
    if (!versions.empty()) {
        if (versions.size() != 1 || versions[0][0].as<int>() != 1 || !versions[0][1].as<bool>()) {
            throw std::runtime_error("Unsupported or incomplete MariaDB migration; restore the backup before retrying");
        }
        return;
    }
    // MariaDB DDL commits implicitly. A durable incomplete marker prevents a
    // partial schema from being mistaken for a successful migration on restart.
    connection.exec("INSERT INTO schema_migrations(version) VALUES(1)");
    std::size_t offset = 0;
    while (offset < kMariaDbMigration.size()) {
        const auto end = kMariaDbMigration.find(';', offset);
        const auto statement = kMariaDbMigration.substr(offset, end == std::string_view::npos ? kMariaDbMigration.size() - offset : end - offset);
        if (statement.find_first_not_of(" \n\r\t") != std::string_view::npos) {
            connection.exec(statement);
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    connection.exec("UPDATE schema_migrations SET completed=TRUE WHERE version=1");
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
