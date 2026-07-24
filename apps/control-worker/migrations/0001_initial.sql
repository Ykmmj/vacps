CREATE TABLE IF NOT EXISTS backends (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  base_url TEXT NOT NULL,
  region TEXT,
  tags_json TEXT NOT NULL DEFAULT '[]',
  enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
  last_status TEXT,
  last_checked_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS tasks (
  id TEXT PRIMARY KEY,
  backend_id TEXT NOT NULL REFERENCES backends(id),
  type TEXT NOT NULL CHECK (type IN ('shell', 'agent')),
  source TEXT NOT NULL CHECK (source IN ('mcp', 'web', 'schedule', 'api')),
  profile TEXT NOT NULL,
  summary TEXT,
  status TEXT NOT NULL,
  schedule_id TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  finished_at TEXT
);
CREATE INDEX IF NOT EXISTS tasks_backend_created_idx ON tasks(backend_id, created_at DESC);
CREATE INDEX IF NOT EXISTS tasks_status_idx ON tasks(status, updated_at DESC);

CREATE TABLE IF NOT EXISTS schedules (
  id TEXT PRIMARY KEY,
  backend_id TEXT NOT NULL REFERENCES backends(id),
  name TEXT NOT NULL,
  cron TEXT NOT NULL,
  timezone TEXT NOT NULL,
  task_template_json TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
  last_run_at TEXT,
  next_run_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS schedules_backend_idx ON schedules(backend_id, enabled);

CREATE TABLE IF NOT EXISTS profiles (
  id TEXT PRIMARY KEY,
  display_name TEXT NOT NULL,
  config_json TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS audit_events (
  id TEXT PRIMARY KEY,
  request_id TEXT NOT NULL,
  action TEXT NOT NULL,
  subject TEXT,
  resource_type TEXT NOT NULL,
  resource_id TEXT,
  metadata_json TEXT,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS audit_events_created_idx ON audit_events(created_at DESC);

INSERT OR IGNORE INTO profiles (id, display_name, config_json, enabled, created_at, updated_at)
VALUES ('full', 'Full access', '{"id":"full","description":"Allows all commands in v1."}', 1, datetime('now'), datetime('now'));
