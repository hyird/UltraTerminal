#pragma once
#include <cstdint>
#include <db_entity.h>
#include <string>

namespace ultraterimal::server {
RUVIA_DB_ENTITY(
    TelegramSettingsEntity,
    "telegram_settings",
    RUVIA_DB_COLUMN(singleton, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .primary_key_ = true, .default_expression_ = ruvia::fixed_string{"TRUE"}}),
    RUVIA_DB_COLUMN(token, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}),
    RUVIA_DB_COLUMN(bot_username, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}),
    RUVIA_DB_COLUMN(webhook_secret, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}),
    RUVIA_DB_COLUMN(generation, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}))

RUVIA_DB_ENTITY(
    TelegramAccountEntity,
    "telegram_accounts",
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(telegram_id, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int}),
    RUVIA_DB_COLUMN(bound_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    TelegramBindingEntity,
    "telegram_bindings",
    RUVIA_DB_COLUMN(token_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 64}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(login_token_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}))

RUVIA_DB_ENTITY(
    ConnectionVerificationEntity,
    "connection_verifications",
    RUVIA_DB_COLUMN(id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(device_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(login_token_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(write_control, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean}),
    RUVIA_DB_COLUMN(grant_revision, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int}),
    RUVIA_DB_COLUMN(requester_telegram_id, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int}),
    RUVIA_DB_COLUMN(bot_generation, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}),
    RUVIA_DB_COLUMN(state, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}),
    RUVIA_DB_COLUMN(decided_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}))
} // namespace ultraterimal::server
