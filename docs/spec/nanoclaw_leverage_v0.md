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
- `refs/nanoclaw/docs/SDK_DEEP_DIVE.md`
- `refs/nanoclaw/src/container-runner.ts`
- `refs/nanoclaw/src/container-runtime.ts`
- `refs/nanoclaw/src/env.ts`
- `refs/nanoclaw/src/group-queue.ts`
- `refs/nanoclaw/src/ipc.ts`
- `refs/nanoclaw/src/mount-security.ts`
- `refs/nanoclaw/src/db.ts`
- `refs/nanoclaw/skills-engine/*`

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

### 7) Agent teams: streaming sessions + multi-result handling

**NanoClaw pattern**
- The SDK’s agent-team mode keeps the leader process alive **after** the first
  `result` message, continuing to yield further results and task notifications.
- If the SDK is used in single-turn (string prompt) mode, it closes stdin after
  the first `result`, which can terminate teammates mid-task.
- The `resumeSessionAt` option anchors resumption to a specific assistant message
  UUID to prevent branching on stale session tips.

**Leverage in our system**
- Treat team runs as **multi-result streams**: don’t assume “first result wins.”
- Keep team sessions open while teammate work is active; only finalize after the
  team signal indicates all members are done (or an explicit shutdown is issued).
- Add **session anchoring** to prevent race conditions when multiple roles share
  a backing model session (or when cross-role handoffs reuse context).

**Integration steps**
1. Extend the run stream contract to permit **multiple terminal results** for
   team runs and surface them in the WebUI timeline in order.
2. Add a `session_anchor_id` (or equivalent) to team member runs to ensure
   resume points are deterministic across concurrent roles.
3. For provider adapters that support “streaming input” semantics, keep the
   input channel open until all teammates have completed or timed out; then
   issue a coordinated shutdown sequence.

### 8) External mount allowlist + read-only project root

**NanoClaw pattern**
- Additional mounts are validated against an external allowlist stored outside
  the project (`~/.config/nanoclaw/mount-allowlist.json`), merged with blocked
  patterns, and resolved via realpath before accepting. The main project root is
  mounted read-only, with only required writable paths mounted separately.

**Leverage in our system**
- Add a **host tool mount allowlist** stored outside the repo, so tool sandbox
  mounts cannot be modified by agents. Enforce blocked patterns and realpath
  checks before mounting.

**Integration steps**
1. Add `~/.config/agent/mount-allowlist.json` (or similar) with allowed roots +
   blocked patterns; keep it out of repo and never mount it into sandboxes.
2. Enforce allowlist + blocked patterns before any host mount is attached to
   a sandboxed tool runner; resolve symlinks to prevent traversal.
3. Mount the project root read-only in sandbox mode; mount writable workspaces
   and IPC paths explicitly to prevent code tampering.

### 9) Per-group IPC namespaces + authorization

**NanoClaw pattern**
- Each group gets its own IPC directory; IPC watchers validate sender identity
  (main vs non-main) before allowing cross-group actions.

**Leverage in our system**
- Use **per-team namespaces** and explicit authorization for internal IPC-like
  operations (e.g., scheduled tasks, agent-to-agent messages) to prevent cross-
  tenant access in broker/agentd.

**Integration steps**
1. Introduce per-team IPC/event namespace directories or channels for tool
   actions that need to cross process boundaries.
2. Enforce sender identity/role checks for cross-team operations (admin-only
   broadcast; non-admin only within their team).
3. Add audit logs for unauthorized attempts to surface policy violations.

### 10) Group-level concurrency queue with idle preemption

**NanoClaw pattern**
- Per-group queue with a global concurrency limit; idle containers can be
  preempted when tasks arrive.

**Leverage in our system**
- Apply **per-team concurrency gating** for run execution (especially when
  sandboxed tools or external provider budgets are involved).

**Integration steps**
1. Add a queue layer in the broker/orchestrator that tracks active runs per
   team and enforces a global concurrency ceiling.
2. Allow “idle” runs to be preempted or paused when higher-priority work
   arrives (based on run priority or SLA).
3. Expose queue status in WebUI for operator visibility.

### 11) Secret handling outside process env

**NanoClaw pattern**
- Reads `.env` directly and passes only whitelisted secrets to containers via
  stdin; avoids populating `process.env` to reduce leak risk.

**Leverage in our system**
- Limit secrets exposure in agentd/broker by **not** injecting all secrets into
  process env; pass only required tokens to the tool runner on-demand.

**Integration steps**
1. Add a small env reader that returns a strict allowlist for tool runs.
2. Pass secrets to tools via ephemeral channels (stdin or short-lived files)
   and scrub logs of secret payloads.
3. Expand diagnostics to confirm which secrets are passed (names only).

### 12) Container runtime abstraction + orphan cleanup

**NanoClaw pattern**
- Container runtime abstraction is centralized; startup checks and orphan
  cleanup run before agent execution.

**Leverage in our system**
- Keep tool sandbox runtime logic isolated in one module and clean up orphans
  on startup to avoid zombie sandboxes.

**Integration steps**
1. Add a `sandbox_runtime` module with `ensure_running()` and `cleanup_orphans()`.
2. Call runtime checks from tool runner initialization.
3. Surface runtime health status in broker diagnostics.

### 13) Skill manifest + apply pipeline

**NanoClaw pattern**
- Skills are first-class artifacts with manifests, structured outcomes, and
  apply/replay/uninstall mechanics (skills engine).

**Leverage in our system**
- Provide **auditable transformation scripts** for common changes (new provider,
  connector, policy preset), with structured metadata and replay support.

**Integration steps**
1. Define a minimal skill manifest schema and store applied skills in state.
2. Build a `tools/skills/apply` flow that backs up and records changes.
3. Add a WebUI “skill” panel with diff preview before apply.

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
