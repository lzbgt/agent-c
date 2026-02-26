# Automouse Unleashed Framework (v0)

Date: 2026-02-26
Status: draft (rolling)

## Goals

1) **Full automation by default**
   - Runs proceed without human gating unless policy explicitly requires it.
   - Default automation profile is "full" (unrestricted) for automouse stacks.

2) **Multi-agent collaboration as the norm**
   - Teams of agents collaborate on a shared goal with explicit role plans,
     shared memory scopes, and handoff events.

3) **Dynamic capacity**
   - The orchestrator can request new agents or reallocate runtime members
     when roles are missing or workload increases.

4) **Low-drift execution**
   - The system should detect and correct drift early with evidence-backed
     checkpoints, replans, and goal contract alignment.

5) **Rare, nonblocking user engagement**
   - User input is optional, invoked only when drift or ambiguity is detected,
     and resumes without breaking autonomy.

6) **Refresh-safe, reconnect-safe operations**
   - The WebUI and control plane resume reliably after reload or reconnect,
     with replayable event history and persisted cursors.

## Non-goals

- Replace or deprecate existing run/workflow protocols.
- Require continuous human supervision.
- Tie the framework to a single model provider or tool stack.

## Constraints (facts)

- Contracts are versioned and feature-gated via `/api/v1/caps`.
- Policy hooks and approvals remain optional; automation is nonblocking by default.
- Evidence and event logs must be replayable and auditable.

## Existing building blocks (references)

- `automation_mode_v0.md`: automation profiles + moderator control plane.
- `team_orchestration_v0.md`: team run model, roles, shared memory, quorum.
- `autonomous_orchestrator_v0.md`: autonomous loop, drift handling, allocator.
- `agent_spawn_adapter_v0.md`: spawn adapter interface for provisioning.
- `user_guidance_lane_v0.md`: operator guidance lane + ack/receipts.
- `webui_server_prefs_v1.md`: refresh-safe WebUI persistence.
- `run-events/run_events_v1.md`: canonical event envelope + payload schemas.

## Architecture (conceptual)

### 1) Autonomy defaults

Automouse stacks should set the **default automation profile** to "full"
(unrestricted). This keeps the system operating without user intervention.
Approvals and policy gates are enabled only by explicit configuration.

### 2) Multi-agent orchestration loop

The orchestrator maintains a **goal contract** and **role plan snapshot**
for the active team run, emits progress/drift events, and dispatches work
to runtime members. Role handoffs are explicit events, enabling replay and
post-hoc audits.

### 3) Dynamic agent creation + allocation

The loop uses:

- **Runtime member allocation** when connected agents exist.
- **Spawn requests** via the spawn adapter when capacity is missing.
- **Allocator mode** for role-based selection when multiple agents fit.

Future extension: backlog- or latency-driven autoscale (beyond missing roles).

### 4) Drift minimization + replanning

Low drift is enforced via:

- Evidence-backed checkpoints and drift events.
- Replan guidance with ack/receipt thresholds.
- Resume events carrying previous + new goals for auditability.

Future extension: automatic replan proposals based on deterministic evals.

### 5) Rare user engagement lane

User engagement is routed through the guidance lane:

- Operator guidance is optional and nonblocking by default.
- Acks + receipts provide the minimal intervention needed to resume.
- When the user returns, the UI presents a concise briefing
  (goal, drift reason, evidence, proposed changes).

### 6) WebUI resilience + reconnect

The UI uses:

- Replayable event logs with cursors.
- Server-side client prefs for run/watch state.
- Resume-on-refresh for long-running runs and workflows.

## Open gaps / next steps

1) **Capacity autoscale signals**: define backlog/latency metrics and hook them
   into orchestrator decisions (beyond missing-role allocation).
2) **Drift auto-replan**: deterministic eval + proposal generation with evidence.
3) **Operator briefing payloads**: structured summaries for re-entry moments.
4) **Goal/role plan evolution**: role plan deltas with versioned diff events.
5) **Orchestrator modularization**: split large loop implementation into
   SOLID submodules for scheduling, allocation, drift, and guidance.
