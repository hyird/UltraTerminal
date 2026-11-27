#pragma once
#include <cstdint>
#include <db_entity.h>
#include <string>

namespace ultraterimal::server {
RUVIA_DB_ENTITY(
    RemoteSessionEntity,
    "remote_sessions",
    RUVIA_DB_COLUMN(id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(kind, std::int32_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::integer}),
    RUVIA_DB_COLUMN(write_control, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean}),
    RUVIA_DB_COLUMN(state, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16, .default_expression_ = ruvia::fixed_string{"'pending'"}}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}),
    RUVIA_DB_COLUMN(setup_expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}),
    RUVIA_DB_COLUMN(ended_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(end_reason, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(login_token_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 64}),
    RUVIA_DB_COLUMN(grant_revision, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int, .nullable_ = true}),
    RUVIA_DB_COLUMN(reconnect_until, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(reconnected_as, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}))

RUVIA_DB_ENTITY(
    SessionChannelEntity,
    "session_channels",
    RUVIA_DB_COLUMN(id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(session_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(purpose, std::int32_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::integer}),
    RUVIA_DB_COLUMN(browser_ticket_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(agent_ticket_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}),
    RUVIA_DB_COLUMN(browser_consumed_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(agent_consumed_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}))
} // namespace ultraterimal::server
