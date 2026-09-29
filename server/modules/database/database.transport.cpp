#include "database.transport.h"

#include <mutex>
#include <mysql.h>

#include <boost/json.hpp>

namespace ultraterimal::server {
namespace {
void initializeThread() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        if (mysql_library_init(0, nullptr, nullptr)) {
            throw DatabaseError(0);
        }
    });
    struct Thread {
        Thread() {
            if (mysql_thread_init()) {
                throw DatabaseError(0);
            }
        }
        ~Thread() { mysql_thread_end(); }
    };
    thread_local Thread thread;
    (void)thread;
}
using Statement = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;
using Metadata = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;
} // namespace
DatabaseConfig parseDatabaseConfig(std::string_view json) {
    const auto value = boost::json::parse(json);
    if (!value.is_object()) {
        throw std::invalid_argument("Expected MariaDB configuration object");
    }
    const auto& object = value.as_object();
    DatabaseConfig result;
    for (const auto& item : object) {
        if (item.key() == "port") {
            const auto port = item.value().as_int64();
            if (port < 1 || port > 65535) {
                throw std::invalid_argument("Invalid MariaDB port");
            }
            result.port = static_cast<std::uint16_t>(port);
            continue;
        }
        if (!item.value().is_string() || item.value().as_string().size() > 4096 || item.value().as_string().find('\0') != boost::json::string::npos) {
            throw std::invalid_argument("Invalid MariaDB setting");
        }
        const std::string text(item.value().as_string());
        if (item.key() == "host") {
            result.host = text;
        } else if (item.key() == "username") {
            result.username = text;
        } else if (item.key() == "password") {
            result.password = text;
        } else if (item.key() == "database") {
            result.database = text;
        } else if (item.key() == "socket") {
            result.socket = text;
        } else if (item.key() == "tls_ca") {
            result.tlsCa = text;
        } else {
            throw std::invalid_argument("Unknown MariaDB setting");
        }
    }
    if (result.username.empty() || result.database.empty() || result.host.empty() || result.database.size() > 64 || result.database.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
        throw std::invalid_argument("Invalid MariaDB identity");
    }
    if (!result.socket.empty() && result.host != "localhost") {
        throw std::invalid_argument("MariaDB socket requires localhost");
    }
    if (result.socket.empty() && result.host != "127.0.0.1" && result.host != "::1" && result.tlsCa.empty()) {
        throw std::invalid_argument("Remote MariaDB requires tls_ca");
    }
    return result;
}
struct DatabaseConnection::State {
    MYSQL* handle = nullptr;
    ~State() {
        if (handle) {
            mysql_close(handle);
        }
    }
};
DatabaseConnection::DatabaseConnection(const DatabaseConfig& config) : state_(std::make_unique<State>()) {
    initializeThread();
    auto* handle = state_->handle = mysql_init(nullptr);
    if (!handle) {
        throw DatabaseError(0);
    }
    const unsigned connectTimeout = 3, ioTimeout = 10, disableLocal = 0;
    const my_bool noReconnect = 0, verify = 1;
    const auto option = [&](mysql_option key, const void* value) {
        if (mysql_options(handle, key, value)) {
            throw DatabaseError(mysql_errno(handle));
        }
    };
    option(MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    option(MYSQL_OPT_READ_TIMEOUT, &ioTimeout);
    option(MYSQL_OPT_WRITE_TIMEOUT, &ioTimeout);
    option(MYSQL_OPT_RECONNECT, &noReconnect);
    option(MYSQL_OPT_LOCAL_INFILE, &disableLocal);
    option(MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!config.tlsCa.empty()) {
        option(MYSQL_OPT_SSL_CA, config.tlsCa.c_str());
        option(MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
        option(MYSQL_OPT_SSL_ENFORCE, &verify);
    }
    if (!mysql_real_connect(handle, config.host.c_str(), config.username.c_str(), config.password.c_str(), config.database.c_str(), config.port, config.socket.empty() ? nullptr : config.socket.c_str(), CLIENT_FOUND_ROWS)) {
        throw DatabaseError(mysql_errno(handle));
    }
    if (!config.tlsCa.empty() && !mysql_get_ssl_cipher(handle)) {
        throw std::runtime_error("MariaDB TLS required");
    }
    exec("SET SESSION time_zone='+00:00',max_statement_time=5,innodb_lock_wait_timeout=2,sql_mode='STRICT_ALL_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'");
    exec("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
}
DatabaseConnection::~DatabaseConnection() = default;
DatabaseConnection::DatabaseConnection(DatabaseConnection&&) noexcept = default;
DatabaseResult DatabaseConnection::execute(std::string_view sql, std::span<const DatabaseParameter> parameters) {
    initializeThread();
    if (sql.size() > 1024 * 1024 || parameters.size() > 128) {
        throw std::length_error("Database query too large");
    }
    Statement statement(mysql_stmt_init(state_->handle), mysql_stmt_close);
    if (!statement) {
        throw DatabaseError(mysql_errno(state_->handle));
    }
    auto* handle = statement.get();
    const auto check = [&](int status) { if (status) { throw DatabaseError(mysql_stmt_errno(handle)); } };
    check(mysql_stmt_prepare(handle, sql.data(), static_cast<unsigned long>(sql.size())));
    if (mysql_stmt_param_count(handle) != parameters.size()) {
        throw std::invalid_argument("Database parameter count mismatch");
    }
    std::vector<MYSQL_BIND> input(parameters.size());
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto& parameter = parameters[i];
        input[i].buffer_type = !parameter.value ? MYSQL_TYPE_NULL : parameter.binary ? MYSQL_TYPE_BLOB
                                                                                     : MYSQL_TYPE_STRING;
        if (parameter.value) {
            if (parameter.value->size() > 1024 * 1024) {
                throw std::length_error("Database parameter too large");
            }
            input[i].buffer = const_cast<char*>(parameter.value->data());
            input[i].buffer_length = static_cast<unsigned long>(parameter.value->size());
        }
    }
    if (!input.empty()) {
        check(mysql_stmt_bind_param(handle, input.data()));
    }
    check(mysql_stmt_execute(handle));
    DatabaseResult result;
    Metadata metadata(mysql_stmt_result_metadata(handle), mysql_free_result);
    if (!metadata) {
        if (mysql_stmt_field_count(handle)) {
            throw DatabaseError(mysql_stmt_errno(handle));
        }
        result.affectedRows = mysql_stmt_affected_rows(handle);
        return result;
    }
    const auto columns = mysql_num_fields(metadata.get());
    if (columns > 128) {
        throw std::length_error("Database result too wide");
    }
    const auto* fields = mysql_fetch_fields(metadata.get());
    std::vector<MYSQL_BIND> output(columns);
    std::vector<unsigned long> lengths(columns);
    std::vector<my_bool> nulls(columns);
    for (unsigned i = 0; i < columns; ++i) {
        output[i].buffer_type = MYSQL_TYPE_STRING;
        output[i].length = &lengths[i];
        output[i].is_null = &nulls[i];
    }
    check(mysql_stmt_bind_result(handle, output.data()));
    std::size_t totalBytes = 0;
    while (true) {
        const int status = mysql_stmt_fetch(handle);
        if (status == MYSQL_NO_DATA) {
            break;
        }
        if (status != 0 && status != MYSQL_DATA_TRUNCATED) {
            check(status);
        }
        if (result.rows.size() >= 100000) {
            throw std::length_error("Database result too long");
        }
        DatabaseRow row;
        row.fields.reserve(columns);
        for (unsigned i = 0; i < columns; ++i) {
            DatabaseField field;
            if (!nulls[i]) {
                totalBytes += lengths[i];
                if (lengths[i] > 1024 * 1024 || totalBytes > 16 * 1024 * 1024) {
                    throw std::length_error("Database result too large");
                }
                field.value.emplace(lengths[i], '\0');
                MYSQL_BIND cell{};
                cell.buffer_type = MYSQL_TYPE_STRING;
                cell.buffer = field.value->data();
                cell.buffer_length = lengths[i];
                check(mysql_stmt_fetch_column(handle, &cell, i, 0));
            }
            row.fields.emplace_back(fields[i].name, std::move(field));
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}
DatabaseTransaction::DatabaseTransaction(DatabaseConnection& connection) : connection_(connection) { connection_.exec("START TRANSACTION"); }
DatabaseTransaction::~DatabaseTransaction() {
    if (active_) {
        try {
            connection_.exec("ROLLBACK");
        } catch (...) {
        }
    }
}
void DatabaseTransaction::commit() {
    if (!active_) {
        throw std::logic_error("Transaction has ended");
    }
    connection_.exec("COMMIT");
    active_ = false;
}
} // namespace ultraterimal::server
