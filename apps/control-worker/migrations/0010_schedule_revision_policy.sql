-- Schema v2: schedule revision for optimistic concurrency + policy + idempotency
ALTER TABLE schedules ADD COLUMN revision INTEGER NOT NULL DEFAULT 1;
ALTER TABLE schedules ADD COLUMN policy_json TEXT NOT NULL DEFAULT '{"concurrency":"forbid","misfire":"run_once","max_catchup_runs":1}';
ALTER TABLE schedules ADD COLUMN idempotency_key TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS schedules_backend_idempotency_idx
  ON schedules(backend_id, idempotency_key)
  WHERE idempotency_key IS NOT NULL;
