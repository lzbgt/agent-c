# Approval Queue + Tool-Level Quorum v0

Date: 2026-02-20
Status: draft

## Summary

Add a unified approval queue for sensitive actions and tool calls, with
team- and role-based quorum rules. Tool execution pauses until the
approval is resolved or times out.

## Goals

- Deterministic, auditable approvals for tool calls and sensitive actions.
- Role- and quorum-aware approval rules.
- SSE updates for approval lifecycle events.
- WebUI approval queue with clear status and audit trail.

## Non-goals

- Human-in-the-loop task planning (only gate execution).
- Cross-run approvals (each approval is scoped to a run).

## Data model

ApprovalRequest:
- `approval_id`
- `run_id` (optional integer when known)
- `trace_id` (best-effort correlation; always present)
- `session_id` (optional)
- `job_id` (optional)
- `team_id` (optional)
- `tool_name` and `tool_args_hash`
- `required_approvals` and `role_constraints`
- `status` (pending | approved | denied | expired)
- `created_unix_ms`, `expires_unix_ms`
- `decision_reason` (optional)

ApprovalDecision:
- `approval_id`
- `member_id`
- `decision` (approve | deny)
- `decision_unix_ms`
- `note` (optional)

## Execution flow

1) Tool call hits approval gate (policy + quorum rules).
2) Tool loop emits `approval_request` event and pauses.
3) Approvers decide; broker/agentd records decisions.
4) Tool loop resumes on approve, or fails on deny/expire.

## Agentd storage (v0)

- SQLite tables:
  - `approval_requests(approval_id TEXT PRIMARY KEY, run_id INTEGER, trace_id TEXT, session_id TEXT, job_id TEXT, team_id TEXT,
     tool_name TEXT, tool_call_id TEXT, tool_args_hash TEXT, required_approvals INTEGER, role_constraints_json TEXT,
     status TEXT, created_unix_ms INTEGER, expires_unix_ms INTEGER, decision_reason TEXT)`
  - `approval_decisions(id INTEGER PRIMARY KEY AUTOINCREMENT, approval_id TEXT, member_id TEXT, decision TEXT,
     decision_unix_ms INTEGER, note TEXT)`
- `approval_id` is generated as `approval_<hex>`; `tool_args_hash` is SHA256 over raw `arguments_json`.
- After run completion, `run_id` is backfilled for approvals with the matching `trace_id`.

## Config surface (agentd)

- `policy_approval_tools` (CSV or array): tool names requiring approvals.
- `policy_approval_required` (int, default 1): approvals required per gate.
- `policy_approval_roles` (CSV, optional): role allowlist.
- `policy_approval_timeout_ms` (int, default 300000): approval expiry.
- `policy_approval_poll_ms` (int, default 500): poll cadence while waiting.

Approvals only gate tools listed in `policy_approval_tools`. Empty list disables the queue.

## API surface

- `GET /v1/approvals` (broker) or `/api/v1/approvals` (agentd)
  - filters: `status`, `team_id`, `run_id`, `trace_id`, `job_id`, `tool_name`
- `GET /v1/approvals/{approval_id}`
- `POST /v1/approvals/{approval_id}/decisions`
  - body: `{ "member_id": "...", "decision": "approve|deny", "note": "..." }`

## SSE events

- `approval_request`
- `approval_update`
- `approval_resolved`

Payload fields (v0):
- `approval_id`, `trace_id`, `run_id` (when known), `team_id`, `tool_name`, `tool_call_id`, `tool_args_hash`
- `status`, `required_approvals`, `role_constraints`
- `created_unix_ms`, `expires_unix_ms`
- For updates/resolved: `decision`, `decision_reason`, `approved`, `required_approvals`

## Policy integration

- Policy hooks declare which tools require approvals.
- Quorum rules define roles + counts.
- Evidence bundles include approval events and decisions.

## Evidence tests

- Tool call pauses on approval requirement.
- Approval resolves tool call deterministically.
- Timeout leads to denied/expired status.
- SSE events include approval lifecycle transitions.

## References

- `docs/spec/team_orchestration_v0.md`
- `docs/spec/policy_hooks_v0.md`
- `docs/AGENTIC_VISION.md`
