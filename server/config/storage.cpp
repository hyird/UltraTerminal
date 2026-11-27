#include "config/storage.h"

#include <chrono>
#include <filesystem>
#include <model.h>
#include <stdexcept>

#include "common/database.h"
#include "config/schema.h"

namespace ultraterimal::server {
namespace {
RUVIA_MODEL(DatabaseConfigModel,
            RUVIA_OPTIONAL_FIELD(host, ruvia::string),
            RUVIA_OPTIONAL_FIELD(port, std::int64_t),
            RUVIA_OPTIONAL_FIELD(username, ruvia::string),
            RUVIA_OPTIONAL_FIELD(password, ruvia::string),
            RUVIA_OPTIONAL_FIELD(database, ruvia::string),
            RUVIA_OPTIONAL_FIELD(tls_ca, ruvia::string));
} // namespace
DatabaseConfig parseDatabaseConfig(std::string_view json) {
    auto parsed = ruvia::parse_json<DatabaseConfigModel>(json, {.reject_unknown_fields_ = true});
    if (!parsed.has_value()) {
        throw std::invalid_argument("Invalid MariaDB configuration");
    }
    const auto model = std::move(parsed).take_value();
    const auto readText = [](const ruvia::string& value) {
        const auto view = value.view();
        if (view.size() > 4096 || view.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("Invalid MariaDB setting");
        }
        return std::string(view);
    };
    DatabaseConfig result;
    if (const auto& portValue = model.get<"port">()) {
        if (*portValue < 1 || *portValue > 65535) {
            throw std::invalid_argument("Invalid MariaDB port");
        }
        result.port = static_cast<std::uint16_t>(*portValue);
    }
    if (const auto& host = model.get<"host">()) {
        result.host = readText(*host);
    }
    if (const auto& username = model.get<"username">()) {
        result.username = readText(*username);
    }
    if (const auto& password = model.get<"password">()) {
        result.password = readText(*password);
    }
    if (const auto& database = model.get<"database">()) {
        result.database = readText(*database);
    }
    if (const auto& tlsCa = model.get<"tls_ca">()) {
        result.tlsCa = readText(*tlsCa);
    }
    if (result.username.empty() || result.database.empty() || result.host.empty() || result.database.size() > 64 || result.database.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string_view::npos) {
        throw std::invalid_argument("Invalid MariaDB identity");
    }
    if (result.host != "127.0.0.1" && result.host != "::1" && result.tlsCa.empty()) {
        throw std::invalid_argument("Remote MariaDB requires tls_ca");
    }
    return result;
}
ruvia::db_config toRuviaDbConfig(const DatabaseConfig& config, std::size_t poolSizePerWorker) {
    ruvia::db_config out{};
    out.driver_ = ruvia::db_driver::mariadb;
    out.host_ = config.host;
    out.port_ = ruvia::port_number(config.port);
    out.username_ = config.username;
    out.password_ = config.password;
    out.database_ = config.database;
    out.pool_size_per_worker_ = poolSizePerWorker;
    out.connect_timeout_ = std::chrono::seconds(3);
    out.read_timeout_ = std::chrono::seconds(10);
    out.write_timeout_ = std::chrono::seconds(10);
    out.command_timeout_ = std::chrono::seconds(10);
    out.acquire_timeout_ = std::chrono::seconds(5);
    out.transaction_timeout_ = std::chrono::seconds(30);
    if (!config.tlsCa.empty()) {
        out.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        out.tls_.ca_file_ = std::filesystem::path(std::u8string(config.tlsCa.begin(), config.tlsCa.end()));
    } else {
        out.tls_.mode_ = ruvia::client_tls_mode::disabled;
    }
    return out;
}
void migrateDatabase(const DatabaseConfig& settings) {
    (void)ruvia::db_migrator::run(toRuviaDbConfig(settings, 1), kSchemaMigrations, {.table_ = "schema_migrations"});
}
ruvia::task<bool> acquireGatewayLease(ruvia::db_executor db, std::string_view ownerId) {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto existing = co_await tx.query("SELECT owner_id,expires_at>UTC_TIMESTAMP(6) AS active FROM gateway_leases WHERE name='gateway' FOR UPDATE");
    if (!existing.empty() && dbField<bool>(existing[0]["active"]) && dbField<std::string>(existing[0]["owner_id"]) != ownerId) {
        co_await tx.rollback();
        co_return false;
    }
    if (existing.empty()) {
        co_await tx.execute("INSERT INTO gateway_leases(name,owner_id,expires_at) VALUES('gateway',?,UTC_TIMESTAMP(6)+INTERVAL 10 SECOND)", ownerId);
    } else {
        co_await tx.execute("UPDATE gateway_leases SET owner_id=?,expires_at=UTC_TIMESTAMP(6)+INTERVAL 10 SECOND WHERE name='gateway'", ownerId);
    }
    co_await tx.commit();
    co_return true;
}
ruvia::task<bool> renewGatewayLease(ruvia::db_executor db, std::string_view ownerId) {
    try {
        const auto result = co_await db.execute("UPDATE gateway_leases SET expires_at=UTC_TIMESTAMP(6)+INTERVAL 10 SECOND WHERE name='gateway' AND owner_id=? AND expires_at>UTC_TIMESTAMP(6)", ownerId);
        co_return result.affected_rows() == 1;
    } catch (...) {
        co_return false;
    }
}
ruvia::task<void> releaseGatewayLease(ruvia::db_executor db, std::string_view ownerId) {
    try {
        (void)co_await db.repository<GatewayLeaseEntity>().delete_by(GatewayLeaseEntity::column<"name">() == "gateway" && GatewayLeaseEntity::column<"owner_id">() == ownerId);
    } catch (...) {
    }
}
} // namespace ultraterimal::server
