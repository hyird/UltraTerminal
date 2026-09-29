ALTER TABLE restart_operations DROP CONSTRAINT restart_operations_status_check;
ALTER TABLE restart_operations ADD CONSTRAINT restart_operations_status_check CHECK (status IN ('PENDING', 'ACCEPTED', 'FAILED', 'UNKNOWN', 'COMPLETED', 'RESOLVED'));
ALTER TABLE restart_operations ADD COLUMN resolved_by TEXT REFERENCES users(id);
ALTER TABLE restart_operations ADD COLUMN resolved_at TIMESTAMPTZ;
ALTER TABLE restart_operations ADD CONSTRAINT restart_operations_resolution_check CHECK ((status = 'RESOLVED') = (resolved_by IS NOT NULL AND resolved_at IS NOT NULL));
