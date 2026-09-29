CREATE TABLE connection_verifications (
    id VARCHAR(128) PRIMARY KEY,
    user_id VARCHAR(128) NOT NULL REFERENCES users(id),
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id),
    login_token_hash CHAR(64) NOT NULL,
    write_control BOOLEAN NOT NULL,
    grant_revision BIGINT NOT NULL,
    requester_telegram_id BIGINT NOT NULL,
    bot_generation TEXT NOT NULL,
    state VARCHAR(16) NOT NULL CHECK(state IN ('pending','approved','denied','consumed','cancelled','failed')),
    created_at DATETIME(6) NOT NULL DEFAULT now(6),
    expires_at DATETIME(6) NOT NULL,
    decided_at DATETIME(6),
    pending_device VARCHAR(128) GENERATED ALWAYS AS (CASE WHEN state='pending' THEN device_id ELSE NULL END) PERSISTENT,
    UNIQUE KEY connection_verifications_pending(user_id,pending_device),
    KEY connection_verifications_user(user_id,created_at DESC),
    CONSTRAINT verifications_control_only CHECK(write_control OR state NOT IN ('pending','approved'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_nopad_bin;
INSERT INTO connection_verifications(id,user_id,device_id,login_token_hash,write_control,grant_revision,requester_telegram_id,bot_generation,state,created_at,expires_at,decided_at)
SELECT id,user_id,device_id,login_token_hash,write_control,grant_revision,requester_telegram_id,bot_generation,CASE WHEN state IN ('pending','approved') THEN 'cancelled' ELSE state END,created_at,expires_at,decided_at FROM connection_approvals;
DROP TABLE connection_approvals;
UPDATE device_grants SET revision=NEXT VALUE FOR grant_revision_sequence WHERE verification_method='telegram';
