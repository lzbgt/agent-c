-- Broker schema (Postgres)

CREATE TABLE IF NOT EXISTS broker_users(
  sub TEXT PRIMARY KEY,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS broker_agents(
  agent_id TEXT PRIMARY KEY,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  owner_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  enabled BOOLEAN NOT NULL DEFAULT true,
  labels JSONB NOT NULL DEFAULT '{}'::jsonb,
  meta JSONB NOT NULL DEFAULT '{}'::jsonb
);

CREATE TABLE IF NOT EXISTS broker_agent_memberships(
  agent_id TEXT NOT NULL REFERENCES broker_agents(agent_id) ON DELETE CASCADE,
  user_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  role TEXT NOT NULL DEFAULT 'user',
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY(agent_id, user_sub)
);

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

CREATE TABLE IF NOT EXISTS broker_agent_connections(
  id BIGSERIAL PRIMARY KEY,
  agent_id TEXT NOT NULL REFERENCES broker_agents(agent_id) ON DELETE CASCADE,
  connected_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  disconnected_at TIMESTAMPTZ,
  remote_addr TEXT NOT NULL DEFAULT '',
  last_seen TIMESTAMPTZ NOT NULL DEFAULT now(),
  meta JSONB NOT NULL DEFAULT '{}'::jsonb
);

CREATE INDEX IF NOT EXISTS broker_agent_connections_by_agent ON broker_agent_connections(agent_id, connected_at DESC);

CREATE TABLE IF NOT EXISTS broker_relay_audit(
  id BIGSERIAL PRIMARY KEY,
  ts TIMESTAMPTZ NOT NULL DEFAULT now(),
  user_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  agent_id TEXT NOT NULL REFERENCES broker_agents(agent_id) ON DELETE CASCADE,
  method TEXT NOT NULL DEFAULT '',
  path TEXT NOT NULL DEFAULT '',
  status INT NOT NULL DEFAULT 0,
  latency_ms INT NOT NULL DEFAULT 0,
  error TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS broker_relay_audit_by_user ON broker_relay_audit(user_sub, ts DESC);
CREATE INDEX IF NOT EXISTS broker_relay_audit_by_agent ON broker_relay_audit(agent_id, ts DESC);
