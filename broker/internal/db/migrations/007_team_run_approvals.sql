CREATE TABLE IF NOT EXISTS broker_team_run_approvals(
  approval_id TEXT PRIMARY KEY,
  team_run_id TEXT NOT NULL REFERENCES broker_team_runs(team_run_id) ON DELETE CASCADE,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  rule_id TEXT NOT NULL REFERENCES broker_team_quorum_rules(rule_id) ON DELETE CASCADE,
  member_id TEXT NOT NULL REFERENCES broker_team_members(member_id) ON DELETE CASCADE,
  role TEXT NOT NULL,
  decision TEXT NOT NULL,
  reason TEXT,
  created_by TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  UNIQUE(team_run_id, rule_id, member_id)
);

CREATE INDEX IF NOT EXISTS idx_broker_team_run_approvals_team_run_id ON broker_team_run_approvals(team_run_id);
