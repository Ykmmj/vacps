CREATE TABLE IF NOT EXISTS managed_tunnels (
  backend_id TEXT PRIMARY KEY,
  tunnel_id TEXT NOT NULL UNIQUE,
  hostname TEXT NOT NULL UNIQUE,
  dns_record_id TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL
);
