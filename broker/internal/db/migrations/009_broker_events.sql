CREATE TABLE IF NOT EXISTS broker_events(
  user_sub TEXT NOT NULL REFERENCES broker_users(sub) ON DELETE CASCADE,
  event_id TEXT NOT NULL,
  event_type TEXT NOT NULL,
  ts_unix_ms BIGINT NOT NULL,
  event_json JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY(user_sub, event_id)
);

CREATE INDEX IF NOT EXISTS idx_broker_events_user_ts ON broker_events(user_sub, ts_unix_ms DESC);
