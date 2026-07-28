-- Schedule request hash for idempotency_conflict; task public kind for create_command.
ALTER TABLE schedules ADD COLUMN request_hash TEXT;
ALTER TABLE tasks ADD COLUMN kind TEXT;
