# User Guidance Lane v0

Date: 2026-02-26
Status: draft

## Summary

Provide a durable, low-friction guidance lane so operators can inject
occasional corrections or constraints without breaking autonomous runs.
Guidance events are persisted, replayable, targeted to team/run/roles,
and produce explicit acknowledgements from orchestrator/agents.

## Goals

- Persist operator guidance so refresh/reconnect never drops intent.
- Target guidance to a team, run, role, member, or specific agent.
- Provide acknowledgements/receipts from orchestrator or agents.
- Replay guidance via SSE/event replay for UI and services.
- Keep the surface minimal and deterministic (small JSON payloads).

## Non-goals

- Real-time chat or free-form conversation threads.
- A replacement for approvals/quorum gates.
- Arbitrary plan editing; guidance is directional, not a full plan graph.

## Data model (broker)

GuidanceEvent:
- `guidance_id` (string, `guidance_<hex>`)
- `team_id`
- `team_run_id` (optional)
- `kind` (directive | context | warning | constraint)
- `priority` (low | normal | high | urgent)
- `message` (string, max 4096 chars)
- `payload` (object, optional structured hints)
- `target_roles` (string array, optional)
- `target_member_ids` (string array, optional)
- `target_agent_ids` (string array, optional)
- `target_orchestrator_id` (string, optional)
- `created_by` (string, display name)
- `created_sub` (string, principal sub)
- `created_unix_ms`
- `expires_unix_ms` (optional)
- `status` (open | acked | superseded | expired)
- `acked_by` (string, optional)
- `acked_unix_ms` (optional)
- `ack_note` (optional)

GuidanceReceipt (audit trail):
- `guidance_id`
- `ack_by`
- `ack_role` (optional)
- `ack_source` (orchestrator | agent | human)
- `ack_unix_ms`
- `note` (optional)

## Storage (v0)

New tables:
- `broker_guidance_events(guidance_id TEXT PRIMARY KEY, team_id TEXT, team_run_id TEXT,
  kind TEXT, priority TEXT, message TEXT, payload JSONB, target_roles JSONB,
  target_member_ids JSONB, target_agent_ids JSONB, target_orchestrator_id TEXT,
  created_by TEXT, created_sub TEXT, created_unix_ms BIGINT, expires_unix_ms BIGINT,
  status TEXT, acked_by TEXT, acked_unix_ms BIGINT, ack_note TEXT)`
- `broker_guidance_receipts(id BIGSERIAL PRIMARY KEY, guidance_id TEXT REFERENCES broker_guidance_events(guidance_id),
  ack_by TEXT, ack_role TEXT, ack_source TEXT, ack_unix_ms BIGINT, note TEXT)`

Indexes:
- `idx_guidance_team_run` on `(team_id, team_run_id)`
- `idx_guidance_status` on `(team_id, status, created_unix_ms DESC)`

## API surface

### Create guidance

`POST /v1/teams/{team_id}/guidance`

Body:
```
{
  "team_run_id": "run_123",
  "kind": "directive",
  "priority": "high",
  "message": "Stay within the existing deployment; do not spawn new agents.",
  "payload": { "reason": "budget" },
  "target_roles": ["lead"],
  "target_member_ids": ["member_1"],
  "target_agent_ids": ["agent_123"],
  "target_orchestrator_id": "orch_primary",
  "expires_unix_ms": 1761177600000
}
```

### List guidance

`GET /v1/teams/{team_id}/guidance?team_run_id=...&status=open&since_ts=...&limit=...`

### Acknowledge guidance

`POST /v1/teams/{team_id}/guidance/{guidance_id}/ack`

Body:
```
{
  "status": "acked",
  "note": "Applied and re-planned.",
  "ack_source": "orchestrator"
}
```

### List guidance receipts

`GET /v1/teams/{team_id}/guidance/{guidance_id}/receipts?limit=50`

## SSE / replay events

Event types:
- `team_guidance_created`
- `team_guidance_ack`
- `team_guidance_expired`

Payload fields:
- `guidance_id`, `team_id`, `team_run_id`, `kind`, `priority`, `message`, `payload`
- `target_roles`, `target_member_ids`, `target_agent_ids`, `target_orchestrator_id`
- `created_by`, `created_sub`, `created_unix_ms`, `expires_unix_ms`
- For ack/expired: `status`, `acked_by`, `acked_unix_ms`, `ack_note`

Events are persisted in the broker event log so `/v1/events/replay` restores
guidance state on refresh.

## Execution flow

1) Operator posts guidance for a team/run or target role.
2) Broker persists the event and emits `team_guidance_created`.
3) Orchestrator/agents subscribe (SSE or replay) and apply guidance.
4) Orchestrator/agent acknowledges via `/ack`, emitting `team_guidance_ack`.
5) UI shows guidance lifecycle and receipt trail.

## RBAC

- Create: team owner or admin (OIDC).  
- Ack: orchestrator service tokens (admin client auth + automation allowlist) or
  team owners for manual ack.

## Limits

- Max message length: 4096 chars.
- Guidance list caps: 200 per team/run (oldest archived or superseded).
- Optional expiry to auto-close stale guidance.

## References

- `docs/spec/autonomous_orchestrator_v0.md`
- `docs/spec/approval_queue_v0.md`
- `docs/spec/team_orchestration_v0.md`
