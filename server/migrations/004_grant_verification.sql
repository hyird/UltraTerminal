ALTER TABLE device_grants ADD COLUMN verification_method VARCHAR(16) NOT NULL DEFAULT 'direct';
UPDATE device_grants SET verification_method='telegram' WHERE require_approval;
ALTER TABLE device_grants DROP COLUMN require_approval;
ALTER TABLE device_grants ADD CONSTRAINT grants_verification_method CHECK(verification_method IN ('direct','telegram','totp'));
