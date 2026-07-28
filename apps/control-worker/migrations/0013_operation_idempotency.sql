-- Generic control-plane operation idempotency (e.g. schedules.delete replay).
CREATE TABLE IF NOT EXISTS operation_idempotency (
  scope TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  request_hash TEXT NOT NULL,
  result_json TEXT NOT NULL,
  created_at TEXT NOT NULL,
  PRIMARY KEY (scope, idempotency_key)
);
