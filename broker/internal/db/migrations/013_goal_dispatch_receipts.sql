-- Broker-owned admission identity is immutable. Only the NULL -> team-run
-- receipt transition is exposed by the application; callers cannot fabricate it.
CREATE TABLE IF NOT EXISTS broker_goal_dispatches(
 team_id TEXT NOT NULL REFERENCES broker_teams(team_id) ON DELETE CASCADE,
 orchestrator_run_id TEXT NOT NULL REFERENCES broker_orchestrator_runs(orchestrator_run_id) ON DELETE CASCADE,
 dispatch_id TEXT NOT NULL,
 agent_id TEXT NOT NULL,
 deployment_id TEXT NOT NULL,
 session_id TEXT NOT NULL,
 ledger_id TEXT NOT NULL,
 revision BIGINT NOT NULL CHECK (revision BETWEEN 1 AND 9007199254740991),
 digest TEXT NOT NULL CHECK (digest ~ '^[0-9a-f]{64}$'),
 control_digest TEXT NOT NULL CHECK (control_digest ~ '^[0-9a-f]{64}$'),
 team_run_id TEXT UNIQUE REFERENCES broker_team_runs(team_run_id),
 admitted_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
 recorded_at TIMESTAMPTZ,
 PRIMARY KEY(team_id, orchestrator_run_id, dispatch_id),
 CHECK ((team_run_id IS NULL) = (recorded_at IS NULL))
);
