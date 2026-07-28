-- Schema v3 naming: store schedule task payload as task_json (not task_template).
-- Public MCP API already uses trigger/policy/task; this aligns the D1 column name.
ALTER TABLE schedules RENAME COLUMN task_template_json TO task_json;
