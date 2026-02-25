# Team Orchestration v0 (agentd + broker)

Date: 2026-02-19
Status: v0 (design draft; broker team registry CRUD + sync/async team runs implemented; quorum enforcement pending)

This spec defines a **team orchestration model** for multi-agent runs that goes beyond
single-run tool loops. It formalizes agent groups, roles, shared memory scopes, and
quorum gating for sensitive actions. This is a **design target**; endpoints and schemas
listed below are proposed and only partially implemented today (team registry CRUD + sync/async team runs).

---

## Goals

- Provide a **first-class team model** (groups + roles + membership).
- Enable **shared memory scopes** with explicit read/write boundaries.
- Support **quorum gates** for safety-sensitive actions (tool classes, policy categories).
- Make orchestration **deterministic and auditable** (events + approval records).
- Keep the surface **transport-agnostic** (works with direct agentd or broker relay).
- Enable **per-member backend profiles** so each role can target its own LLM backend or runtime defaults.

## Non-goals (v0)

- Auto team formation or LLM-driven role assignment.
- Dynamic, content-based policy scripting (handled by policy hooks/VM later).
- Global load balancing or autoscaling (use existing broker + scheduler capabilities).
- Cross-tenant data sharing without explicit policy and audit.

---

## Core concepts

### 1) Team

A **team** is a stable group of agents with explicit roles and constraints.

### 2) Role

Roles are strings (e.g., `planner`, `executor`, `reviewer`) used for routing and quorum
requirements. Roles are **declarative** and must be enforced by orchestrators.

### 3) Shared memory scope

A team can attach a shared memory scope for collaboration. The scope has:
- **mode**: `read_only` or `read_write`
- **retention policy** reference (same format as memory policy IDs)
- **namespace isolation** from user sessions by default

### 4) Quorum gate

A **quorum gate** is a rule that requires approvals before certain actions proceed.
Example: at least 2 reviewers must approve `shell_exec`.

### 5) Team run

A **team run** is a coordinated execution that may spawn multiple sub-runs
per team member, with explicit aggregation and quorum checks.

### 6) Run mode (v0.4)

Team runs may execute in **sync** or **async** mode:

- `mode=sync` (default): broker blocks until all member runs complete.
- `mode=async`: broker dispatches `/api/v1/run_async` per member, returns immediately with job IDs,
  and persists `member_jobs` metadata. Status is reconciled by polling job status on demand.

Async mode enables **nonblocking** orchestration: UI refresh or disconnect does not stop member runs.
Member job execution persists inside each target `agentd` (job engine), so broker restarts do not cancel
in-flight member runs.

### 7) Cancellation (v0.5)

Async team runs can be cancelled without interrupting the browser session:

- `POST /v1/teams/{team_id}/runs/{team_run_id}/cancel`
  - broker fans out `POST /api/v1/job/cancel` to each member job
  - records `cancel_requested_unix_ms` + `cancel_results` in the run payload
  - run status shifts to `cancelling` until job statuses reconcile to terminal states

Cancellation is **best-effort**: each `agentd` cooperatively stops its tool loop at safe boundaries.

### 8) Runtime members (v0.3)

Team runs may include **runtime members** supplied in the run request. These are
ephemeral members used **only for the current run** and are **not persisted** in
the team registry. This enables an orchestrator agent (or moderator) to allocate
new members dynamically while still using a pre-created team as the anchor.

Constraints:
- Runtime members must include `agent_id` and `role` (role is used for filters + quorum).
- Runtime members participate in run fan-out but do **not** count as persistent
  team members for approval storage (approvals are stored against the team registry).
- Runtime members may include `meta.run_overrides` and `meta.backend_label`,
  and the same **allowlist** rules apply to `run_overrides`.

### 9) Member backend profile (v0.2)

Each team member may carry an optional backend profile stored under `member.meta`.
The broker can optionally merge these **run overrides** into the base run when executing
team runs. This enables role-specific LLM backends without per-deployment orchestration.

Recommended shape (stored in `member.meta.run_overrides`):

```json
{
  "run_overrides": {
    "model": "gpt-4.1-mini",
    "base_url": "https://api.openai.com/v1",
    "summary_model": "gpt-4.1-mini",
    "tools": "basic",
    "timeout_ms": 60000
  },
  "backend_label": "openai-mini"
}
```

Constraints:
- Only a **safe allowlist** of fields is applied (e.g., `model`, `base_url`, `summary_model`,
  `tools`, `timeout_ms`, `max_steps`, `stream_assistant`).
- `api_key` is never accepted via member meta (avoid secret storage in broker DB).
- The broker merges overrides as: `run = base_run ⊕ member.run_overrides` (member wins).

---

## Data model (proposed)

### Team

```
team_id: string
display_name: string
created_by: string
created_unix_ms: integer
tags: string[]
policy_ref: string | null
shared_memory_scope_id: string | null
```

### Team member

```
member_id: string
team_id: string
deployment_id: string | null  # broker-routed deployments
agent_id: string | null       # agentd-local member
role: string
capabilities: string[]        # optional hints (tools, modalities)
status: string                # active|paused|disabled
weight: integer               # scheduling hint
created_unix_ms: integer
```

Note: PATCH updates may reassign `agent_id`/`deployment_id` to retarget a role without recreating the member.

### Quorum rule

```
rule_id: string
team_id: string
action: string                # e.g. tool:proc_exec, policy:high_risk
tool_names: string[] | null   # optional explicit tool list
min_approvals: integer
role_allowlist: string[] | null
require_distinct_roles: boolean
timeout_ms: integer
quorum_mode: string           # strict|best_effort
```

### Shared memory scope

```
scope_id: string
team_id: string
mode: string                  # read_only|read_write
retention_policy_ref: string | null
```

### Team run

```
team_run_id: string
team_id: string
root_run_id: string           # canonical run ID in agentd
mode: string                  # sync|async (default sync)
created_unix_ms: integer
```

#### Member jobs (async mode)

```
member_jobs: [
  {
    member_id: string
    agent_id: string
    deployment_id: string | null
    job_id: string
    status: string               # queued|running|done|error|cancelled|interrupted|unknown
    ok: boolean | null
    error: string | null
    updated_unix_ms: integer | null
  }
]
dispatch_errors: [
  { member_id: string, agent_id: string, error: string }
]
member_job_summary: {
  total: integer
  queued: integer
  running: integer
  done: integer
  error: integer
  cancelled: integer
  interrupted: integer
  unknown: integer
  ok: integer
  failed: integer
  dispatch_errors: integer
}
cancel_requested_unix_ms: integer | null
cancel_results: [
  { member_id: string, agent_id: string, deployment_id: string | null, job_id: string, ok: boolean, error: string | null }
]
```

### Quorum approval

```
approval_id: string
team_run_id: string
rule_id: string
member_id: string
role: string
decision: string              # approve|deny
reason: string | null
created_by: string | null
created_unix_ms: integer
```

---

## API surface (proposed)

This spec does **not** commit to agentd vs broker ownership. The same shapes can be
hosted in either component; the broker is recommended for multi-deployment routing.

Current broker implementation:
- `GET/POST/PATCH/DELETE /v1/teams` + `/members` + `/quorum` are implemented.
- `POST /v1/teams/{team_id}/runs` executes sync or async fan-out across active members.
  - `team.mode=sync` blocks until all member runs complete (default).
  - `team.mode=async` dispatches `/api/v1/run_async` per member and returns immediately with job IDs.
- `GET /v1/teams/{team_id}/runs/{team_run_id}` returns stored status + current members.
- `POST /v1/teams/{team_id}/runs` enforces **team_run quorum rules** when
  `team.quorum_policy.mode` is `auto` (default). Approvals are passed inline
  via `team.approvals`; strict failures return `409`. Inline approvals are persisted
  when the run is created.
- `GET/POST /v1/teams/{team_id}/runs/{team_run_id}/approvals` list and create persisted approvals.

### Team management

```
POST   /v1/teams
GET    /v1/teams/{team_id}
PATCH  /v1/teams/{team_id}
DELETE /v1/teams/{team_id}
```

### Membership

```
POST   /v1/teams/{team_id}/members
GET    /v1/teams/{team_id}/members
PATCH  /v1/teams/{team_id}/members/{member_id}
DELETE /v1/teams/{team_id}/members/{member_id}
```

### Quorum rules

```
POST   /v1/teams/{team_id}/quorum
GET    /v1/teams/{team_id}/quorum
PATCH  /v1/teams/{team_id}/quorum/{rule_id}
DELETE /v1/teams/{team_id}/quorum/{rule_id}
```

### Team runs

```
POST /v1/teams/{team_id}/runs
GET  /v1/teams/{team_id}/runs/{team_run_id}
PATCH /v1/teams/{team_id}/runs/{team_run_id}/runtime_members
GET  /v1/teams/{team_id}/runs/{team_run_id}/approvals
POST /v1/teams/{team_id}/runs/{team_run_id}/approvals
```

Run requests may accept:

```
"team": {
  "team_id": "...",
  "mode": "sync" | "async",
  "role": "planner",
  "shared_memory": { "scope_id": "...", "mode": "read_only" },
  "quorum_policy": { "mode": "auto" | "off" },
  "approvals": [ { "member_id": "...", "rule_id": "...", "decision": "approve" } ],
  "runtime_members": [
    { "member_id": "rt-1", "agent_id": "agent_a", "role": "executor" }
  ],
  "run_overrides_mode": "off" | "member_meta" | "explicit",
  "member_overrides": {
    "member_1": { "model": "...", "base_url": "...", "tools": "basic" }
  }
}
```

Runtime members can be updated after run creation:

```
PATCH /v1/teams/{team_id}/runs/{team_run_id}/runtime_members
{
  "mode": "replace" | "merge",
  "runtime_members": [ ... ]
}
```

Notes:
- `replace` (default) overwrites the stored runtime member list; `merge` updates by `member_id` and appends new entries.
- The broker validates the runtime members and updates the stored run payload.
- For synchronous runs that already completed, updates are recorded for auditability and future orchestration; they do not retroactively re-run the completed fan-out.
- For async runs, updates do not affect already-dispatched member jobs; use runtime updates to prepare for follow-on orchestration.

`run_overrides_mode`:
- `off` (default): no per-member overrides.
- `member_meta`: apply allowlisted overrides from `member.meta.run_overrides`.
- `explicit`: apply allowlisted overrides from `team.member_overrides`.

---

## Quorum gating semantics

1) When a gated action is requested, the orchestrator emits a **quorum request**.
2) Approvals are gathered from members that match `role_allowlist`.
3) If `min_approvals` is satisfied:
   - `strict`: action executes (`decision=approve`).
   - `best_effort`: action executes (`decision=approve`).
4) If quorum is not satisfied before `timeout_ms`, the action fails (strict) or
   continues with a policy decision event (best_effort). The quorum result is
   emitted with `decision=deny` (strict) or `decision=best_effort` (best_effort).

Quorum checks should be surfaced as `policy_decision` or `team_quorum_*` events
so runs remain replayable and auditable.

---

## Shared memory semantics

Team runs may read/write a shared memory scope.

Rules:
- Team scopes are **distinct** from user session memory.
- `read_only` blocks writes from members.
- Memory queries may include `scope_id` and `mode` in parameters.

---

## Events (proposed)

New run events (names + schemas to be defined):

- `team_handoff`: explicit handoff between roles.
- `team_quorum_request`: quorum gate opened for approval.
- `team_quorum_result`: approvals received and decision outcome.
- `team_member_result`: per-member run summary.

Existing `policy_decision` events remain valid for tool/limit enforcement.

---

## Evidence tests (planned)

1) **Team run smoke**: two members, shared memory read-only, ensure writes are rejected.
2) **Quorum gate smoke**: 2-of-3 approval required for `shell_exec`.
3) **Role routing smoke**: planner-only tool list enforced by policy hooks.
4) **Team run quorum smoke**: strict `team_run` approvals return `409` until approvals supplied.

---

## Implementation notes

- This spec **builds on** existing workflow fan-out patterns but formalizes the data model.
- Broker is the recommended control plane for multi-deployment teams.
- Policy hooks can enforce role-based tool allowlists in v1.

---

## WebUI requirements (v0.2)

- Team creation panel (team id + display name).
- Member editor with role + agent assignment + deployment selection.
- **Member backend profile editor** (model/base URL/tools) with safety hints (no keys).
- Quorum rule editor (action + approvals + role allowlists).
- Team run panel with role filters, quorum previews, and inline approvals.
- **Run overrides selector** (off/member_meta/explicit) with per-member overrides grid.

---

## Open questions

- Where should team metadata live: broker DB, agentd DB, or both?
- How should quorum approvals be authenticated and attested?
- Do we need a dedicated team memory store or a namespaced view over existing memory tables?
