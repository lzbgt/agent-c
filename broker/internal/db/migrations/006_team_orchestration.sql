CREATE TABLE IF NOT EXISTS broker_teams(
  team_id TEXT PRIMARY KEY,
  owner_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  display_name TEXT NOT NULL,
  tags JSONB NOT NULL DEFAULT '[]'::jsonb,
  policy_ref TEXT,
  shared_memory_scope_id TEXT,
  meta JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_broker_teams_owner_sub ON broker_teams(owner_sub);

CREATE TABLE IF NOT EXISTS broker_team_members(
  member_id TEXT PRIMARY KEY,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  deployment_id TEXT,
  agent_id TEXT,
  role TEXT NOT NULL,
  capabilities JSONB NOT NULL DEFAULT '[]'::jsonb,
  status TEXT NOT NULL DEFAULT 'active',
  weight INTEGER NOT NULL DEFAULT 0,
  meta JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_broker_team_members_team_id ON broker_team_members(team_id);

CREATE TABLE IF NOT EXISTS broker_team_quorum_rules(
  rule_id TEXT PRIMARY KEY,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  action TEXT NOT NULL,
  tool_names JSONB NOT NULL DEFAULT '[]'::jsonb,
  min_approvals INTEGER NOT NULL,
  role_allowlist JSONB NOT NULL DEFAULT '[]'::jsonb,
  require_distinct_roles BOOLEAN NOT NULL DEFAULT FALSE,
  timeout_ms BIGINT NOT NULL DEFAULT 0,
  quorum_mode TEXT NOT NULL,
  meta JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_broker_team_quorum_rules_team_id ON broker_team_quorum_rules(team_id);

CREATE TABLE IF NOT EXISTS broker_team_runs(
  team_run_id TEXT PRIMARY KEY,
  team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
  status TEXT NOT NULL,
  created_by TEXT,
  run_json JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_broker_team_runs_team_id ON broker_team_runs(team_id);
