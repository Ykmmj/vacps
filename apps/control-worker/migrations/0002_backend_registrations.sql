CREATE TABLE IF NOT EXISTS backend_registrations (
  id TEXT PRIMARY KEY,
  backend_id TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL,
  base_url TEXT NOT NULL,
  region TEXT,
  tags_json TEXT NOT NULL DEFAULT '[]',
  agent_version TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'approved', 'rejected')) DEFAULT 'pending',
  rejection_reason TEXT,
  requested_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  decision_at TEXT
);

CREATE INDEX IF NOT EXISTS backend_registrations_status_requested_idx
ON backend_registrations(status, requested_at DESC);
