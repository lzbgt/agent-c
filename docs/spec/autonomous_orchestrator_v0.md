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

## Constraints (facts)

- Team orchestration exists in the broker (`team_orchestration_v0.md`).
- Orchestrator console UX is defined in `orchestrator_console_v0.md`.
- Moderator directives/tasks exist and are persisted (`automation_mode_v0.md`).
- Runtime members are supported for team runs (replace/merge).

## Current primitives (implemented)

- Teams, members, quorum rules, and team runs (sync/async).
- Role plan in team meta (`role_graph`, `role_instructions`, `role_prompt_mode`).
- Runtime members for dynamic per-run composition.
- Team run SSE events: created/status/quorum/runtime updates.
- Moderator fan-out (directives/tasks) + event aggregation.

## Gaps to reach “full automation, low drift”

1) **Goal contract + drift guard**
   - Explicit goal contract stored with a run (goal, success criteria, constraints).
   - Periodic checkpoints emit `goal_progress` and `goal_drift` events.

2) **Role graph execution**
   - Handoff edges are stored but not yet executed or audited.
   - Need a `team_handoff` event and a minimal handoff runner.

3) **Dynamic runtime member allocation**
   - Runtime members are manual today.
   - Need an allocator that selects connected agents by role and capacity.

4) **Shared memory scope enforcement**
   - Scope IDs are stored; enforcement is still pending.
   - Team runs should apply read/write mode at tool level.

5) **Tool-level quorum gates**
   - Team-run quorum exists; tool-level quorum is still pending.

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

### Goal progress + drift events (SSE)

- `goal_progress`: checkpoint updates (milestones, partials, evidence references)
- `goal_drift`: detected deviation from the goal contract

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
