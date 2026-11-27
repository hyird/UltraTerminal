#pragma once
#include <db_migration.h>
#include <vector>

namespace ultraterimal::server {
// 全新安装基线：所有表结构与初始种子均在此以 C++ 声明，由 ruvia::db_migrator 管理。
inline const std::vector<ruvia::db_migration> kSchemaMigrations{
    ruvia::db_migration({.id_ = "001_users", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id VARCHAR(128) PRIMARY KEY,
    username VARCHAR(64) NOT NULL UNIQUE,
    password_salt CHAR(64) NOT NULL,
    password_hash CHAR(64) NOT NULL,
    password_iterations INTEGER NOT NULL CHECK (password_iterations >= 600000),
    disabled BOOLEAN NOT NULL DEFAULT FALSE,
    role VARCHAR(16) NOT NULL DEFAULT 'user' CHECK (role IN ('admin','user')),
    totp_secret TEXT NOT NULL DEFAULT '',
    totp_pending TEXT NOT NULL DEFAULT '',
    totp_pending_expires_at DATETIME(6),
    totp_last_step BIGINT NOT NULL DEFAULT -1,
    failed_logins INTEGER NOT NULL DEFAULT 0,
    login_blocked_until DATETIME(6),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_login_sessions", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS login_sessions (
    token_hash CHAR(64) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    expires_at DATETIME(6) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    INDEX login_sessions_user(user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_recovery_codes", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS recovery_codes (
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    code_hash CHAR(64) NOT NULL,
    PRIMARY KEY(user_id,code_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_devices", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS devices (
    id VARCHAR(128) PRIMARY KEY,
    name VARCHAR(128) NOT NULL CHECK (length(name) BETWEEN 1 AND 128),
    credential_hash CHAR(64) NOT NULL UNIQUE,
    owner_id VARCHAR(128) REFERENCES users(id),
    enrolled BOOLEAN NOT NULL DEFAULT TRUE,
    agent_version VARCHAR(64),
    revoked_at DATETIME(6),
    last_seen_at DATETIME(6),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_enrollment_codes", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS enrollment_codes (
    code_hash CHAR(64) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) REFERENCES devices(id),
    expires_at DATETIME(6) NOT NULL,
    consumed_at DATETIME(6),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    INDEX enrollment_codes_device(device_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_device_capabilities", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS device_capabilities (
    device_id VARCHAR(128) PRIMARY KEY REFERENCES devices(id),
    protocol_version INTEGER NOT NULL,
    protobuf MEDIUMBLOB NOT NULL,
    updated_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_grant_revision_sequence", .sql_ = "CREATE SEQUENCE IF NOT EXISTS grant_revision_sequence"}),
    ruvia::db_migration({.id_ = "001_device_grants", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS device_grants (
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    desktop BOOLEAN NOT NULL DEFAULT TRUE,
    vnc BOOLEAN NOT NULL DEFAULT TRUE,
    ssh BOOLEAN NOT NULL DEFAULT TRUE,
    write_control BOOLEAN NOT NULL DEFAULT TRUE,
    system_restart BOOLEAN NOT NULL DEFAULT FALSE,
    manage_credentials BOOLEAN NOT NULL DEFAULT FALSE,
    verification_method VARCHAR(16) NOT NULL DEFAULT 'direct',
    revision BIGINT NOT NULL DEFAULT (NEXT VALUE FOR grant_revision_sequence),
    PRIMARY KEY (device_id, user_id),
    CONSTRAINT grants_control_only CHECK (write_control OR NOT (desktop OR vnc OR ssh)),
    CONSTRAINT grants_verification_method CHECK (verification_method IN ('direct','telegram','totp'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_remote_sessions", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS remote_sessions (
    id VARCHAR(128) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    kind INTEGER NOT NULL CHECK (kind IN (1,2,3)),
    write_control BOOLEAN NOT NULL,
    state VARCHAR(16) NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','active','closed')),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    setup_expires_at DATETIME(6) NOT NULL,
    ended_at DATETIME(6),
    end_reason VARCHAR(128),
    login_token_hash CHAR(64),
    grant_revision BIGINT,
    reconnect_until DATETIME(6) NULL,
    reconnected_as VARCHAR(128) NULL,
    active_writer VARCHAR(128) GENERATED ALWAYS AS (CASE WHEN kind IN (1,2) AND write_control AND state<>'closed' THEN device_id ELSE NULL END) PERSISTENT,
    CHECK ((state='closed') = (ended_at IS NOT NULL)),
    CONSTRAINT sessions_control_only CHECK (write_control OR state='closed'),
    UNIQUE INDEX remote_sessions_writer(active_writer),
    INDEX remote_sessions_user(user_id),
    INDEX remote_sessions_device(device_id),
    INDEX remote_sessions_login(login_token_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_session_channels", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS session_channels (
    id VARCHAR(128) PRIMARY KEY,
    session_id VARCHAR(128) NOT NULL REFERENCES remote_sessions(id),
    purpose INTEGER NOT NULL CHECK (purpose IN (2,3,4,5)),
    browser_ticket_hash CHAR(64) NOT NULL UNIQUE,
    agent_ticket_hash CHAR(64) NOT NULL UNIQUE,
    expires_at DATETIME(6) NOT NULL,
    browser_consumed_at DATETIME(6),
    agent_consumed_at DATETIME(6),
    UNIQUE(session_id,purpose)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_audit_events", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS audit_events (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    user_id VARCHAR(128) REFERENCES users(id),
    device_id VARCHAR(128) REFERENCES devices(id),
    session_id VARCHAR(128) REFERENCES remote_sessions(id),
    event_type VARCHAR(128) NOT NULL,
    success BOOLEAN NOT NULL,
    reason VARCHAR(128),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_restart_operations", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS restart_operations (
    operation_id CHAR(64) PRIMARY KEY CHECK(operation_id REGEXP '^[0-9a-f]{64}$'),
    idempotency_key VARCHAR(128) NOT NULL,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    status VARCHAR(16) NOT NULL CHECK(status IN ('PENDING','ACCEPTED','FAILED','UNKNOWN','COMPLETED','RESOLVED')),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    expires_at DATETIME(6) NOT NULL,
    updated_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    result_status VARCHAR(16) CHECK(result_status IS NULL OR result_status IN ('ACCEPTED','FAILED','UNKNOWN')),
    win32_error BIGINT,
    boot_time_unix_seconds BIGINT CHECK(boot_time_unix_seconds IS NULL OR boot_time_unix_seconds>=0),
    resolved_by VARCHAR(128) REFERENCES users(id),
    resolved_at DATETIME(6),
    CHECK ((status='RESOLVED')=(resolved_by IS NOT NULL AND resolved_at IS NOT NULL)),
    UNIQUE(user_id,idempotency_key),
    INDEX restart_operations_device_created(device_id,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_telegram_settings", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS telegram_settings (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK(singleton),
    token TEXT NOT NULL,
    bot_username TEXT NOT NULL,
    webhook_secret TEXT NOT NULL,
    generation TEXT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_telegram_accounts", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS telegram_accounts (
    user_id VARCHAR(128) PRIMARY KEY REFERENCES users(id),
    telegram_id BIGINT NOT NULL UNIQUE CHECK(telegram_id>0),
    bound_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_telegram_bindings", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS telegram_bindings (
    token_hash CHAR(64) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL UNIQUE REFERENCES users(id),
    login_token_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_identity_telegram_verifications", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS identity_telegram_verifications (
    id CHAR(32) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL,
    login_token_hash CHAR(64) NOT NULL,
    telegram_id BIGINT NOT NULL,
    bot_generation VARCHAR(64) NOT NULL,
    state VARCHAR(16) NOT NULL DEFAULT 'pending',
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    expires_at DATETIME(6) NOT NULL,
    INDEX identity_telegram_user(user_id, created_at),
    CHECK (state IN ('pending','approved','denied','consumed','failed'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_connection_verifications", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS connection_verifications (
    id VARCHAR(128) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    login_token_hash CHAR(64) NOT NULL,
    write_control BOOLEAN NOT NULL,
    grant_revision BIGINT NOT NULL,
    requester_telegram_id BIGINT NOT NULL,
    bot_generation TEXT NOT NULL,
    state VARCHAR(16) NOT NULL CHECK(state IN ('pending','approved','denied','consumed','cancelled','failed')),
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    expires_at DATETIME(6) NOT NULL,
    decided_at DATETIME(6),
    pending_device VARCHAR(128) GENERATED ALWAYS AS (CASE WHEN state='pending' THEN device_id ELSE NULL END) PERSISTENT,
    UNIQUE KEY connection_verifications_pending(user_id,pending_device),
    KEY connection_verifications_user(user_id,created_at DESC),
    CONSTRAINT verifications_control_only CHECK(write_control OR state NOT IN ('pending','approved'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_operation_locks", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS operation_locks (
    name VARCHAR(32) PRIMARY KEY
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "001_operation_locks_seed", .sql_ = "INSERT IGNORE INTO operation_locks(name) VALUES('bootstrap'),('accounts'),('telegram')"}),
    ruvia::db_migration({.id_ = "001_gateway_leases", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS gateway_leases (
    name VARCHAR(32) PRIMARY KEY,
    owner_id CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
    ruvia::db_migration({.id_ = "002_devices_platform", .sql_ = R"SQL(
ALTER TABLE devices ADD COLUMN IF NOT EXISTS platform VARCHAR(32) NOT NULL DEFAULT 'windows-x64')SQL"}),
    ruvia::db_migration({.id_ = "003_api_tokens", .sql_ = R"SQL(
CREATE TABLE IF NOT EXISTS api_tokens (
    id VARCHAR(128) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    name VARCHAR(128) NOT NULL,
    token_prefix VARCHAR(16) NOT NULL,
    token_hash CHAR(64) NOT NULL UNIQUE,
    scopes VARCHAR(512) NOT NULL,
    device_ids TEXT NOT NULL DEFAULT '',
    expires_at DATETIME(6) NULL,
    last_used_at DATETIME(6) NULL,
    revoked_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT UTC_TIMESTAMP(6),
    INDEX api_tokens_user(user_id, created_at DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin)SQL"}),
};
} // namespace ultraterimal::server
