-- Retention policy columns: skip auto-cleanup when held/pinned.
ALTER TABLE tasks ADD COLUMN legal_hold INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN pinned_at TEXT;
ALTER TABLE tasks ADD COLUMN output_expires_at TEXT;

-- Optional index for protected skips on purge scans.
CREATE INDEX IF NOT EXISTS idx_tasks_purge_eligible
ON tasks(expires_at, deleted_at, legal_hold)
WHERE deleted_at IS NULL;
