-- Add trace_id to relay audit for cross-system correlation.
ALTER TABLE IF EXISTS broker_relay_audit
  ADD COLUMN IF NOT EXISTS trace_id TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS broker_relay_audit_by_trace ON broker_relay_audit(trace_id);

