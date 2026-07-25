CREATE TABLE IF NOT EXISTS cloudflare_oauth_connections (
  id TEXT PRIMARY KEY CHECK (id = 'default'),
  account_id TEXT NOT NULL,
  zone_id TEXT NOT NULL,
  base_domain TEXT NOT NULL,
  access_token_ciphertext TEXT NOT NULL,
  refresh_token_ciphertext TEXT,
  expires_at TEXT,
  scopes TEXT,
  connected_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS cloudflare_oauth_states (
  state TEXT PRIMARY KEY,
  account_id TEXT NOT NULL,
  zone_id TEXT NOT NULL,
  base_domain TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS cloudflare_oauth_states_expires_idx
ON cloudflare_oauth_states(expires_at);
