CREATE TABLE IF NOT EXISTS broker_guidance_events(
  guidance_id TEXT PRIMARY KEY,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  team_run_id TEXT REFERENCES broker_team_runs(team_run_id) ON DELETE SET NULL,
  kind TEXT NOT NULL,
  priority TEXT NOT NULL,
  message TEXT NOT NULL,
  payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  target_roles JSONB NOT NULL DEFAULT '[]'::jsonb,
  target_member_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
  target_agent_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
  target_orchestrator_id TEXT,
  created_by TEXT,
  created_sub TEXT,
  created_unix_ms BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW()) * 1000),
  expires_unix_ms BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL,
  acked_by TEXT,
  acked_unix_ms BIGINT NOT NULL DEFAULT 0,
  ack_note TEXT
);

CREATE TABLE IF NOT EXISTS broker_guidance_receipts(
  id BIGSERIAL PRIMARY KEY,
  guidance_id TEXT NOT NULL REFERENCES broker_guidance_events(guidance_id) ON DELETE CASCADE,
  ack_by TEXT NOT NULL,
  ack_role TEXT,
  ack_source TEXT NOT NULL,
  ack_note TEXT,
  acked_unix_ms BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW()) * 1000)
);

CREATE INDEX IF NOT EXISTS idx_broker_guidance_team_run ON broker_guidance_events(team_id, team_run_id);
CREATE INDEX IF NOT EXISTS idx_broker_guidance_status ON broker_guidance_events(team_id, status, created_unix_ms DESC);
CREATE INDEX IF NOT EXISTS idx_broker_guidance_created ON broker_guidance_events(team_id, created_unix_ms DESC);
