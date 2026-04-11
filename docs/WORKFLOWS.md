# Durable Workflows (agentd)

Date: 2026-02-05

`agentd` includes a durable workflow scheduler: a workflow is a persisted DAG of tasks (typically normal `/api/v1/run` requests)
that can continue running across daemon restarts.

The workflow engine is intended to be the framework’s “power source” for:

- task continuity across time (durable + resumable)
- retries + backoff
- deterministic correctness checks
- budgets and higher-level scheduling policies (fairness/backpressure)

## Key semantics

### Workflow deadline (v1.2)

Workflows may optionally carry a scheduler-level deadline:

- `deadline_unix_ms` (submit request top-level; Unix time in milliseconds)

Semantics:
- Once `now_unix_ms > deadline_unix_ms`, the workflow engine:
  - cancels any `queued` tasks (marks them `cancelled` with error `"deadline exceeded"`)
  - requests cancellation for the workflow (best-effort)
  - does **not** forcibly interrupt `running` tasks (v1)
- When all tasks become terminal, the workflow becomes terminal (typically `cancelled`) with error `"deadline exceeded"`.

This deadline is enforced by the **scheduler**, so it helps bound long fan-out workflows even if tasks are misconfigured.

### Scheduling priority (v1)

Workflows and tasks support a simple integer priority hint:

- `workflow.priority` (submit request top-level `priority`)
- `task.priority` (task entry `priority`, or `request.priority`)

Higher priorities are scheduled sooner when multiple runnable tasks exist.

Notes:
- The daemon clamps priorities to `[-1000, 1000]` (default `0`).
- Priority is a scheduling hint, not a correctness guarantee.

### Fairness budgets (v1.1)

Under load, “priority only” is not enough: a single large fan-out workflow can monopolize all workers.
`agentd` therefore supports **simple scheduler-level fairness caps**:

- `--workflow-max-inflight-per-workflow <n>` (default: `2`)
  - Limits how many tasks from the same workflow may be `running` concurrently.
  - Prevents “fan-out storms” from starving other workflows.
- `--workflow-max-inflight-per-session <n>` (default: `0` = disabled)
  - Optional multi-tenant cap across all workflows sharing the same `session_id`.
  - Useful when multiple clients share one daemon and you want predictable fairness.

Env equivalents:
- `AGENTD_WORKFLOW_MAX_INFLIGHT_PER_WORKFLOW`
- `AGENTD_WORKFLOW_MAX_INFLIGHT_PER_SESSION`

### Admission control / backpressure at submit time (v1.5)

Fairness limits how many tasks may be running concurrently, but it does not prevent a client from submitting an
unbounded backlog (which can grow the DB and delay other work). `agentd` therefore supports **admission control**
caps applied at `POST /api/v1/workflow/submit`:

- `--workflow-admit-max-inflight-tasks-per-session <n>` (default: `0` = disabled)
  - Caps total workflow tasks with status `queued|running` across all workflows that share the same `session_id`.
- `--workflow-admit-max-inflight-tasks-total <n>` (default: `0` = disabled)
  - Caps total workflow tasks with status `queued|running` across the whole daemon.

If a submit would exceed a configured cap, the endpoint responds with HTTP `429` and a small `retry_after_ms` hint.

Env equivalents:
- `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_PER_SESSION`
- `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_TOTAL`

Notes:
- The per-session cap only applies when `allow_sessions=true` and a non-empty `session_id` is provided.
- `agentd` will auto-create/upsert the referenced session row on first submit to satisfy the DB foreign key constraint.

### Durability and restart recovery

- Workflows and tasks are persisted in the SQLite DB (`agentd.db`).
- Client/UI disconnects (including browser refresh) do **not** cancel workflows; cancellation is explicit via `/api/v1/workflow/cancel`.
- If the daemon restarts while tasks are running, those tasks are recovered back to `queued`.
  - This implies **at-least-once** execution semantics for inflight tasks.
  - If your tools or external side-effects are not idempotent, design tasks with explicit idempotency keys.

### Run replay bundles (deterministic audit)

Each run can persist a redacted, deterministic replay bundle for offline verification.

Contents:
- request (redacted)
- response (redacted)
- tool_records (per-tool inputs/outputs)
- replay_sha256 (canonical JSON hash via agent_json_c14n_v1)

Redaction:
- request: remove api_key, Authorization, auth_token, trace_text, http_body, input_files[].data_base64
- response: remove http_body, trace_text

Bounds:
- request JSON max: 512 KiB (redacted)
- response JSON max: 1 MiB (redacted)

Endpoint:
- GET /api/v1/run/replay?run_id=<id>
- GET /api/v1/run/attestation?run_id=<id> (attestation bundle referencing replay hash)

Notes:
- Runs with no_session=true do not persist replay bundles.
- Oversized payloads are omitted and replay_error is recorded.

### Workflow submit idempotency (v1.3)

Workflow submission supports an optional **idempotency key** to make client retries safe:

- `idempotency_key` (submit request top-level; `id`-safe string, max 128 chars)

Semantics:
- If `idempotency_key` is provided and a workflow already exists with the same:
  - `idempotency_key`, and
  - `COALESCE(session_id,'')` scope (session-less workflows share one global scope),
  then `POST /api/v1/workflow/submit` returns the existing workflow and sets `deduped=true`.
- The second submit does **not** overwrite the existing workflow’s spec; the first accepted submit “wins”.

This is primarily for robustness against:
- client-side retry loops (HTTP timeouts, disconnections)
- load balancers that retry POSTs
- “at-least-once” delivery semantics from upstream systems

### Task dependency scheduling (DAG)

Each task may declare:

- `depends_on: ["A", "B", ...]`

A task is runnable when:

- its status is `queued`
- `now >= ready_unix_ms` (retry/backoff gate)
- all dependencies are satisfied:
  - default: dependency status is `done`
  - if the dependency is `error` and `allow_error=true`, it is also considered satisfied (soft-fail)
  - for aggregation/join tasks (`kind:"aggregate"`), dependencies may be any terminal state (`done|error|cancelled`)
    so the join can compute over failures/timeouts without blocking forever

#### Optional dependency inference (`infer_depends_on`)

Template-based dataflow is safest when it is paired with explicit dependencies. To reduce human error, the workflow submit
endpoint can optionally infer dependencies by scanning each task request JSON for:
- `${task.<id>...}` template references
- `{"$ref":"task.<id>..."}` JSON-native embedding references

When enabled, inferred IDs are merged into `depends_on` for that task before the DAG is validated/persisted.

Enable it per-submit:
- `POST /api/v1/workflow/submit` with top-level `infer_depends_on: true`

Notes:
- This is a correctness convenience; it does not change runtime template semantics.
- If inference introduces a cycle, submission fails with “invalid workflow DAG”.

### Retries and backoff

Each task has:

- `max_attempts` (default: `1`)
- `attempt` (incremented each time the engine claims the task)
- `ready_unix_ms` (used by the engine to defer retries)

On failure, if `attempt < max_attempts`, the engine re-queues the task and sets a bounded quadratic backoff.

Some task kinds can also request a custom retry schedule:

- If a task returns `retryable=true` with `retry_in_ms`, the engine re-queues it using that delay (instead of the default backoff).
  This is intended for polling patterns like edge task completion waits.

### Soft-fail tasks (`allow_error`)

If a task is submitted with:

- `allow_error: true`

Then if that task ends `status="error"`, the workflow may still complete `done` (so long as there are no remaining “hard”
errors from tasks where `allow_error=false`).

### Deterministic correctness checks (`expect`)

`expect` is a deterministic assertion object evaluated by the workflow engine after the task’s run completes.

Supported checks (v1):

- `ok: true|false` (checks the run response `ok` field)
- `assistant_text_contains: "<substring>"` (or an array of substrings)
- `json_pointer_equals`:
  - object `{ pointer: "/some/path", value: <any-json> }`, or
  - array of such objects
- `json_pointer_exists: "/ptr"` (or an array of pointers)
- `json_pointer_regex`:
  - object `{ pointer: "/some/path", regex: "..." }`, or
  - array of such objects
  - semantics: `regex_search` over the stringified value (if the value is not a string, it is JSON-stringified first)
- `json_pointer_number_between`:
  - object `{ pointer: "/some/path", min?: <number>, max?: <number> }`, or
  - array of such objects
  - values may be JSON numbers or numeric strings
- `json_pointer_schema`:
  - object `{ pointer: "/some/path", schema: <json-schema-subset> }`, or
  - array of such objects
  - the root pointer is the empty string `""`
  - supported schema subset: `type`, `enum`, `required`, `properties`, `additionalProperties:false`, and `items`
  - unsupported schema keywords are ignored, so use this for deterministic contract checks rather than full JSON Schema validation
- Tool-call constraints (v1.1):
  - `tool_called: "tool_name"` (or array of names)
    - requires that at least one `tool_call` event exists for each named tool
  - `tool_not_called: "tool_name"` (or array of names)
    - requires that no `tool_call` event exists for each named tool
  - `tool_calls_total_between: { min?: <int>, max?: <int> }`
    - bounds total number of tool calls executed (counted from `tool_call` events)
  - `tool_calls_for_tool_between`:
    - object `{ tool: "name", min?: <int>, max?: <int> }`, or
    - array of such objects

Pointers follow JSON Pointer (`""` for root, otherwise `/key/0/subkey`).

If expectations fail, the task is treated as failed (and may retry if configured).

### Prompt templating (v1)

For simple dataflow, the engine expands placeholders in `request.prompt`:

- `${task.<id>.assistant_text}`
- `${task.<id>.json:<json_pointer>}` (e.g. `${task.A.json:/assistant_text}`)

This is resolved from the **completed** dependency’s `assistant_text` (from its persisted `result_json`).

As of v2, template expansion is applied recursively to the task’s full request JSON (not only `prompt`), so you can:
- feed prior task outputs into `edge_invoke.args` for MCU actuation
- wire prior outputs into non-prompt request fields (e.g., structured messages payloads)

### Workflow inputs (`inputs`) (v2.1)

For safer, more maintainable dataflow (vs repeating `${task.X...}` everywhere), tasks may carry an `inputs` object
(`map<string, any-json>`) which is expanded and then made available to later template expansion.

Where `inputs` can be defined:
- Workflow-level: `POST /api/v1/workflow/submit` top-level `inputs` (copied into every task request)
- Per-task: `task.inputs` (or `task.request.inputs`) overrides workflow-level keys for that task
- Runtime: tasks may include `inputs` directly in their persisted request JSON (advanced use / internal tooling)

Inputs are expanded in **two phases**:
1) Expand `task.*` templates across the full task request JSON (so inputs can reference prior task results).
2) Build `inputs_by_name` from the resolved `inputs` object and expand `input.*` templates across the request JSON.
   This is repeated in bounded rounds so `input -> input` chains converge.

Input template forms:
- `${input.<name>}` (stringifies the whole JSON value if it is not a string)
- `${input.<name>.json:<json_pointer>}` (extract a JSON Pointer from the input value)

Constraints:
- Input names must be “id-safe”: `[A-Za-z0-9_-]` (length 1..128).
- `inputs` is persisted in the workflow DB; do not store secrets unless you intend them to be durable.

### JSON-native embedding (`$ref`)

String templating is convenient but always produces strings. For structured dataflow, tasks may embed JSON from prior task
results using a special object form:

```json
{ "$ref": "task.A.json:/some/pointer" }
```

Supported references:
- `task.<id>.assistant_text`
- `task.<id>.json:<json_pointer>`
- `input.<name>`
- `input.<name>.json:<json_pointer>`

If the reference cannot be resolved, the task fails deterministically with `error="template expansion failed"`.

### Deterministic delay task (`kind:"delay"`)

For deterministic scheduling tests and “wait gates” that do not involve an LLM/provider call, workflows can use:

```json
{
  "task_id": "W",
  "kind": "delay",
  "delay_ms": 500,
  "result": { "assistant_text": "woke up" }
}
```

Semantics:
- Sleeps for `delay_ms` (clamped to `0..600000`).
- Produces a deterministic result object with `ok=true`, plus any merged keys from `result`.
- If `result.assistant_text` is omitted, `assistant_text` defaults to `"delay:<delay_ms>"`.

### Deterministic outbound HTTP JSON task (`kind:"http_json"`) (v1.8)

For broker/agent interop and other cross-service collaboration, workflows can run a deterministic outbound HTTP task.

Security model:
- This task is **disabled by default** (SSRF risk).
- Operators must opt in by starting the daemon with `--workflow-enable-http-tasks` (or env `AGENTD_WORKFLOW_ENABLE_HTTP_TASKS=1`).
- Optional hardening: restrict outbound targets with `--workflow-http-allow-host <host[:port]>` (repeatable),
  or env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`.
- Further hardening:
  - `--workflow-http-allow-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
  - `--workflow-http-deny-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_DENY_CIDRS=...`
  - `--workflow-http-deny-private` / env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
  - `--workflow-http-dns-pin` / env `AGENTD_WORKFLOW_HTTP_DNS_PIN=1`
    - When enabled, the daemon pins the **same** resolved addresses used by the outbound policy decision into the actual HTTP request
      (defense-in-depth against DNS rebinding between “check” and “connect”).
- Do **not** embed secrets into persisted workflow specs via headers. `Authorization` headers are rejected at submit time.
  - If you need a bearer token, use `http_json.bearer_env` to reference an env var name (only the name is persisted).

Example (local echo server):

```json
{
  "task_id": "H",
  "kind": "http_json",
  "http_json": {
    "url": "http://127.0.0.1:8080/echo",
    "method": "POST",
    "timeout_ms": 5000,
    "max_bytes": 65536,
    "body": { "ping": "pong" }
  }
}
```

Result shape (high level):
- `ok` is true when HTTP status is 2xx.
- `http.status` is the HTTP status code.
- `http.response_text` is the captured response body (bounded by `max_bytes`).
- `http.resolved_addrs` is best-effort DNS resolution evidence when `--workflow-http-dns-pin` is enabled (array of IP strings).
- `http.response_json` is best-effort parsed JSON (when parse succeeds), which enables downstream `${task.H.json:/http/response_json/...}`
  and `{"$ref":"task.H.json:/http/response_json/..."}` wiring.
- `http.response_sha256` is a best-effort deterministic hash token over `http.response_json` canonical bytes (algorithm `agent_json_c14n_v1`).
  This is intended for quorum-style joins (`kind:"aggregate" mode:"quorum_hashes"`) and cross-agent correctness correlation.
  - `mode:"quorum_hashes"` compares stable string tokens; when a pointer resolves to a non-string JSON value, the platform hashes the
    canonical JSON bytes and votes on the resulting `sha256:<64hex>` token (ergonomics for structured results).

### Deterministic agent-to-agent collaboration task (`kind:"agentd_call"`) (v1.9)

For multi-agent collaboration where **another `agentd` instance** should run a durable workflow and return a correctness-checkable
result, workflows can use `kind:"agentd_call"`.

This is a deterministic collaboration primitive:
 - submit a remote workflow (`POST <base_url>/api/v1/workflow/submit`)
 - poll until terminal (`GET <base_url>/api/v1/workflow?workflow_id=...`)
 - compute best-effort deterministic hash surfaces for quorum/correlation:
   - `agentd.result_sha256`: hash of a **stable projection** of terminal remote `final.result` (preferred stable surface)
     - `agentd.result_sha256_schema=agentd_call_result_stable_v1` (defines which ephemeral fields are pruned before hashing)
   - `agentd.final_sha256`: hash of the full terminal remote workflow JSON (includes workflow_id/timestamps; mainly for audit/debug)
   (algorithm `agent_json_c14n_v1`)

Broker proxy compatibility:
 - `base_url` does not have to be a directly reachable agentd. It can also be a **broker proxy prefix**:
  - `https://<broker>/v1/agents/<agent_id>/proxy`
  - The task will then call `POST .../proxy/api/v1/workflow/submit` and poll `GET .../proxy/api/v1/workflow?...`.
- In broker deployments, auth is usually `Authorization: Bearer <OIDC_JWT>`:
  - use `agentd_call.bearer_env` to reference an env var name containing the OIDC token (secret value is not persisted).
  - optionally, you can avoid hardcoding the proxy path by using `agentd_call.broker_proxy` (server computes/persists `base_url`, target identity metadata, and optional deployment routing):

```json
{
  "task_id": "REMOTE",
  "kind": "agentd_call",
  "agentd_call": {
    "broker_proxy": { "broker_base_url": "https://broker.example", "agent_id": "1", "deployment_id": "dep-a" },
    "bearer_env": "OIDC_TOKEN",
    "workflow": { "tasks": [ { "task_id": "W", "kind": "delay", "delay_ms": 10 } ] }
  }
}
```

Security model:
- This task is **disabled by default** (same SSRF surface as `http_json`).
- Operators must opt in by starting the daemon with `--workflow-enable-http-tasks` (or env `AGENTD_WORKFLOW_ENABLE_HTTP_TASKS=1`).
- Optional hardening: restrict outbound targets with `--workflow-http-allow-host <host[:port]>` (repeatable),
  or env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`.
- Further hardening:
  - `--workflow-http-allow-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
  - `--workflow-http-deny-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_DENY_CIDRS=...`
  - `--workflow-http-deny-private` / env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
  - `--workflow-http-dns-pin` / env `AGENTD_WORKFLOW_HTTP_DNS_PIN=1`
    - When enabled, the daemon pins the **same** resolved addresses used by the outbound policy decision into the actual HTTP request.
- Do **not** embed secrets into persisted workflow specs via headers. `Authorization` headers are rejected at submit time.
  - If you need a bearer token, use `agentd_call.bearer_env` to reference an env var name (only the name is persisted).

Example (remote agent runs a tiny deterministic delay workflow):

```json
{
  "task_id": "REMOTE",
  "kind": "agentd_call",
  "agentd_call": {
    "base_url": "http://127.0.0.1:9090",
    "op": "workflow_submit_and_wait",
    "timeout_ms": 20000,
    "poll_ms": 50,
    "include_tasks": true,
    "include_results": true,
    "workflow": {
      "tasks": [
        { "task_id": "W", "kind": "delay", "delay_ms": 10, "result": { "assistant_text": "remote ok" } }
      ]
    }
  }
}
```

Result shape (high level):
- `ok` is true when the *remote workflow* reaches status `"done"`.
- `agentd.base_url` and `agentd.op` identify the canonical remote request path.
- `agentd.target_identity` is the stable join identity (`url:<base_url>` for direct targets, or
  `broker:<broker_base_url>:<agent_id>[:<deployment_id>]` for broker-proxy targets).
- `agentd.target_id`, `agentd.broker_agent_id`, and `agentd.broker_deployment_id` are present when the submit-time spec or
  `agentd_parallel` macro supplies that metadata.
- `agentd.workflow_id` is the remote workflow_id (reused on retry to avoid duplicate submit).
- `agentd.final` is the remote `GET /api/v1/workflow` JSON response (best-effort bounded), enabling references like:
  - `${task.REMOTE.json:/agentd/final/workflow/status}`
  - `{"$ref":"task.REMOTE.json:/agentd/final/result/results_by_task/W/assistant_text"}`

### Parallel agent collaboration macro (`kind:"agentd_parallel"`) (v1.10)

`kind:"agentd_call"` is the primitive; `kind:"agentd_parallel"` is the high-leverage composition:
fan out to multiple remote `agentd` instances **in parallel**, then join deterministically with `kind:"aggregate"`.

This is submit-time syntactic sugar (like `delegate_parallel`):
- the server expands the macro into one derived `kind:"agentd_call"` task per target (task ids `<task_id>:<target_id>`)
- then replaces the macro task with a deterministic join task at the original `task_id` (`kind:"aggregate"`)

Security / gating:
- This macro expands into `agentd_call` tasks, so it is gated by `--workflow-enable-http-tasks` (SSRF surface).
- The same outbound hardening applies (`--workflow-http-allow-host`, `--workflow-http-allow-cidr`, `--workflow-http-deny-cidr`, `--workflow-http-deny-private`, `--workflow-http-dns-pin`).

Example (fan out across 2 remotes; pick the first successful branch):

```json
{
  "task_id": "P",
  "kind": "agentd_parallel",
  "agentd_parallel": {
    "targets": [
      { "id": "r0", "base_url": "http://127.0.0.1:9090" },
      { "id": "r1", "base_url": "http://127.0.0.1:9091" }
    ],
    "agentd_call": {
      "op": "workflow_submit_and_wait",
      "timeout_ms": 20000,
      "poll_ms": 50,
      "include_tasks": true,
      "include_results": true,
      "workflow": {
        "tasks": [
          { "task_id": "W", "kind": "delay", "delay_ms": 10, "result": { "assistant_text": "remote ok" } }
        ]
      }
    },
    "aggregate": {
      "mode": "first_ok",
      "ok_pointer": "/ok",
      "value_pointer": "/agentd/final/result/results_by_task/W/assistant_text"
    }
  }
}
```

Notes:
- `agentd_parallel.agentd_call.base_url` must be omitted (each target provides a base_url).
- The server overwrites `aggregate.task_ids` to match the derived tasks.
- Default join strategy is `mode:"first_ok"` if `aggregate.mode` is omitted.
- For `aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.node_pointer="/agentd/target_identity"` when omitted, so
  `require_distinct_nodes:true` counts direct URL targets and broker agent/deployment targets correctly.
- For `aggregate.mode:"quorum_hashes"`, the server also defaults `aggregate.pointers=["/agentd/result_sha256"]` when omitted.
- `agentd_parallel.targets[]` entries may also use `broker_proxy:{broker_base_url,agent_id,deployment_id?}` (same as `agentd_call.broker_proxy`); the server computes `base_url` as `.../v1/agents/<agent_id>/proxy`, sets `X-Agentd-Deployment` when `deployment_id` is present, and emits `agentd.target_identity`.
- `agentd_parallel.targets_from_broker_registry` snapshots targets from `GET <broker_base_url>/v1/agents` at submit time. It is gated by `--workflow-enable-http-tasks` plus the normal workflow outbound allowlist/DNS-pin policy, supports `bearer_env`, and expands connected/enabled broker agents into ordinary `broker_proxy` targets before scheduling.
  - `deployment_policy:"prefer_deployments"` (default) creates one target per connected deployment when `agents[].deployments` is present, otherwise falls back to an agent-level broker proxy.
  - `deployment_policy:"all_connected"` creates only connected deployment targets.
  - `deployment_policy:"agent"` creates one agent-level broker proxy per matching agent.
  - `agent_ids`, `connected_only`, `enabled_only`, `max_targets`, `timeout_ms`, `max_bytes`, and `id_prefix` narrow or bound discovery. The discovered target list is snapshotted into concrete derived tasks; later broker registry changes do not mutate an accepted workflow.
- `agentd_parallel.routing_policy.require_distinct_targets:true` rejects duplicate derived target identities before the workflow is accepted.
- `agentd_parallel.memory_scope` injects `memory_scope_id` / `memory_scope_mode` into each remote workflow; by default the scope is per target (`<scope_id>:<target_id>`) and the remote inputs include `agentd_parallel_target_id` plus `agentd_parallel_target_identity`.
- Example file: `docs/examples/workflows/agentd_parallel_demo.json`.
- Generator helper: `tools/gen_agentd_parallel_demo.py` (reads `out/devstack_state.json` when present).
- Shell wrapper: `tools/gen_agentd_parallel_demo.sh`.
- Submit helper: `tools/submit_agentd_parallel_demo.sh`.
- Python submit helper: `tools/submit_agentd_parallel_demo.py`.
  - Both support `--wait` to poll until completion.

### Deterministic memory update task (`kind:"memory_put"`) (v1.7)

To make memory updates **correctness-gated** and correlated to durable execution, workflows can run a deterministic
structured memory upsert task:

```json
{
  "task_id": "M",
  "kind": "memory_put",
  "depends_on": ["A"],
  "memory_put": {
    "path": "STRUCTURED.md",
    "entries": [
      { "key": "wf.last_alpha", "kind": "fact", "value": "${task.A.assistant_text}" }
    ],
    "checkpoint": true,
    "keep_checkpoints": 100
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host --host-policy full` (this task executes the host tool `memory_put`).
- Only **structured** updates are allowed (it does not accept legacy `text` overwrites).
- The engine injects correlation evidence into `entries[].source` when missing, so structured memory records keep a bounded
  `sources[]` trail like: `workflow:<workflow_id> task:<task_id> trace:<trace_id> [session:<session_id>]`.
- This task does not call an LLM/provider, so it is suitable for deterministic “write facts only when upstream checks passed”
  workflows (pair it with `depends_on` + `expect` on the upstream tasks).

### Deterministic memory search task (`kind:"memory_search"`) (v1.7)

For deterministic, **read-only** memory retrieval (substring/FTS search), workflows can run:

```json
{
  "task_id": "S",
  "kind": "memory_search",
  "memory_search": {
    "query": "refresh-proof",
    "max_results": 5,
    "daily_days": 0,
    "case_sensitive": false,
    "use_index": true
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host` (this task executes the host tool `memory_search`).
- This is a **read-only** host tool, so it works with `--host-policy readonly` or `--host-policy full`.
- The full host tool response is surfaced under the task result as `memory_search_response` for deterministic `expect` assertions
  and JSON templating (`${task.S.json:/memory_search_response/data/results/0/citation}` etc.).

### Deterministic memory timeline task (`kind:"memory_timeline"`) (v1.7)

For deterministic retrieval of a **bounded context window** around a citation (`path:line`), workflows can run:

```json
{
  "task_id": "T",
  "kind": "memory_timeline",
  "depends_on": ["S"],
  "memory_timeline": {
    "citation": "${task.S.json:/memory_search_response/data/results/0/citation}",
    "context_lines": 4,
    "max_chars": 2000
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host` (this task executes the host tool `memory_timeline`).
- This is a **read-only** host tool, so it works with `--host-policy readonly` or `--host-policy full`.
- The full host tool response is surfaced under the task result as `memory_timeline_response` for deterministic `expect` assertions
  and JSON templating (`${task.T.json:/memory_timeline_response/data/text}` etc.).

### Deterministic structured memory query task (`kind:"memory_structured_query"`) (v1.7)

For deterministic retrieval of **structured facts/preferences/tasks** (not fuzzy substring search), workflows can run:

```json
{
  "task_id": "Q",
  "kind": "memory_structured_query",
  "depends_on": ["M"],
  "memory_structured_query": {
    "path": "STRUCTURED.md",
    "key_prefix": "wf.",
    "kinds": ["fact"],
    "status": "active",
    "include_versions": false,
    "include_sources": true,
    "limit": 50
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host` (this task executes the host tool `memory_structured_query`).
- This is a **read-only** host tool, so it works with `--host-policy readonly` or `--host-policy full`.
- To avoid accidental full dumps, the server requires at least one filter: `key`, `key_prefix`, non-empty `kinds[]`, or `source_contains`.
- Optional query-plan knobs:
  - `source_contains`: filter by evidence trail (e.g. `trace:<trace_id>...` injected by `memory_put` tasks)
  - `updated_since_utc` / `updated_until_utc`: bound by record `updated_utc` (ISO UTC strings)
  - `order_by:"updated_desc"`: fetch most recently updated records first
- The full host tool response is surfaced under the task result as `memory_structured_query_response` for deterministic `expect` assertions
  and JSON templating (`${task.Q.json:/memory_structured_query_response/data/results/0/record/value}` etc.).

### Agent collaboration / fallback delegation (`kind:"delegate"`) (v1.6)

Sometimes “power” comes from **redundancy** and **explicit fallback**: run the same intent through multiple candidate
requests (different base_url/model/tools/budgets) and accept the first attempt that succeeds deterministically.

`kind:"delegate"` executes a sequence of sub-requests (attempts) and returns:
- a structured `delegate.attempts[]` array with per-attempt `ok/run_ok/expect_ok` outcomes
- `delegate.chosen_id` (first successful attempt when `stop_on_ok=true`)
- `assistant_text` copied from the chosen attempt (for easy templating)

Example:

```json
{
  "task_id": "D",
  "kind": "delegate",
  "delegate": {
    "stop_on_ok": true,
    "attempts": [
      {
        "id": "primary",
        "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/bad", "api_key": "dummy", "model": "stub" }
      },
      {
        "id": "fallback",
        "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" },
        "expect": { "assistant_text_contains": "OK" }
      }
    ]
  }
}
```

Notes:
- Attempt requests are normal `/api/v1/run` request objects; workflow-level `defaults` are merged into each attempt.
- The workflow engine injects a stable per-attempt `trace_id` when missing (`<workflow_trace_id>:<task_id>:<attempt_id>`).
- This is a **sequential** primitive (v1). For true parallel multi-agent collaboration, model the fan-out as separate workflow tasks
  and use `kind:"aggregate"` to join deterministically.

### Parallel collaboration macro (`kind:"delegate_parallel"`) (v1.7.0)

For true scheduler-visible collaboration (parallel execution, fairness caps, per-attempt budgets), `agentd` supports a submit-time
macro task:

- `kind:"delegate_parallel"`

This is **syntactic sugar**: on submit, the server expands it into:
- one normal workflow task per attempt (derived task IDs `<task_id>:<attempt_id>`, soft-failing by default via `allow_error=true`)
- one deterministic `kind:"aggregate"` join task at the original `task_id` (default mode `first_ok`)

This means:
- attempts run in parallel under normal workflow concurrency and fairness caps
- you can attach `expect` to each attempt (deterministic correctness)
- a single successful attempt yields the join task `ok=true`, while failed attempts do not fail the workflow

Example:

```json
{
  "task_id": "P",
  "kind": "delegate_parallel",
  "delegate": {
    "attempts": [
      { "id": "primary", "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" } },
      { "id": "fallback", "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" },
        "expect": { "assistant_text_contains": "OK" }
      }
    ]
  }
}
```

Join result fields live under the normal aggregate output at task `P`:
- `chosen_task_id` (e.g. `"P:fallback"`)
- `assistant_text` from the chosen attempt (defaults to `/assistant_text`)

#### Custom join strategy (`delegate.aggregate`)

For some collaboration patterns, “first successful” is not the best join. For example:
- **best-of-n** with self-scoring (choose the highest score)
- **quorum hashes** across multiple candidates (deterministic consensus checks)
- **collect** (materialize multiple outputs for downstream deterministic processing)

`delegate_parallel` lets you customize the join by passing `delegate.aggregate` (same knobs as `kind:"aggregate"`, except `task_ids`):
 - the server **overwrites** `aggregate.task_ids` with the derived attempt task ids (`<task_id>:<attempt_id>`)
 - if `aggregate.mode` is omitted, the server defaults to `first_ok`
 - for `aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.pointers=["/assistant_text"]` when omitted
 - for `aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.node_pointer="/effective_base_url"` when omitted (enables distinct-provider quorum)

 Example: best-of-n join over JSON candidates emitted as assistant text:

```json
{
  "task_id": "P",
  "kind": "delegate_parallel",
  "delegate": {
    "aggregate": {
      "mode": "best_of_n",
      "candidate_pointer": "/assistant_text",
      "parse_json": true,
      "score_pointer": "/score",
      "value_pointer": "/answer",
      "maximize": true,
      "require_ok": true
    },
    "attempts": [
      {
        "id": "lo",
        "request": { "prompt": "{\"score\":0.2,\"answer\":\"OK_LOW\"}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" }
      },
      {
        "id": "hi",
        "request": { "prompt": "{\"score\":0.9,\"answer\":\"OK_HIGH\"}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" }
      }
    ]
  }
}
```

## HTTP API

All endpoints require daemon auth when the daemon is started with `--auth-token`.

### Submit a workflow

`POST /api/v1/workflow/submit`

Minimal example (DAG A → B → C):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -d '{
    "allow_inline_api_keys": true,
    "tasks": [
      { "task_id": "A", "request": { "prompt": "Alpha", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" } },
      { "task_id": "B", "depends_on": ["A"], "request": { "prompt": "B got ${task.A.assistant_text}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" } },
      { "task_id": "C", "depends_on": ["B"], "request": { "prompt": "C got ${task.B.assistant_text}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" },
        "expect": { "assistant_text_contains": "Alpha" }
      }
    ]
  }' \
  "http://127.0.0.1:8123/api/v1/workflow/submit"
```

Notes:
- For durable workflows, inline `api_key` storage is rejected by default; set `allow_inline_api_keys=true` only when you
  explicitly accept storing keys in the daemon DB. Prefer configuring keys via daemon runtime config/secrets.

### Get workflow status

`GET /api/v1/workflow?workflow_id=...&include_tasks=1&include_results=0|1`

Optional debugging:
- `include_spec=1` returns the redacted persisted submit request as:
  - `spec_json` (always; may be truncated)
  - `spec` (only when `spec_json` parses as JSON)

Budget visibility (best-effort):
- `workflow_limits` is surfaced when present in the persisted submit spec.
- `workflow_usage` aggregates retry-safe per-task cumulative counters (available even when `include_tasks=0`).
- `workflow_remaining` is computed as `(limit - used)` for any positive limits (available even when `include_tasks=0`).

### Workflow scheduler stats

`GET /api/v1/workflow/stats`

Returns lightweight queue pressure metrics, useful for backpressure tuning and debugging:

- `workflows_by_status` (counts)
- `tasks_by_status` (counts)
- `tasks_queued_ready` vs `tasks_queued_not_ready` (retry/backoff vs runnable)

Optional (best-effort) session-level snapshot for multi-tenant fairness tuning:
- `GET /api/v1/workflow/stats?include_sessions=1&session_limit=32`
- Response fields: `sessions[]` with `inflight_tasks/queued_tasks/running_tasks` per `session_id`

### Workflow events (durable)

`GET /api/v1/workflow/events?workflow_id=...&after_event_id=0&limit=256`

This returns the persisted workflow event log (DB-backed), which is also the source for the SSE stream.

### Workflow streaming (SSE)

`GET /api/v1/workflow/stream?workflow_id=...&cursor=0`

Streams:

- `event: workflow_event` (each durable event record)
- `event: workflow_done` (terminal summary; stream closes)

### List workflows

`GET /api/v1/workflows?status=running&limit=50`

Status values:
- `running`, `queued`, `active` (running + queued), `done`, `error`, `cancelled`, or `all` (unfiltered)

Optional filters:
- `q` (substring match against workflow_id, trace_id, session_id, or idempotency_key)

### Cancel a workflow (best-effort)

`POST /api/v1/workflow/cancel`

Body:

```json
{ "workflow_id": "wf_..." }
```

Cancellation semantics:
- queued tasks are transitioned to `cancelled` best-effort
- running tasks are cooperatively cancelled at the next safe boundary (v1.4)
  - host tools: long-running subprocesses are terminated best-effort
  - provider calls: cancellation takes effect between requests/tool calls (cannot always interrupt an in-flight HTTP request)

## Borrowed workflow ideas (adopted and tracked)

These were imported from the urine_monitor project because they compound reliability and operability.

Implemented:
- Evidence bundles: tools/capture_agent_evidence_bundle.sh + tools/check_agent_evidence_bundle.py
- Scenario packs: tools/scenarios/ + tools/scenario_runner.py
- Eval packs: tools/eval_pack.py + tools/eval_packs/ (deterministic checks + scoring)
- One-command dev stack: tools/devstack_agent.sh + tools/devstack_agent_down.sh (use --wipe-volumes to reset Keycloak)
- Operator UX defaults: runtime config via ui/public/agentui-config.js (no rebuild required)

Proposed (still open):
- Standardized API error envelope: {"err":"...","code":"...","details":{...}}

## Storage (SQLite)

See `docs/DB.md` for:

- `workflows`
- `workflow_tasks`
- `workflow_events`
