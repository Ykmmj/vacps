-- Per-task agent output TTL (from create payload output.retention_seconds).
ALTER TABLE tasks ADD COLUMN output_retention_seconds INTEGER;
