CREATE TABLE IF NOT EXISTS broker_client_prefs(
  id BIGSERIAL PRIMARY KEY,
  owner_sub TEXT NOT NULL,
  client_kind TEXT NOT NULL,
  client_id TEXT NOT NULL,
  version INTEGER NOT NULL DEFAULT 1,
  prefs JSONB NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  UNIQUE(owner_sub, client_kind, client_id)
);

CREATE INDEX IF NOT EXISTS broker_client_prefs_owner_idx ON broker_client_prefs(owner_sub);
