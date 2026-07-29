-- Task retention / cleanup (Phase 0–1)
ALTER TABLE tasks ADD COLUMN terminal_at TEXT;
ALTER TABLE tasks ADD COLUMN expires_at TEXT;
ALTER TABLE tasks ADD COLUMN labels_json TEXT;
ALTER TABLE tasks ADD COLUMN environment TEXT;
ALTER TABLE tasks ADD COLUMN retention_class TEXT;
ALTER TABLE tasks ADD COLUMN deleted_at TEXT;
ALTER TABLE tasks ADD COLUMN deleted_by TEXT;
ALTER TABLE tasks ADD COLUMN deletion_reason TEXT;
ALTER TABLE tasks ADD COLUMN cleanup_state TEXT NOT NULL DEFAULT 'none';

-- Backfill terminal_at / expires_at for existing finished rows (conservative 30d from finished_at).
UPDATE tasks
SET terminal_at = finished_at,
    expires_at = datetime(finished_at, '+30 days'),
    cleanup_state = 'eligible'
WHERE finished_at IS NOT NULL
  AND terminal_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_tasks_expires_at ON tasks(expires_at);
CREATE INDEX IF NOT EXISTS idx_tasks_deleted_at ON tasks(deleted_at);
CREATE INDEX IF NOT EXISTS idx_tasks_environment ON tasks(environment);
CREATE INDEX IF NOT EXISTS idx_tasks_cleanup ON tasks(cleanup_state, expires_at);
