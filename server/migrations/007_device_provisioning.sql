ALTER TABLE devices ADD COLUMN enrolled BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE enrollment_codes ADD COLUMN device_id TEXT REFERENCES devices(id);
CREATE INDEX enrollment_codes_device ON enrollment_codes(device_id);
-- Previously issued codes did not identify a device. Require a new per-device
-- command after this migration; existing enrolled devices keep their identity.
UPDATE enrollment_codes SET expires_at=now() WHERE consumed_at IS NULL;
