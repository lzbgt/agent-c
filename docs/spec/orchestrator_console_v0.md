# Orchestrator Console (WebUI) v0

Date: 2026-02-25  
Status: draft (rolling)

## Goals

1) **Full automation by default**
   - Orchestrator runs continue without user presence.
   - Users can intervene at any time without blocking default execution.

2) **Moderator-as-agent**
   - The user is represented as a moderator agent (see `automation_mode_v0.md`).
   - Moderator directives/tasks are first-class events and persisted.

3) **Team-graph orchestration UI**
   - Create teams and define **role-specific backends** (model/base URL/tools/timeout) using `team.role_overrides`.
   - Define a **role plan** (roles + handoff edges) plus **role instructions** tied to a goal.
   - Connect agents to roles, define orchestration goals, and start runs with role-aware prompts.

4) **Reload-safe, persistent runs**
   - UI reloads must not interrupt background work.
   - Run state is persisted in the broker DB and rehydrated on load.

## Non-goals

- New workflow engine (reuse existing broker team runs + agentd run/task model).
- New identity provider (reuse existing OIDC + broker auth model).
- Global policy revamp (reuse existing policy hooks + approvals).

## Constraints (facts)

- Team orchestration exists in the broker (`team_orchestration_v0.md`).
- Automation profiles and moderator control plane exist in `automation_mode_v0.md`.
- Tool/host policies are enforced by agentd (policy hooks + approvals).
- All durable state must be auditable and deterministic.

## Current primitives (implemented)

- Teams, members, quorum rules, and team runs in broker DB.
- Per-member backend profiles via member meta `run_overrides` allowlist.
- Per-role run overrides via `team.role_overrides` (allowlist enforced).
- Team defaults can persist `meta.role_overrides` and are applied when runs omit overrides.
- Team meta supports a **role plan** stored under:
  - `meta.role_graph` (roles + edges)
  - `meta.role_instructions` (per-role instruction strings)
  - `meta.role_prompt_mode` (`prepend|append|replace`)
  These defaults are applied to team runs when the run does not override them.
- Runtime members for per-run team composition (replace/merge).
- Async team runs (`team.mode=async`) with job persistence + status reconcile.
- Broker SSE events: `team_run_created`, `team_run_status`, `team_quorum_*`, `team_runtime_members_updated`.
- WebUI Team console with run panel (recent runs + inline approvals).
- WebUI Team console includes orchestrator run panel (create/list/update/heartbeat).
- WebUI team run status panel shows goal contracts, goal progress/drift events, and handoff events; it can emit goal + handoff updates.

## Proposed UI flows

### 1) Team designer

- **Create team** with display name + optional tags.
- **Define roles**: planner, executor, reviewer, orchestrator (free-form).
- **Bind agents to roles** with:
  - agent_id + deployment_id
  - role + status
  - backend profile (model/base URL/tools/timeout)
- **Role plan editor**:
  - Role graph edges (`from_role` → `to_role`, optional `reason`)
  - Role instructions (used to generate per-role prompts for a goal)

### 2) Backend profile editor

- Per-role backend config (model/base URL/tools/timeout) wired to `team.role_overrides`.
- Per-member overrides (already supported by broker allowlist via member meta).
- Inline validation: no secrets in UI state (keys are not stored).

### 3) Orchestration launch

- Create a **team run** with:
  - goal/prompt
  - role prompt mode (`prepend|append|replace`) + per-role instructions (optional)
  - role selection / role allowlist
  - async vs sync
  - optional quorum approvals (inline)
- For advanced flows, allow a run to spawn **runtime members** (ephemeral).

### 4) Run monitor (reload-safe)

- Recent runs list (SSE-driven refresh).
- Run detail view (status, member jobs, dispatch errors, cancel results).
- Runtime member updates and patch actions (replace/merge).

## Persistence + reload safety

- Runs are persisted by broker with `team_run_id`, status, payload, and job ids.
- Orchestrator runs persist separately (`/v1/teams/{team_id}/orchestrator/runs`).
- UI refresh uses `GET /v1/teams/{team_id}/runs` and SSE events to rehydrate.
- Broker event replay is used to restore team run + orchestrator run events when reconnecting.
- Orchestrator run responses include lease status (heartbeat age + timeout) for stale detection.
- SSE is used to refresh lists; durable state is always in DB.
- Team runs persist `member_sessions` so moderators can target specific members
  after reload (used by team-run moderator broadcasts).
- The UI stores the **last-focused team run** per team in local storage so it can
  auto-resume lookups after refresh (optional, user-controlled).

## Event model (SSE)

Required for UI refresh:

- `team_run_created` (run persisted, includes status + mode + created_unix_ms)
- `team_run_status` (status transitions)
- `team_quorum_request` / `team_quorum_result` (approvals)
- `team_runtime_members_updated` (runtime member changes)

## Security + access

- Team owner (or admin) controls team membership and runs.
- Moderator actions are stored as events and do not bypass policy enforcement.
- Role-based approvals are enforced by existing approval queue logic.

## Implementation plan (rolling)

Phase 0 (docs + alignment):
- This spec + link to `team_orchestration_v0.md` and `automation_mode_v0.md`.
- UI copy updates for nonblocking orchestration (already partially shipped).

Phase 1 (role + backend UX):
- Add role definitions + backend profile editor UI (per-role + per-member).
- Persist role metadata in team meta (broker DB).
 - Add role instructions + role graph editor (stored in team meta).
 - Allow team runs to reuse stored role instructions without re-entry.

Phase 2 (team graph + orchestration UX):
- Visual role graph (nodes=roles, edges=handoff) and handoff list view.
- Orchestrator run builder (goal + role routing + approvals).
 - Role prompt composition (prepend/append/replace with goal substitution).

Phase 3 (moderator ops):
- Moderator task panel integrated with team run view.
- Moderators can publish directives without stopping runs.
- Broadcast uses team-run session mappings to target members by role or id.
- Moderator event feed aggregates per-member session events into a unified view.

## Open questions

- How should role graphs be stored (team meta vs dedicated table)?
- Should a team support multiple orchestrator roles?
- When should runtime member updates be auto-applied vs manual?
