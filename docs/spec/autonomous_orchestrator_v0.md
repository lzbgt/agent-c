# Autonomous Orchestrator v0 (full automation, low drift)

Date: 2026-02-25  
Status: draft (rolling)

## Goals

1) **Full automation by default**
   - Orchestrator runs continue without user presence.
   - User engagement is optional and nonblocking.

2) **Low-drift execution**
   - Runs track explicit goals and progress checkpoints.
   - Drift is detected and recorded, not hidden.

3) **Dynamic collaboration**
   - Orchestrator can allocate runtime members as needed.
   - Teams are pre-created and remain the durable anchor.

4) **Auditable + replayable**
   - All orchestration actions are persisted and evented.
   - Decisions are explainable and replayable.

## Non-goals (v0)

- Auto-provisioning host processes or containers for agentd nodes.
- Global autoscaling or multi-tenant resource scheduling.
- LLM-driven policy scripting (policy hooks/VM are handled separately).

Future (v1+):
- Pluggable `agent_spawn` adapters that can directly provision new agentd nodes.
- V0 exposes spawn request APIs so external automation can act as the adapter (see `agent_spawn_adapter_v0.md`).

## Constraints (facts)

- Team orchestration exists in the broker (`team_orchestration_v0.md`).
- Orchestrator console UX is defined in `orchestrator_console_v0.md`.
- Moderator directives/tasks exist and are persisted (`automation_mode_v0.md`).
- Runtime members are supported for team runs (replace/merge).
- Broker `/v1/events` persists events and exposes `/v1/events/replay` (Broker console + team console rehydrate on refresh).
- Broker orchestrator spawn requests persist and emit events (`/v1/teams/{team_id}/orchestrator/spawn_requests`).
- Broker can optionally allow admin client tokens for automation endpoints
  (`--client-auth-allow-automation`) to run unattended services.

## Current primitives (implemented)

- Teams, members, quorum rules, and team runs (sync/async).
- Role plan in team meta (`role_graph`, `role_instructions`, `role_prompt_mode`).
- Runtime members for dynamic per-run composition.
- Team run SSE events: created/status/quorum/runtime updates.
- Moderator fan-out (directives/tasks) + event aggregation.
- Auto-allocation: broker allocator endpoint + `auto_allocate_roles` on team runs.
- Broker event log + replay API (`/v1/events/replay`).
- Orchestrator runs persisted in broker (`/v1/teams/{team_id}/orchestrator/runs`).
- Spawn adapter CLI + claim guard (`expected_status`) for spawn requests.

## Gaps to reach “full automation, low drift”

1) **Goal contract + drift guard**
   - Explicit goal contract stored with a run (goal, success criteria, constraints).
   - Periodic checkpoints emit `goal_progress` and `goal_drift` events.

2) **Role graph execution**
   - Handoff edges are stored but not yet executed or audited.
   - Need a `team_handoff` event and a minimal handoff runner.

3) **Dynamic runtime member allocation**
   - Broker supports an allocator endpoint and `auto_allocate_roles` on team runs.
   - Orchestrator loop attempts allocator-backed runtime member allocation when
     `auto_allocate_missing_roles` are reported on the active team run, then falls back
     to spawn requests if roles remain missing (validation pending via compose smoke).

4) **Automated spawn request issuance**
   - Spawn requests are persisted + evented and adapters can fulfill them.
   - Orchestrator loop issues spawn requests after allocator attempts (or when allocation is disabled).

5) **Shared memory scope enforcement**
   - Scope IDs are stored; enforcement is still pending.
   - Team runs should apply read/write mode at tool level.

6) **Tool-level quorum gates**
   - Team-run quorum exists; tool-level quorum is still pending.

7) **Durable orchestration state**
   - Orchestrator runs are persisted (DB + CRUD).
   - Lease/heartbeat endpoint exists; ownership claim guards prevent split-brain.
   - Automated lease supervision still needed (stale detection + takeover policy).

8) **Event replay + UI rehydration**
   - Replay API exists; Broker console + team console rehydrate on refresh.
   - Cursor persistence and replay tuning still need to be hardened for long-lived runs.

## Proposed orchestration model (v0)

### Orchestrator run (new durable object)

```
orchestrator_run_id: string
team_id: string
goal: string
goal_contract: { success_criteria?: string[], constraints?: string[] }
role_plan_snapshot: { role_graph?, role_instructions?, role_prompt_mode? }
status: planned|running|waiting|paused|done|error
created_unix_ms: integer
updated_unix_ms: integer
```

### Autonomous loop service (agentd-orchestrator)

The autonomous loop is a **separate adapter process** (like the spawn adapter) that
polls broker state and drives orchestration forward without user presence.

Minimal responsibilities:

1) **Lease heartbeat**
   - Refresh `/v1/teams/{team_id}/orchestrator/runs/{run_id}/heartbeat` for active runs.
   - If status is `planned`, the loop may advance it to `running`.
   - Claim ownership when `meta.orchestrator_owner` is empty (`expected_owner=""`).
   - Heartbeats and updates include `expected_owner` to avoid conflicting writers.

2) **Team run dispatch**
   - If `meta.active_team_run_id` is missing, create a team run using the
     orchestrator run goal and role plan snapshot.
   - Store the created `team_run_id` in `meta.active_team_run_id`.

3) **Completion handling**
   - When the active team run reaches a terminal status, update the orchestrator run
     status (`done` or `error`) based on `meta.completion_mode`.

4) **Spawn requests on missing roles**
   - If a team run reports `auto_allocate_missing_roles`, emit spawn requests for those roles
     (unless existing requests already cover them).
   - Attempt allocator-backed runtime member allocation first when enabled.

5) **Progress + drift checkpoints**
   - Emit `goal_progress` events periodically (`meta.progress_every_ms`).
   - Emit a single `goal_drift` event when a run exceeds `meta.drift_after_ms`.
   - Optional drift action (`meta.drift_action="guidance"|"pause"|"cancel"|"replan"`)
     can emit guidance, pause the orchestrator run, cancel the active team run,
     or pause + request a replan.

6) **Handoff execution**
   - When `meta.handoff_queue` contains events, publish them to the active team run
     and pop them from the queue.
   - Orchestrator dispatches a moderator directive to the `to_role` when possible;
     if no eligible member sessions exist, it retries without popping the queue.

#### Orchestrator run meta contract (v0)

These optional fields live in `orchestrator_run.meta` and drive the loop behavior:

```
  meta:
  autonomous: true|false              # default true
  team_mode: "async"|"sync"           # default async
  auto_allocate_roles: true|false     # default true
  auto_allocate_max_members: number   # optional cap
  allocator_retry_after_ms: number    # optional retry window for allocator attempts
  spawn_missing_roles: true|false     # default true
  spawn_count_per_role: number        # default 1
  spawn_count_by_role: { role: number }  # optional per-role override
  spawn_requirements_by_role: { role: { ... } }  # optional per-role requirements override
  retire_runtime_members: true|false  # default false
  retire_runtime_member_status: "paused"|"active"  # default paused
  completion_mode: "on_success"|"on_failure"|"never"  # default on_success
  progress_every_ms: number           # emit goal_progress every N ms (optional)
  drift_after_ms: number              # emit goal_drift after N ms (optional)
  drift_action: "none"|"guidance"|"pause"|"cancel"|"replan"  # optional drift response
  drift_guidance_kind: "warning"|"constraint"|"directive"|"context"  # optional
  drift_guidance_priority: "low"|"normal"|"high"|"urgent"            # optional
  drift_guidance_message: string      # optional override for guidance text
  drift_guidance_payload: { ... }     # optional payload merged into guidance
  drift_guidance_target_orchestrator: string # optional target; default "human"
  drift_guidance_target_roles: [string]      # optional targeting
  drift_guidance_target_member_ids: [string] # optional targeting
  drift_guidance_target_agent_ids: [string]  # optional targeting
  drift_replan_kind: "directive"|"warning"|"constraint"|"context"     # optional
  drift_replan_priority: "low"|"normal"|"high"|"urgent"               # optional
  drift_replan_message: string        # optional override for replan text
  drift_replan_payload: { ... }       # optional payload merged into replan guidance
  drift_replan_target_orchestrator: string # optional target; default "human"
  drift_replan_target_roles: [string]      # optional targeting
  drift_replan_target_member_ids: [string] # optional targeting
  drift_replan_target_agent_ids: [string]  # optional targeting
  replan_goal: string                      # optional goal override on resume
  replan_goal_contract: { ... }            # optional goal contract override on resume
  replan_role_plan_snapshot: { ... }       # optional role plan override on resume
  replan_create_new_run: true|false        # optional: clear active run and start new
  replan_cancel_active_run: true|false     # optional: cancel active run before new
  replan_ack_min: number                   # optional receipts required (default 1)
  replan_ack_roles: [string]               # optional receipt role filter
  replan_ack_sources: [string]             # optional receipt source filter
  replan_ack_all_roles: true|false         # optional require all roles to ack
  drift_replan_ack_by: string              # set when guidance is acked
  drift_replan_ack_note: string            # set when guidance is acked
  drift_replan_ack_unix_ms: integer        # set when guidance is acked
  replan_ack_count: number                 # receipt count meeting filters
  replan_ack_satisfied_unix_ms: integer    # when receipt threshold met
  replan_ack_roles_seen: [string]          # roles from receipts used to resume
  replan_ack_sources_seen: [string]        # sources from receipts used to resume
  replan_event_unix_ms: integer            # when replan resume event emitted
  replan_event_error: string               # error if resume event fails
  replan_prev_goal: string                 # previous goal captured before resume
  replan_prev_goal_contract: { ... }       # previous goal contract captured before resume
  replan_prev_role_plan_snapshot: { ... }  # previous role plan snapshot captured before resume
  replan_prev_team_run_id: string          # previous run id when new run requested
  replan_new_run_requested_unix_ms: integer # when new run requested
  replan_cancel_unix_ms: integer           # when active run cancel issued
  replan_cancel_error: string              # cancel error, if any
  allow_takeover: true|false          # default true; allow stale-lease takeover
  handoff_queue: [{from_role,to_role,reason?,message?,data?}]  # optional queue
  run_template: { ... }               # optional /api/v1/run payload
  team_overrides: { ... }             # optional TeamRunRequest.team overrides

  # loop-maintained fields
  orchestrator_owner: string
  orchestrator_owner_claimed_unix_ms: integer
  orchestrator_owner_prev: string
  active_team_run_id: string
  team_run_history: [{team_run_id, status, updated_unix_ms}]
  spawn_requests: { role: [spawn_request_id, ...] }
  allocator_last_team_run_id: string
  allocator_last_missing_roles: [string, ...]
  allocator_last_missing_signature: string
  allocator_last_unix_ms: integer
  allocator_allocated_roles: [string, ...]
  allocator_missing_roles: [string, ...]
  allocator_warnings: [string, ...]
  allocator_runtime_members_added: integer
  runtime_members_retired_team_run_id: string
  runtime_members_retired_status: string
  runtime_members_retired_unix_ms: integer
  last_tick_unix_ms: integer
```

Notes:
- Drift guidance defaults to `target_orchestrator_id="human"` when no explicit targets
  are provided so the item stays open for operator review. Set
  `drift_guidance_target_orchestrator` to the orchestrator id to auto-ack.
- `drift_action="cancel"` uses the team-run cancel endpoint (async runs only); sync
  runs will return a 400 and the loop records `drift_action_error`.
- `drift_action="pause"` updates the orchestrator run status to `paused` and the
  loop skips further dispatch until resumed.
- `drift_action="replan"` pauses the orchestrator run and emits a guidance item
  with `replan_requested=true` for operator review.
  - Once the guidance item is acked, the loop resumes the orchestrator run and
    records `drift_replan_ack_*` fields in meta.
  - On resume, the loop snapshots the previous goal/contract/role plan into
    `replan_prev_*` fields and includes `prev_goal`/`goal` in the
    `replan_resume` event payload when available.
  - When `replan_ack_min` or receipt filters are set, the loop waits for enough
    guidance receipts before resuming.
  - If `replan_create_new_run` is true, the loop clears `active_team_run_id` so
    a new team run is created with any `replan_*` overrides.
  - If `replan_cancel_active_run` is true, the loop attempts to cancel the
    existing team run before starting a new one.

### Goal progress + drift events (SSE)

- `goal_progress`: checkpoint updates (milestones, partials, evidence references)
- `goal_drift`: detected deviation from the goal contract
- `spawn_validation`: invalid spawn meta detected (errors persisted in orchestrator run meta)
- `replan_resume`: replan guidance acked and orchestrator run resumed

### Runtime member allocation

Allocator input:

```
roles: ["planner","executor",...]
prefer_connected: true
max_members?: integer
```

Allocator output:

```
runtime_members: [{ agent_id, deployment_id?, role }]
warnings?: string[]
```

Allocator should:
- Avoid duplicates with team members + existing runtime members.
- Prefer connected agents with active deployments.

## User engagement model (nonblocking)

- The user is a moderator agent that can:
  - publish directives/tasks
  - override goals or add constraints
- User engagement never blocks the run by default; it creates events.

## Evidence tests (planned)

1) Goal contract smoke:
   - Create run with `goal_contract`, verify progress events.
2) Role handoff smoke:
   - Emit `team_handoff` and verify audit trail.
3) Allocator smoke:
   - Allocate runtime members from connected agents by role.
4) Drift guard smoke:
   - Force a deviation and validate `goal_drift` event.
