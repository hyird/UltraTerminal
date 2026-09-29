ALTER TABLE device_grants ADD COLUMN system_restart BOOLEAN NOT NULL DEFAULT FALSE;
CREATE TABLE restart_operations (
    operation_id TEXT PRIMARY KEY CHECK (operation_id ~ '^[0-9a-f]{64}$'),
    idempotency_key TEXT NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 128),
    user_id TEXT NOT NULL REFERENCES users(id),
    device_id TEXT NOT NULL REFERENCES devices(id),
    status TEXT NOT NULL CHECK (status IN ('PENDING', 'ACCEPTED', 'FAILED', 'UNKNOWN', 'COMPLETED')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    result_status TEXT CHECK (result_status IS NULL OR result_status IN ('ACCEPTED', 'FAILED', 'UNKNOWN')),
    win32_error INTEGER,
    boot_time_unix_seconds BIGINT CHECK (boot_time_unix_seconds IS NULL OR boot_time_unix_seconds >= 0),
    UNIQUE (user_id, idempotency_key)
);
CREATE UNIQUE INDEX restart_operations_one_pending_per_device
    ON restart_operations(device_id) WHERE status IN ('PENDING', 'ACCEPTED', 'UNKNOWN');
CREATE INDEX restart_operations_device_created ON restart_operations(device_id, created_at DESC);
