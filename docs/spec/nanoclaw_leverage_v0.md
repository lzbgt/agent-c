# NanoClaw Leverage Plan v0

Date: 2026-03-04
Status: draft (rolling)

## Summary

This document captures **leverageable architectural patterns** from the NanoClaw
codebase (cloned under `refs/nanoclaw`) and maps them to concrete, low-risk
integration steps for this project. The goal is to harvest the best ideas
without importing NanoClaw wholesale or diluting our SOLID boundaries.

## Sources reviewed (local)

- `refs/nanoclaw/README.md`
- `refs/nanoclaw/docs/SPEC.md`
- `refs/nanoclaw/docs/SECURITY.md`

## Goals

- Identify patterns that **increase capability** with minimal coupling.
- Provide **concrete integration steps** tied to our existing subsystems.
- Preserve our production posture: audits, replayability, broker support,
  and operator-grade controls.

## Non-goals

- Rewriting our stack to match NanoClaw’s runtime model.
- Replacing `agentd` workflows with NanoClaw scheduler loops.
- Importing NanoClaw source code or dependencies directly.

## Constraints

- Must remain compatible with the current agentd/broker/WebUI contract.
- Must preserve replayability, audits, and policy hooks.
- Must keep the WebUI “advanced” layout (Scene + Conversation) as primary UI.

## Pattern leverage map

### 1) Channel registry + self-registration

**NanoClaw pattern**
- Channels self-register at startup via a registry (`src/channels/registry.ts`).
- Skills add channel implementations and register them at module load time.

**Leverage in our system**
- Define a **tool/connector registry** boundary for agentd/broker-side
  “channels” (Slack/Discord/Email) that register at startup.
- Keep the registry **thin**: channel init + health + send/receive callbacks.

**Integration steps**
1. Introduce a `channel_registry` module in the broker (or a dedicated
   “connector” service) with a minimal interface (connect/send/health).
2. Add a “skill installer” workflow that copies channel modules into a
   controlled `connectors/` directory and updates a registry barrel file.
3. Add a broker capability flag (`caps.features.channels=true`) and a
   WebUI Connections panel that lists registered channels.

### 2) Per-group isolation (workspace + memory)

**NanoClaw pattern**
- Each group has isolated filesystem mounts and a dedicated `CLAUDE.md` memory.

**Leverage in our system**
- Expand team run isolation with **per-run workspace roots** and
  **memory scope** enforcement.
- Avoid reusing a single session context across role boundaries.

**Integration steps**
1. Add `workspace_root` to team run member overrides, stored per member.
2. Bind `memory_scope_id` to the run’s `workspace_root` by default, unless
   a shared scope is explicitly requested.
3. Surface workspace/memory linkage in the WebUI Team console (read-only
   summary plus override controls).

### 3) Container-isolated tool runtime

**NanoClaw pattern**
- Bash and tool access happen inside a Linux container with explicit mounts.

**Leverage in our system**
- Provide an **optional sandboxed tool runner** for high-risk tools while
  preserving agentd’s default host tooling for low-latency workflows.

**Integration steps**
1. Define a tool execution adapter interface (`tool_exec_adapter`) with
   `host` and `sandbox` implementations.
2. Implement a minimal sandbox adapter that runs tool calls in a container
   with explicit mounts and no network by default (opt-in).
3. Add policy hooks so `policy_mode=enforce` can require the sandbox adapter
   for specific tools (e.g. `shell_exec`, `proc_exec`).

### 4) Scheduled task loop

**NanoClaw pattern**
- A scheduler loop runs recurring tasks and wakes the agent container.

**Leverage in our system**
- Expose a **first-class cron schedule** binding for agentd workflows, so
  operator tasks no longer require external scheduling glue.

**Integration steps**
1. Add a `schedule` field to workflow submissions (cron + timezone).
2. Persist schedule in workflow metadata; agentd loop enqueues runs
   without manual polling.
3. Add WebUI schedule UI in Workflows panel (list + enable/disable).

### 5) SQLite-backed message/event log

**NanoClaw pattern**
- Messages are persisted and a loop polls DB for work.

**Leverage in our system**
- We already persist runs/events; apply NanoClaw’s **reload-safe history**
  approach to ensure WebUI refresh always reconstructs a linear timeline.

**Integration steps**
1. Ensure the WebUI main conversation view reconstructs a strict
   **timestamp-ordered timeline** across messages, tool records, and run
   outcomes.
2. Expand event replay to include tool command/result details in the
   timeline without grouping them artificially.

### 6) “Skills over config” customization

**NanoClaw pattern**
- Claude Code skills rewrite the codebase to add features instead of
  bloated configuration.

**Leverage in our system**
- Provide operator-grade, **scripted transforms** for common changes
  (new providers, new connectors, custom tool policies).

**Integration steps**
1. Define a `tools/skills/` catalog with small, auditable patch scripts.
2. Add a WebUI “Guided change” panel that executes a chosen skill and
   shows a diff preview before applying.

## Risks and tradeoffs

- **Sandboxed tools** add latency and operational complexity; must stay
  optional and policy-driven.
- **Cron workflows** increase scheduler load; need caps/quotas to prevent
  unbounded background work.
- **Channel registry** can expand attack surface; enforce auth and
  permissions at the broker boundary.

## Acceptance criteria

- A new spec exists, linked from the spec index.
- The TODO tracker references this leverage plan as delivered.
- Follow-up issues map each integration step to a concrete milestone.
