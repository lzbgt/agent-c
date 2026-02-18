# Memory Retention Policy (v1)

This document defines the **deterministic, operator-controlled** retention policy for durable memory on disk.
The goal is to keep memory growth bounded without relying on LLM summarization or lossy heuristics.

## Goals

- Bound disk usage for daily memory logs and structured checkpoints.
- Provide a **dry-run** mode so operators can preview deletions.
- Support both **on-demand** enforcement and **background** enforcement.
- Be deterministic and reversible (by restoring files from backups, if available).
- Allow structured memory facts to be **deprecated** when they age out of policy.
- Provide a broker fan-out path so multi-deployment fleets can enforce retention together.
- Enable optional LLM-driven memory recaps as a complementary, operator-controlled surface.

## Promoted goals (formerly non-goals)

These are now explicit goals and implemented surfaces:

- LLM-based recaps (`/api/v1/memory/recaps`) as an operator-triggered summarization surface.
- Structured memory deprecation for aging entries (policy-driven, deterministic).
- Broker fan-out for multi-deployment retention enforcement.

## Policy (v1)

Retention is applied to two surfaces:

1) **Daily logs** — `state_dir/memory/YYYY-MM-DD.md`
   - **Age bound**: keep at most `daily_max_days` of daily files (older files deleted first).
   - **Size bound**: keep total daily bytes under `daily_max_bytes` (delete oldest until within budget).

2) **Structured checkpoints** — `state_dir/memory/checkpoints/structured_*.json`
   - **Age bound**: delete checkpoints older than `checkpoint_max_days`.
   - **Count bound**: keep at most `checkpoint_max_count` (delete oldest first).

3) **Structured memory deprecation** — `state_dir/memory/STRUCTURED.md` (optional)
   - **Age bound**: entries whose `observed_utc`/`updated_utc` is older than `structured_deprecate_days`
     are upserted with `status="deprecated"`.
   - **Cap**: only deprecate up to `structured_deprecate_max_entries` per run (bounded, deterministic).
   - **Implementation**: uses `memory_put(entries=[...])` to keep structured evidence consistent.

Retention is **best-effort** and never deletes core memory files (`MEMORY.md`, `STRUCTURED.md`).

## Configuration

Config knobs (all optional; `0` disables each bound):

- `memory_retention_interval_ms` — background enforcement interval (0 = disabled)
- `memory_retention_daily_max_days`
- `memory_retention_daily_max_bytes`
- `memory_retention_checkpoint_max_days`
- `memory_retention_checkpoint_max_count`
- `memory_retention_structured_deprecate_days`
- `memory_retention_structured_deprecate_max_entries`

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
  "checkpoint_max_count": 200,
  "structured_deprecate_days": 90,
  "structured_deprecate_max_entries": 50
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
  "structured_deprecated_count": 3,
  "structured_deprecated_keys": ["user.pref.language", "ops.retention.override"],
  "daily_bytes_before": 12345,
  "daily_bytes_after": 6789
}
```

## Broker fan-out (multi-deployment)

When using broker mode, you can enforce retention across all connected deployments:

- `POST /v1/agents/{agent_id}/memory/retention/enforce`

Omit `deployment_ids` to fan out to all connected deployments, or pass
`deployment_ids=dep1,dep2` to target specific deployments.

## Background enforcement

When `memory_retention_interval_ms > 0`, the daemon periodically applies the policy
using the configured limits. This is **best-effort** and never blocks request handling.

## Future work

- Salience-driven structured deprecations (policy uses salience score instead of age only).
- Archive tier for memory artifacts with restore workflows.
- Operator-visible retention audit trail.

## References

- Source snapshot: `ref/claude-mem` (see `.source.json` for upstream metadata).
