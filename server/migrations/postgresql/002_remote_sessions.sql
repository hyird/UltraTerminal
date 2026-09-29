CREATE TABLE remote_sessions (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id),
    device_id TEXT NOT NULL REFERENCES devices(id),
    kind INTEGER NOT NULL CHECK (kind IN (1,2,3)),
    write_control BOOLEAN NOT NULL,
    operating_system_user TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','active','closed')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    setup_expires_at TIMESTAMPTZ NOT NULL,
    ended_at TIMESTAMPTZ,
    end_reason TEXT,
    CHECK ((state='closed') = (ended_at IS NOT NULL))
);
CREATE UNIQUE INDEX remote_sessions_writer ON remote_sessions(device_id)
    WHERE kind IN (1,2) AND write_control AND state<>'closed';
CREATE INDEX remote_sessions_user ON remote_sessions(user_id) WHERE state<>'closed';
CREATE INDEX remote_sessions_device ON remote_sessions(device_id) WHERE state<>'closed';
CREATE TABLE session_channels (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES remote_sessions(id),
    purpose INTEGER NOT NULL CHECK (purpose IN (2,3,4,5)),
    browser_ticket_hash TEXT NOT NULL UNIQUE,
    agent_ticket_hash TEXT NOT NULL UNIQUE,
    expires_at TIMESTAMPTZ NOT NULL,
    browser_consumed_at TIMESTAMPTZ,
    agent_consumed_at TIMESTAMPTZ,
    UNIQUE(session_id,purpose)
);
ALTER TABLE audit_events ADD COLUMN session_id TEXT REFERENCES remote_sessions(id);
ALTER TABLE audit_events ADD COLUMN reason TEXT;
