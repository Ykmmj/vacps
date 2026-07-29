-- Schema v3: public kind is command|shell|agent. Legacy CHECK only allowed shell|agent on type.
-- SQLite cannot ALTER CHECK in place; rebuild tasks with relaxed type + kind constraints.
-- Data is preserved. New inserts store type=shell for command|shell and type=agent for agent.

CREATE TABLE IF NOT EXISTS tasks_v3 (
  id TEXT PRIMARY KEY,
  backend_id TEXT NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('shell', 'agent', 'command')),
  kind TEXT,
  source TEXT NOT NULL CHECK (source IN ('mcp', 'web', 'schedule', 'api')),
  profile TEXT NOT NULL,
  name TEXT,
  summary TEXT,
  status TEXT NOT NULL,
  schedule_id TEXT,
  idempotency_key TEXT,
  request_hash TEXT,
  retry_of_task_id TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  finished_at TEXT
);

INSERT INTO tasks_v3 (
  id, backend_id, type, kind, source, profile, name, summary, status, schedule_id,
  idempotency_key, request_hash, retry_of_task_id, created_at, updated_at, finished_at
)
SELECT
  id,
  backend_id,
  CASE
    WHEN type IN ('shell', 'agent', 'command') THEN type
    ELSE 'shell'
  END,
  COALESCE(kind, type),
  source,
  profile,
  name,
  summary,
  status,
  schedule_id,
  idempotency_key,
  request_hash,
  retry_of_task_id,
  created_at,
  updated_at,
  finished_at
FROM tasks;

DROP TABLE tasks;
ALTER TABLE tasks_v3 RENAME TO tasks;

CREATE INDEX IF NOT EXISTS tasks_backend_created_idx ON tasks(backend_id, created_at DESC);
CREATE INDEX IF NOT EXISTS tasks_status_idx ON tasks(status, updated_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS tasks_backend_idempotency_uidx
  ON tasks(backend_id, idempotency_key)
  WHERE idempotency_key IS NOT NULL;
