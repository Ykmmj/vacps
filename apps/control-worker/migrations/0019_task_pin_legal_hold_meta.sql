-- Metadata for pin / legal-hold operators (columns legal_hold + pinned_at already in 0018).
ALTER TABLE tasks ADD COLUMN pinned_by TEXT;
ALTER TABLE tasks ADD COLUMN legal_hold_reason TEXT;
ALTER TABLE tasks ADD COLUMN legal_hold_at TEXT;
ALTER TABLE tasks ADD COLUMN legal_hold_by TEXT;
