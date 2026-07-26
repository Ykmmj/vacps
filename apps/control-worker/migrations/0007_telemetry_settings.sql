CREATE TABLE IF NOT EXISTS control_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

INSERT OR IGNORE INTO control_settings (key, value, updated_at)
VALUES ('telemetry_interval_seconds', '120', datetime('now'));
