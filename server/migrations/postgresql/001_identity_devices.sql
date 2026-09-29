CREATE TABLE users (
    id TEXT PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    password_salt TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    password_iterations INTEGER NOT NULL CHECK (password_iterations >= 600000),
    disabled BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE login_sessions (
    token_hash TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id),
    expires_at TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX login_sessions_user ON login_sessions(user_id);
CREATE TABLE enrollment_codes (
    code_hash TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id),
    expires_at TIMESTAMPTZ NOT NULL,
    consumed_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE devices (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 128),
    credential_hash TEXT NOT NULL UNIQUE,
    revoked_at TIMESTAMPTZ,
    last_seen_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE device_capabilities (
    device_id TEXT PRIMARY KEY REFERENCES devices(id),
    protocol_version INTEGER NOT NULL,
    protobuf BYTEA NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE device_grants (
    device_id TEXT NOT NULL REFERENCES devices(id),
    user_id TEXT NOT NULL REFERENCES users(id),
    desktop BOOLEAN NOT NULL DEFAULT TRUE,
    vnc BOOLEAN NOT NULL DEFAULT TRUE,
    ssh BOOLEAN NOT NULL DEFAULT TRUE,
    write_control BOOLEAN NOT NULL DEFAULT TRUE,
    PRIMARY KEY (device_id, user_id)
);
CREATE TABLE audit_events (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id TEXT REFERENCES users(id),
    device_id TEXT REFERENCES devices(id),
    event_type TEXT NOT NULL,
    success BOOLEAN NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
