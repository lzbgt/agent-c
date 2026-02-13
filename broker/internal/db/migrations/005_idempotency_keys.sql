-- Idempotency keys for proxy/orchestrate retries.

CREATE TABLE IF NOT EXISTS broker_idempotency_keys(
  id BIGSERIAL PRIMARY KEY,
  ts TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at TIMESTAMPTZ NOT NULL,
  user_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  idempotency_key TEXT NOT NULL,
  request_sha256 TEXT NOT NULL,
  method TEXT NOT NULL DEFAULT '',
  path TEXT NOT NULL DEFAULT '',
  query TEXT NOT NULL DEFAULT '',
  agent_id TEXT NOT NULL DEFAULT '',
  completed BOOLEAN NOT NULL DEFAULT false,
  response_status INT NOT NULL DEFAULT 0,
  response_headers JSONB NOT NULL DEFAULT '{}'::jsonb,
  response_body BYTEA NOT NULL DEFAULT ''::bytea
);

CREATE UNIQUE INDEX IF NOT EXISTS broker_idempotency_keys_unique ON broker_idempotency_keys(user_sub, idempotency_key);
CREATE INDEX IF NOT EXISTS broker_idempotency_keys_expires ON broker_idempotency_keys(expires_at);
CREATE INDEX IF NOT EXISTS broker_idempotency_keys_user ON broker_idempotency_keys(user_sub, ts DESC);
