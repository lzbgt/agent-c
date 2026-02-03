# Durable Workflows (agentd)

Date: 2026-02-03

`agentd` includes a durable workflow scheduler: a workflow is a persisted DAG of tasks (typically normal `/api/v1/run` requests)
that can continue running across daemon restarts.

The workflow engine is intended to be the framework’s “power source” for:

- task continuity across time (durable + resumable)
- retries + backoff
- deterministic correctness checks
- (future) budgets and higher-level scheduling policies

## Key semantics

### Durability and restart recovery

- Workflows and tasks are persisted in the SQLite DB (`agentd.db`).
- If the daemon restarts while tasks are running, those tasks are recovered back to `queued`.
  - This implies **at-least-once** execution semantics for inflight tasks.
  - If your tools or external side-effects are not idempotent, design tasks with explicit idempotency keys.

### Task dependency scheduling (DAG)

Each task may declare:

- `depends_on: ["A", "B", ...]`

A task is runnable when:

- its status is `queued`
- `now >= ready_unix_ms` (retry/backoff gate)
- all dependencies are in `done`

### Retries and backoff

Each task has:

- `max_attempts` (default: `1`)
- `attempt` (incremented each time the engine claims the task)
- `ready_unix_ms` (used by the engine to defer retries)

On failure, if `attempt < max_attempts`, the engine re-queues the task and sets a bounded quadratic backoff.

### Deterministic correctness checks (`expect`)

`expect` is a deterministic assertion object evaluated by the workflow engine after the task’s run completes.

Supported checks (v1):

- `ok: true|false` (checks the run response `ok` field)
- `assistant_text_contains: "<substring>"` (or an array of substrings)
- `json_pointer_equals`:
  - object `{ pointer: "/some/path", value: <any-json> }`, or
  - array of such objects

Pointers follow JSON Pointer (`""` for root, otherwise `/key/0/subkey`).

If expectations fail, the task is treated as failed (and may retry if configured).

### Prompt templating (v1)

For simple dataflow, the engine expands placeholders in `request.prompt`:

- `${task.<id>.assistant_text}`

This is resolved from the **completed** dependency’s `assistant_text` (from its persisted `result_json`).

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

### List workflows

`GET /api/v1/workflows?status=running&limit=50`

### Cancel a workflow (best-effort)

`POST /api/v1/workflow/cancel`

Body:

```json
{ "workflow_id": "wf_..." }
```

Cancellation semantics:
- queued tasks are transitioned to `cancelled` best-effort
- running tasks are not forcibly interrupted (v1)

## Storage (SQLite)

See `docs/DB.md` (schema v9) for:

- `workflows`
- `workflow_tasks`

