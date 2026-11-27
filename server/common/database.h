#pragma once
#include <cstdint>
#include <db_entity.h>
#include <db_executor.h>
#include <db_repository.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ultraterimal::server {
inline constexpr ruvia::db_transaction_options kReadCommitted{
    .isolation_ = ruvia::db_transaction_isolation::read_committed,
};
template <typename T>
inline T dbField(const ruvia::db_field& field) {
    const auto parsed = field.as<T>();
    if (!parsed) {
        throw std::runtime_error("Unexpected NULL or invalid database field");
    }
    return *parsed;
}
template <typename T>
inline T dbFieldOr(const ruvia::db_field& field, T fallback) {
    const auto parsed = field.as<T>();
    return parsed ? *parsed : std::move(fallback);
}
template <typename T>
inline std::optional<T> dbOptional(const ruvia::db_field& field) {
    return field.as<T>();
}
inline ruvia::db_value dbNullableText(const std::optional<std::string>& value) {
    return value ? ruvia::db_value(std::string_view(*value)) : ruvia::db_value(nullptr);
}

RUVIA_DB_ENTITY(
    AuditEventEntity,
    "audit_events",
    RUVIA_DB_COLUMN(id, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int, .primary_key_ = true, .identity_ = true}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(session_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(event_type, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(success, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean}),
    RUVIA_DB_COLUMN(reason, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    OperationLockEntity,
    "operation_locks",
    RUVIA_DB_COLUMN(name, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 32}))

RUVIA_DB_ENTITY(
    GatewayLeaseEntity,
    "gateway_leases",
    RUVIA_DB_COLUMN(name, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 32}),
    RUVIA_DB_COLUMN(owner_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}))
} // namespace ultraterimal::server
