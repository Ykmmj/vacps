-- Create-idempotency tombstones survive hard delete of the task index row.
CREATE TABLE IF NOT EXISTS task_create_idempotency (
  backend_id TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  request_hash TEXT NOT NULL,
  task_id TEXT,
  task_deleted INTEGER NOT NULL DEFAULT 0 CHECK (task_deleted IN (0, 1)),
  original_status TEXT,
  original_created_at TEXT,
  created_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  PRIMARY KEY (backend_id, idempotency_key)
);

CREATE INDEX IF NOT EXISTS idx_task_create_idempotency_expires
ON task_create_idempotency(expires_at);

-- Backfill live keys so hard-delete of existing rows can still leave a tombstone later.
INSERT OR IGNORE INTO task_create_idempotency (
  backend_id, idempotency_key, request_hash, task_id, task_deleted,
  original_status, original_created_at, created_at, expires_at
)
SELECT
  backend_id,
  idempotency_key,
  COALESCE(request_hash, ''),
  id,
  CASE WHEN deleted_at IS NOT NULL THEN 1 ELSE 0 END,
  status,
  created_at,
  created_at,
  datetime(created_at, '+30 days')
FROM tasks
WHERE idempotency_key IS NOT NULL;
