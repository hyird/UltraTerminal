#pragma once
#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "database.config.h"
#include "database.types.h"

namespace ultraterimal::server {
// Synchronous Connector/C adapter, used only by the bounded blocking pool or CLI.
// A connection and its transactions are never accessed concurrently.
class DatabaseConnection {
  public:
    explicit DatabaseConnection(const DatabaseConfig& config);
    ~DatabaseConnection();
    DatabaseConnection(DatabaseConnection&&) noexcept;
    DatabaseConnection& operator=(DatabaseConnection&&) = delete;
    DatabaseConnection(const DatabaseConnection&) = delete;
    DatabaseConnection& operator=(const DatabaseConnection&) = delete;
    DatabaseResult execute(std::string_view sql, std::span<const DatabaseParameter> parameters);
    template <typename... Args>
    DatabaseResult exec(std::string_view sql, Args&&... args) {
        const std::array<DatabaseParameter, sizeof...(Args)> parameters{DatabaseParameter(std::forward<Args>(args))...};
        return execute(sql, parameters);
    }

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class DatabaseTransaction {
  public:
    explicit DatabaseTransaction(DatabaseConnection& connection);
    ~DatabaseTransaction();
    DatabaseTransaction(const DatabaseTransaction&) = delete;
    DatabaseTransaction& operator=(const DatabaseTransaction&) = delete;
    template <typename... Args>
    DatabaseResult exec(std::string_view sql, Args&&... args) {
        if (!active_) {
            throw std::logic_error("Transaction has ended");
        }
        return connection_.exec(sql, std::forward<Args>(args)...);
    }
    void commit();

  private:
    DatabaseConnection& connection_;
    bool active_ = true;
};
} // namespace ultraterimal::server
