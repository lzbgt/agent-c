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
- `run_id`
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

## API surface

- `GET /v1/approvals` (broker) or `/api/v1/approvals` (agentd)
  - filters: `status`, `team_id`, `run_id`
- `GET /v1/approvals/{approval_id}`
- `POST /v1/approvals/{approval_id}/decisions`
  - body: `{ "member_id": "...", "decision": "approve|deny", "note": "..." }`

## SSE events

- `approval_request`
- `approval_update`
- `approval_resolved`

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
