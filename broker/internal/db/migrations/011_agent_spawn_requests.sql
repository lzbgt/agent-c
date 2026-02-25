CREATE TABLE IF NOT EXISTS broker_agent_spawn_requests(
  spawn_request_id TEXT PRIMARY KEY,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  orchestrator_run_id TEXT REFERENCES broker_orchestrator_runs(orchestrator_run_id) ON DELETE SET NULL,
  role TEXT NOT NULL,
  count INTEGER NOT NULL DEFAULT 1 CHECK (count > 0),
  status TEXT NOT NULL,
  requirements JSONB NOT NULL DEFAULT '{}'::jsonb,
  assigned_members JSONB NOT NULL DEFAULT '[]'::jsonb,
  error TEXT,
  meta JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_by TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_broker_spawn_requests_team_id ON broker_agent_spawn_requests(team_id);
CREATE INDEX IF NOT EXISTS idx_broker_spawn_requests_status ON broker_agent_spawn_requests(status);
CREATE INDEX IF NOT EXISTS idx_broker_spawn_requests_orun ON broker_agent_spawn_requests(orchestrator_run_id);
