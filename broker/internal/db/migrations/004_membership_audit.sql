-- Add membership audit trail for agent membership changes.
CREATE TABLE IF NOT EXISTS broker_agent_membership_audit(
  id BIGSERIAL PRIMARY KEY,
  ts TIMESTAMPTZ NOT NULL DEFAULT now(),
  actor_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  agent_id TEXT NOT NULL REFERENCES broker_agents(agent_id) ON DELETE CASCADE,
  target_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  action TEXT NOT NULL DEFAULT '',
  role TEXT NOT NULL DEFAULT '',
  trace_id TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS broker_agent_membership_audit_by_agent ON broker_agent_membership_audit(agent_id, ts DESC);
CREATE INDEX IF NOT EXISTS broker_agent_membership_audit_by_actor ON broker_agent_membership_audit(actor_sub, ts DESC);
CREATE INDEX IF NOT EXISTS broker_agent_membership_audit_by_target ON broker_agent_membership_audit(target_sub, ts DESC);
CREATE INDEX IF NOT EXISTS broker_agent_membership_audit_by_trace ON broker_agent_membership_audit(trace_id);
