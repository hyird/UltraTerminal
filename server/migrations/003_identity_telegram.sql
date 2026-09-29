CREATE TABLE identity_telegram_verifications (
 id CHAR(32) PRIMARY KEY,
 user_id VARCHAR(128) NOT NULL,
 login_token_hash CHAR(64) NOT NULL,
 telegram_id BIGINT NOT NULL,
 bot_generation VARCHAR(64) NOT NULL,
 state VARCHAR(16) NOT NULL DEFAULT 'pending',
 created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
 expires_at DATETIME(6) NOT NULL,
 INDEX identity_telegram_user(user_id, created_at),
 CHECK (state IN ('pending','approved','denied','consumed','failed'))
);
