#pragma once
#include <cstdint>
#include <db_entity.h>
#include <string>

namespace ultraterimal::server {
RUVIA_DB_ENTITY(
    DeviceEntity,
    "devices",
    RUVIA_DB_COLUMN(id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(name, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(credential_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(owner_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(enrolled, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"TRUE"}}),
    RUVIA_DB_COLUMN(agent_version, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 64}),
    RUVIA_DB_COLUMN(revoked_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(last_seen_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    EnrollmentCodeEntity,
    "enrollment_codes",
    RUVIA_DB_COLUMN(code_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 64}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}),
    RUVIA_DB_COLUMN(consumed_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    DeviceCapabilityEntity,
    "device_capabilities",
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(protocol_version, std::int32_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::integer}),
    RUVIA_DB_COLUMN(protobuf, ruvia::bytes, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::bytea}),
    RUVIA_DB_COLUMN(updated_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    DeviceGrantEntity,
    "device_grants",
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(desktop, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"TRUE"}}),
    RUVIA_DB_COLUMN(vnc, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"TRUE"}}),
    RUVIA_DB_COLUMN(ssh, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"TRUE"}}),
    RUVIA_DB_COLUMN(write_control, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"TRUE"}}),
    RUVIA_DB_COLUMN(system_restart, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"FALSE"}}),
    RUVIA_DB_COLUMN(manage_credentials, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"FALSE"}}),
    RUVIA_DB_COLUMN(verification_method, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16, .default_expression_ = ruvia::fixed_string{"'direct'"}}),
    RUVIA_DB_COLUMN(revision, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int}))

RUVIA_DB_ENTITY(
    RestartOperationEntity,
    "restart_operations",
    RUVIA_DB_COLUMN(operation_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 64}),
    RUVIA_DB_COLUMN(idempotency_key, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(status, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}),
    RUVIA_DB_COLUMN(updated_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}),
    RUVIA_DB_COLUMN(result_status, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 16}),
    RUVIA_DB_COLUMN(win32_error, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int, .nullable_ = true}),
    RUVIA_DB_COLUMN(boot_time_unix_seconds, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int, .nullable_ = true}),
    RUVIA_DB_COLUMN(resolved_by, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(resolved_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}))
} // namespace ultraterimal::server
