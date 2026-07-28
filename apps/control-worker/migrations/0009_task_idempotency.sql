-- Optional client idempotency key. Uniqueness is scoped per backend.
ALTER TABLE tasks ADD COLUMN idempotency_key TEXT;
ALTER TABLE tasks ADD COLUMN retry_of_task_id TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS tasks_backend_idempotency_uidx
ON tasks(backend_id, idempotency_key)
WHERE idempotency_key IS NOT NULL;
