CREATE TABLE IF NOT EXISTS broker_orchestrator_runs(
  orchestrator_run_id TEXT PRIMARY KEY,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  status TEXT NOT NULL,
  goal TEXT,
  goal_contract JSONB NOT NULL DEFAULT '{}'::jsonb,
  role_plan_snapshot JSONB NOT NULL DEFAULT '{}'::jsonb,
  meta JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_by TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  last_heartbeat_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_broker_orchestrator_runs_team_id ON broker_orchestrator_runs(team_id);
CREATE INDEX IF NOT EXISTS idx_broker_orchestrator_runs_status ON broker_orchestrator_runs(status);
