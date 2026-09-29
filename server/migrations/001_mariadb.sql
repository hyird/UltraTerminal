CREATE TABLE users (
    id VARCHAR(128) PRIMARY KEY,
    username VARCHAR(64) NOT NULL UNIQUE,
    password_salt CHAR(64) NOT NULL,
    password_hash CHAR(64) NOT NULL,
    password_iterations INTEGER NOT NULL CHECK (password_iterations >= 600000),
    disabled BOOLEAN NOT NULL DEFAULT FALSE,
    created_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE login_sessions (
    token_hash CHAR(64) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    expires_at DATETIME(6) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE INDEX login_sessions_user ON login_sessions(user_id);
CREATE TABLE enrollment_codes (
    code_hash CHAR(64) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    expires_at DATETIME(6) NOT NULL,
    consumed_at DATETIME(6),
    created_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE devices (
    id VARCHAR(128) PRIMARY KEY,
    name VARCHAR(128) NOT NULL CHECK (length(name) BETWEEN 1 AND 128),
    credential_hash CHAR(64) NOT NULL UNIQUE,
    revoked_at DATETIME(6),
    last_seen_at DATETIME(6),
    created_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE device_capabilities (
    device_id VARCHAR(128) PRIMARY KEY REFERENCES devices(id),
    protocol_version INTEGER NOT NULL,
    protobuf MEDIUMBLOB NOT NULL,
    updated_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE device_grants (
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    desktop BOOLEAN NOT NULL DEFAULT TRUE,
    vnc BOOLEAN NOT NULL DEFAULT TRUE,
    ssh BOOLEAN NOT NULL DEFAULT TRUE,
    write_control BOOLEAN NOT NULL DEFAULT TRUE,
    PRIMARY KEY (device_id, user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE audit_events (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    user_id VARCHAR(128) REFERENCES users(id),
    device_id VARCHAR(128) REFERENCES devices(id),
    event_type VARCHAR(128) NOT NULL,
    success BOOLEAN NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;

CREATE TABLE remote_sessions (
    id VARCHAR(128) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    kind INTEGER NOT NULL CHECK (kind IN (1,2,3)),
    write_control BOOLEAN NOT NULL,
    state VARCHAR(16) NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','active','closed')),
    created_at DATETIME(6) NOT NULL DEFAULT now(6),
    setup_expires_at DATETIME(6) NOT NULL,
    ended_at DATETIME(6),
    end_reason VARCHAR(128),
    CHECK ((state='closed') = (ended_at IS NOT NULL))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
ALTER TABLE remote_sessions ADD COLUMN active_writer VARCHAR(128) GENERATED ALWAYS AS (CASE WHEN kind IN (1,2) AND write_control AND state<>'closed' THEN device_id ELSE NULL END) PERSISTENT;
CREATE UNIQUE INDEX remote_sessions_writer ON remote_sessions(active_writer);
CREATE INDEX remote_sessions_user ON remote_sessions(user_id);
CREATE INDEX remote_sessions_device ON remote_sessions(device_id);
CREATE TABLE session_channels (
    id VARCHAR(128) PRIMARY KEY,
    session_id VARCHAR(128) NOT NULL REFERENCES remote_sessions(id),
    purpose INTEGER NOT NULL CHECK (purpose IN (2,3,4,5)),
    browser_ticket_hash CHAR(64) NOT NULL UNIQUE,
    agent_ticket_hash CHAR(64) NOT NULL UNIQUE,
    expires_at DATETIME(6) NOT NULL,
    browser_consumed_at DATETIME(6),
    agent_consumed_at DATETIME(6),
    UNIQUE(session_id,purpose)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
ALTER TABLE audit_events ADD COLUMN session_id VARCHAR(128) REFERENCES remote_sessions(id);
ALTER TABLE audit_events ADD COLUMN reason VARCHAR(128);

ALTER TABLE devices ADD COLUMN enrolled BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE enrollment_codes ADD COLUMN device_id VARCHAR(128) REFERENCES devices(id);
CREATE INDEX enrollment_codes_device ON enrollment_codes(device_id);
ALTER TABLE device_grants ADD COLUMN system_restart BOOLEAN NOT NULL DEFAULT FALSE;
CREATE TABLE restart_operations (
    operation_id CHAR(64) PRIMARY KEY CHECK(operation_id REGEXP '^[0-9a-f]{64}$'),
    idempotency_key VARCHAR(128) NOT NULL,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    status VARCHAR(16) NOT NULL CHECK(status IN ('PENDING','ACCEPTED','FAILED','UNKNOWN','COMPLETED','RESOLVED')),
    created_at DATETIME(6) NOT NULL DEFAULT now(6),
    expires_at DATETIME(6) NOT NULL,
    updated_at DATETIME(6) NOT NULL DEFAULT now(6),
    result_status VARCHAR(16) CHECK(result_status IS NULL OR result_status IN ('ACCEPTED','FAILED','UNKNOWN')),
    win32_error BIGINT,
    boot_time_unix_seconds BIGINT CHECK(boot_time_unix_seconds IS NULL OR boot_time_unix_seconds>=0),
    resolved_by VARCHAR(128) REFERENCES users(id),
    resolved_at DATETIME(6),
    CHECK ((status='RESOLVED')=(resolved_by IS NOT NULL AND resolved_at IS NOT NULL)),
    UNIQUE(user_id,idempotency_key),
    INDEX restart_operations_device_created(device_id,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
ALTER TABLE users ADD COLUMN role VARCHAR(16) NOT NULL DEFAULT 'user' CHECK (role IN ('admin','user'));

ALTER TABLE users ADD COLUMN totp_secret TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN totp_pending TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN totp_pending_expires_at DATETIME(6);
ALTER TABLE users ADD COLUMN totp_last_step BIGINT NOT NULL DEFAULT -1;
ALTER TABLE users ADD COLUMN failed_logins INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN login_blocked_until DATETIME(6);
CREATE TABLE recovery_codes (
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    code_hash CHAR(64) NOT NULL,
    PRIMARY KEY(user_id,code_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
ALTER TABLE devices ADD COLUMN owner_id VARCHAR(128) REFERENCES users(id);

ALTER TABLE device_grants ADD COLUMN manage_credentials BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE device_grants ADD COLUMN require_approval BOOLEAN NOT NULL DEFAULT FALSE;
CREATE SEQUENCE grant_revision_sequence;
ALTER TABLE device_grants ADD COLUMN revision BIGINT NOT NULL DEFAULT (NEXT VALUE FOR grant_revision_sequence);

ALTER TABLE remote_sessions ADD COLUMN login_token_hash CHAR(64);
ALTER TABLE remote_sessions ADD COLUMN grant_revision BIGINT;
CREATE INDEX remote_sessions_login ON remote_sessions(login_token_hash);

CREATE TABLE telegram_settings (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK(singleton),
    token TEXT NOT NULL,
    bot_username TEXT NOT NULL,
    webhook_secret TEXT NOT NULL,
    generation TEXT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE telegram_accounts (
    user_id VARCHAR(128) PRIMARY KEY REFERENCES users(id),
    telegram_id BIGINT NOT NULL UNIQUE CHECK(telegram_id>0),
    bound_at DATETIME(6) NOT NULL DEFAULT now(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE telegram_bindings (
    token_hash CHAR(64) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL UNIQUE REFERENCES users(id),
    login_token_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE TABLE connection_approvals (
    id VARCHAR(128) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    login_token_hash CHAR(64) NOT NULL,
    write_control BOOLEAN NOT NULL,
    grant_revision BIGINT NOT NULL,
    requester_telegram_id BIGINT NOT NULL,
    owner_id VARCHAR(128) NOT NULL REFERENCES users(id),
    owner_telegram_id BIGINT NOT NULL,
    bot_generation TEXT NOT NULL,
    state VARCHAR(16) NOT NULL CHECK(state IN ('pending','approved','denied','consumed','cancelled','failed')),
    created_at DATETIME(6) NOT NULL DEFAULT now(6),
    expires_at DATETIME(6) NOT NULL,
    decided_at DATETIME(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
CREATE INDEX connection_approvals_user ON connection_approvals(user_id,created_at DESC);

ALTER TABLE connection_approvals ADD COLUMN pending_device VARCHAR(128) GENERATED ALWAYS AS (CASE WHEN state='pending' THEN device_id ELSE NULL END) PERSISTENT;
CREATE UNIQUE INDEX connection_approvals_pending ON connection_approvals(user_id,pending_device);

CREATE TABLE operation_locks (name VARCHAR(32) PRIMARY KEY) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
INSERT INTO operation_locks(name) VALUES('bootstrap'),('accounts'),('telegram');
