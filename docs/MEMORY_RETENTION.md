# Memory Retention Policy (v1)

This document defines the **deterministic, operator-controlled** retention policy for durable memory on disk.
The goal is to keep memory growth bounded without relying on LLM summarization or lossy heuristics.

## Goals

- Bound disk usage for daily memory logs and structured checkpoints.
- Provide a **dry-run** mode so operators can preview deletions.
- Support both **on-demand** enforcement and **background** enforcement.
- Be deterministic and reversible (by restoring files from backups, if available).

## Non-goals

- No LLM-based summarization, salience scoring, or semantic pruning in v1.
- No mutation of `STRUCTURED.md` contents (facts stay intact in v1).
- No cross-agent retention coordination (per-agentd only).

## Policy (v1)

Retention is applied to two surfaces:

1) **Daily logs** — `state_dir/memory/YYYY-MM-DD.md`
   - **Age bound**: keep at most `daily_max_days` of daily files (older files deleted first).
   - **Size bound**: keep total daily bytes under `daily_max_bytes` (delete oldest until within budget).

2) **Structured checkpoints** — `state_dir/memory/checkpoints/structured_*.json`
   - **Age bound**: delete checkpoints older than `checkpoint_max_days`.
   - **Count bound**: keep at most `checkpoint_max_count` (delete oldest first).

Retention is **best-effort** and never touches core memory files (`MEMORY.md`, `STRUCTURED.md`).

## Configuration

Config knobs (all optional; `0` disables each bound):

- `memory_retention_interval_ms` — background enforcement interval (0 = disabled)
- `memory_retention_daily_max_days`
- `memory_retention_daily_max_bytes`
- `memory_retention_checkpoint_max_days`
- `memory_retention_checkpoint_max_count`

These can be set via flags, env vars, or `/api/v1/config/update` and are reported by `/api/v1/config` and `/api/v1/caps`.

## API

### `POST /api/v1/memory/retention/enforce`

Request body (all fields optional):

```json
{
  "dry_run": true,
  "daily_max_days": 30,
  "daily_max_bytes": 104857600,
  "checkpoint_max_days": 30,
  "checkpoint_max_count": 200
}
```

Response includes counts and deletions:

```json
{
  "ok": true,
  "dry_run": true,
  "daily_deleted": ["2025-12-01.md"],
  "daily_deleted_count": 1,
  "checkpoint_deleted": ["checkpoints/structured_2025-12-01T00:00:00Z.json"],
  "checkpoint_deleted_count": 1,
  "daily_bytes_before": 12345,
  "daily_bytes_after": 6789
}
```

## Background enforcement

When `memory_retention_interval_ms > 0`, the daemon periodically applies the policy
using the configured limits. This is **best-effort** and never blocks request handling.

## Future work

- Salience/decay policies for structured memory (requires explicit scoring metadata).
- Archive tier for memory artifacts with restore workflows.
- Operator-visible retention audit trail.

## References

- Source snapshot: `ref/claude-mem` (see `.source.json` for upstream metadata).
