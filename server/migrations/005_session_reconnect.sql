ALTER TABLE remote_sessions
    ADD COLUMN reconnect_until DATETIME(6) NULL,
    ADD COLUMN reconnected_as VARCHAR(128) NULL;
