#include "database.service.h"

#include "migration.schema.h"
namespace ultraterimal::server {
pqxx::connection openDatabase(const std::string& settings) {
    pqxx::connection connection(settings);
    pqxx::work transaction(connection);
    transaction.exec("SET statement_timeout = '5s'; SET lock_timeout = '2s'; SET idle_in_transaction_session_timeout = '10s'; SET TIME ZONE 'UTC'");
    transaction.commit();
    return connection;
}
void migrateDatabase(const std::string& settings) {
    auto connection = openDatabase(settings);
    pqxx::work transaction(connection);
    transaction.exec("SELECT pg_advisory_xact_lock(842105211)");
    transaction.exec("CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY, applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=1").empty()) {
        transaction.exec(kInitialMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(1)");
    }
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=2").empty()) {
        transaction.exec(kSessionMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(2)");
    }
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=3").empty()) {
        transaction.exec(kAccountMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(3)");
    }
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=4").empty()) {
        transaction.exec(kRestartMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(4)");
    }
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=5").empty()) {
        transaction.exec(kRestartResolutionMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(5)");
    }
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=6").empty()) {
        transaction.exec(kRestartDirectMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(6)");
    }
    if (transaction.exec("SELECT version FROM schema_migrations WHERE version=7").empty()) {
        transaction.exec(kDeviceProvisioningMigration);
        transaction.exec("INSERT INTO schema_migrations(version) VALUES(7)");
    }
    transaction.commit();
}
pqxx::connection acquireGatewayLease(const std::string& settings) {
    auto connection = openDatabase(settings);
    pqxx::work transaction(connection);
    if (!transaction.exec("SELECT pg_try_advisory_lock(842105213)")[0][0].as<bool>()) {
        throw std::runtime_error("A gateway already owns this database");
    }
    transaction.commit();
    return connection;
}
bool gatewayLeaseAlive(pqxx::connection& lease) {
    try {
        pqxx::read_transaction transaction(lease);
        return transaction.exec("SELECT EXISTS(SELECT 1 FROM pg_locks WHERE locktype='advisory' AND pid=pg_backend_pid() AND classid=0 AND objid=842105213 AND granted)")[0][0].as<bool>();
    } catch (...) {
        return false;
    }
}
} // namespace ultraterimal::server
