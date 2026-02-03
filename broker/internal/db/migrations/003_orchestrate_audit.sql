-- Persist orchestrate summaries keyed by trace_id for correlated debugging.

CREATE TABLE IF NOT EXISTS broker_orchestrate_audit(
  id BIGSERIAL PRIMARY KEY,
  ts TIMESTAMPTZ NOT NULL DEFAULT now(),
  user_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  trace_id TEXT NOT NULL DEFAULT '',
  request_json JSONB NOT NULL DEFAULT '{}'::jsonb,
  response_json JSONB NOT NULL DEFAULT '{}'::jsonb
);

CREATE INDEX IF NOT EXISTS broker_orchestrate_audit_by_user ON broker_orchestrate_audit(user_sub, ts DESC, id DESC);
CREATE INDEX IF NOT EXISTS broker_orchestrate_audit_by_trace ON broker_orchestrate_audit(trace_id, ts DESC, id DESC);

