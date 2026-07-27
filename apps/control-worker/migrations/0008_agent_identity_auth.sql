-- Registration Tokens are bearer capabilities only for the first identity-binding request.
-- The raw token never enters D1: only its SHA-256 hash is persisted.
CREATE TABLE IF NOT EXISTS registration_tokens (
  id TEXT PRIMARY KEY,
  token_hash TEXT NOT NULL UNIQUE,
  expires_at TEXT NOT NULL,
  consumed_at TEXT,
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS registration_tokens_expiry_idx
ON registration_tokens(expires_at);

-- A node's Ed25519 public key is its durable identity. Existing installations are intentionally
-- left NULL so they can be re-enrolled with an administrator-issued one-time token.
ALTER TABLE backend_registrations ADD COLUMN public_key TEXT;

-- One nonce per valid signed Agent request prevents a captured request from being replayed within
-- the allowed clock-skew window. The Worker Cron removes expired values.
CREATE TABLE IF NOT EXISTS agent_request_nonces (
  backend_id TEXT NOT NULL,
  nonce TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  PRIMARY KEY (backend_id, nonce)
);

CREATE INDEX IF NOT EXISTS agent_request_nonces_expiry_idx
ON agent_request_nonces(expires_at);
