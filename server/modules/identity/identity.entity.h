#pragma once
#include <cstdint>
#include <db_entity.h>
#include <string>

namespace ultraterimal::server {
RUVIA_DB_ENTITY(
    UserEntity,
    "users",
    RUVIA_DB_COLUMN(id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(username, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(password_salt, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(password_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(password_iterations, std::int32_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::integer}),
    RUVIA_DB_COLUMN(disabled, bool, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::boolean, .default_expression_ = ruvia::fixed_string{"FALSE"}}),
    RUVIA_DB_COLUMN(role, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16, .default_expression_ = ruvia::fixed_string{"'user'"}}),
    RUVIA_DB_COLUMN(totp_secret, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text, .default_expression_ = ruvia::fixed_string{"''"}}),
    RUVIA_DB_COLUMN(totp_pending, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text, .default_expression_ = ruvia::fixed_string{"''"}}),
    RUVIA_DB_COLUMN(totp_pending_expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(totp_last_step, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int, .default_expression_ = ruvia::fixed_string{"-1"}}),
    RUVIA_DB_COLUMN(failed_logins, std::int32_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::integer, .default_expression_ = ruvia::fixed_string{"0"}}),
    RUVIA_DB_COLUMN(login_blocked_until, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .nullable_ = true}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    LoginSessionEntity,
    "login_sessions",
    RUVIA_DB_COLUMN(token_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 64}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}))

RUVIA_DB_ENTITY(
    RecoveryCodeEntity,
    "recovery_codes",
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 128}),
    RUVIA_DB_COLUMN(code_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 64}))

RUVIA_DB_ENTITY(
    IdentityTelegramVerificationEntity,
    "identity_telegram_verifications",
    RUVIA_DB_COLUMN(id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 32}),
    RUVIA_DB_COLUMN(user_id, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 128}),
    RUVIA_DB_COLUMN(login_token_hash, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(telegram_id, std::int64_t, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::big_int}),
    RUVIA_DB_COLUMN(bot_generation, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}),
    RUVIA_DB_COLUMN(state, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16, .default_expression_ = ruvia::fixed_string{"'pending'"}}),
    RUVIA_DB_COLUMN(created_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp, .default_expression_ = ruvia::fixed_string{"UTC_TIMESTAMP(6)"}}),
    RUVIA_DB_COLUMN(expires_at, std::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp}))
} // namespace ultraterimal::server
