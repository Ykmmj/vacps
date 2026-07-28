-- Task idempotency: store canonical request hash for conflict detection.
ALTER TABLE tasks ADD COLUMN request_hash TEXT;
ALTER TABLE tasks ADD COLUMN name TEXT;
