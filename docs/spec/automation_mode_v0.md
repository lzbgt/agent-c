# Automation Mode + Moderator Control Plane (v0)

Date: 2026-02-25
Status: draft (rolling)

## Goals

1) **Full automation by default**
   - Default runtime profile enables full tool access and uninterrupted execution.
   - Human intervention is optional; runs should not block unless policy explicitly requires it.

2) **Moderator-as-agent model**
   - The user is represented as a **moderator agent** that can publish tasks, issue directives,
     and resolve approvals without halting the system.
   - Moderator actions are first-class events, stored and replayable like all other agent actions.

3) **Nonblocking engagement**
   - The system continues to execute while the user is idle or offline.
   - When the user reconnects, they can inspect state, issue directives, and optionally override.

4) **High efficiency + natural interaction**
   - Interaction model supports short, natural commands ("pause", "split", "retry", "delegate").
   - Automation mode should minimize friction and maximize throughput.

5) **Rolling memory consolidation**
   - Memory system continuously consolidates long-running context into recap layers
     (daily/weekly/session) while preserving evidence and provenance.

## Constraints (facts)

- The portable core remains **env-agnostic** and **transport-agnostic**.
- All policy decisions must be **deterministic** and auditable.
- The approval queue remains **optional** and is only enforced when configured.
- Contracts are versioned; feature detection occurs via `/api/v1/caps`.

## Architecture

### 1) Automation profiles

Introduce a named automation profile, with **default derived from daemon config**.

- **full**
  - `yolo_default=true`
  - `host_policy=full`
  - `policy_mode=off`
- **guided**
  - `yolo_default=false`
  - `host_policy=readonly`
  - `policy_mode=audit`
- **strict**
  - `yolo_default=false`
  - `host_policy=readonly`
  - `policy_mode=enforce`
- **custom**
  - No override (use daemon config as-is)

Profiles are **declared** by agentd and exposed via `/api/v1/caps` as `features.automation`.
`default_profile` is derived from the daemon config; clients may override per-run via `automation_profile`.
The run response includes `effective_automation_profile`.

### 2) Moderator agent identity

A moderator action is represented as an **actor** with role `moderator`.
All moderator actions are stored as normal run events:

- `moderator_task_published`
- `moderator_directive`
- `moderator_approval_decision`

This keeps the system replayable and deterministic.

### 3) Nonblocking user engagement

Automation continues by default. Blocking occurs only if:

- a policy hook requires quorum approval, or
- an explicit workflow step is declared as `await_moderator`.

### 4) Rolling memory consolidation

A layered pipeline performs continuous consolidation:

1) **Capture**: observations and tool outputs saved with timestamps.
2) **Checkpoint**: daily/weekly summaries with provenance links.
3) **Correlation**: cross-run entity linking and thematic clustering.
4) **Recall**: progressive disclosure (index → timeline → details).

All consolidation steps are explicit jobs with stored outputs.

## API / data model additions (planned)

- `automation.default_profile` in `/api/v1/caps`
- `automation_profile` in run submit payloads
- `moderator` role identity in approvals + event streams
- `POST /api/v1/moderator/directive` + `POST /api/v1/moderator/task`
- `GET /api/v1/moderator/events` (filtered from client events)

## Implementation plan (rolling)

Phase 0 (docs + config):
- Define automation profiles in docs and caps.
- Expose default profile in `/api/v1/caps`.

Phase 1 (identity + approvals):
- Bind authenticated users to `moderator` role.
- Enforce `policy_approval_roles` when identity is present.

Phase 2 (moderator control plane):
- Add moderator task/directive publish + subscribe endpoints.
- WebUI panel for moderator directives + approvals.
  - 2026-02-25: endpoints + WebUI panel for directives/tasks added (events stored as client events).

Phase 3 (memory consolidation):
- Add consolidation jobs + schedule.
- WebUI memory rollup explorer + evidence links.

## Open questions

- How should multiple moderators resolve conflicts (single writer vs quorum)?
- What is the default directive vocabulary for natural commands?
- Should moderator directives be globally scoped or per-run?
