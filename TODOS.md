# Roadmap / TODOs (highest leverage)

Date: 2026-02-04

This roadmap is biased toward “power unleashed” coming from the **agentic framework itself**:

- autonomous high-efficiency scheduling (priorities, budgets, retries, backoff)
- task continuity across time (durable + resumable execution)
- memory architecture that compensates for stateless/context-bounded LLMs
- rolling consolidation + correlation as time advances
- result correctness (deterministic checks, replay, validations)

Observability (trace/timeline) matters, but it is **not** the origin of capability; it is the proof/debug surface.

## Recently shipped (proof in CI)

- Tool plugins (`--tool-plugin`) so tools are composable without rebuilding.
  - Proof: `ctest` includes `agentd_tool_plugin_smoke`.
- “Docs as truth” guardrails (DB schema doc + OpenAPI sanity checks).
  - Proof: `ctest` includes `docs_sanity_tests` + `openapi_sanity_tests`.
- `trace_id` correlation end-to-end, plus a merged trace timeline across broker ⇄ agentd.
  - Proof: `ctest` includes `agentd_trace_id_smoke`.
- Durable workflow engine v1 (DAG + retries + resumable after restart + deterministic expectations).
  - API:
    - `POST /api/v1/workflow/submit`
    - `GET /api/v1/workflow`
    - `GET /api/v1/workflows`
    - `POST /api/v1/workflow/cancel`
  - Engine semantics:
    - dependency scheduling (`depends_on`)
    - retries (`max_attempts`) + backoff (`ready_unix_ms`)
    - correctness assertions (`expect`)
    - **restart continuity**: running tasks are recovered back to queued (at-least-once)
    - simple prompt templating: `${task.<id>.assistant_text}`
  - Proof: `ctest` includes `agentd_workflow_smoke` (validates DAG ordering + templating + restart recovery).
- Resumable async jobs: `run_async` requests persist enough state to resume after daemon restart (at-least-once semantics).
  - Proof: `ctest` includes `agentd_job_restart_durability_smoke` (restart mid-job → finishes done).
- Memory v2 (retrieval): `memory_search` now prefers a ranked on-disk index (SQLite FTS5) when available, with automatic fallback to bounded substring scan.
  - Also: structured memory updates (`memory_put(entries)`) produce rolling JSON checkpoints under `memory/checkpoints/` for time-correlation.
  - Proof: `ctest` includes `test_host_toolset` (memory tools) plus existing daemon smokes that exercise host tools.
- Memory v2.1 (rolling consolidation, deterministic): `agentd` can promote explicit `@mem ...` markers from daily memory into structured memory via `POST /api/v1/memory/consolidate`, and can run it periodically with `--memory-consolidate-interval-ms`.
  - Proof: `ctest` includes `agentd_memory_consolidate_smoke` (idempotent; no checkpoint churn on second run).
- Memory v2.2 (versioned facts + evidence): structured memory entries now keep bounded `sources[]` (evidence) and `versions[]` (superseded history) under schema `agent_memory_v2`.
  - Proof: `ctest` includes `host_toolset_tests` assertions that validate schema upgrade + history retention.
- Workflow event log + streaming (durable): `agentd` persists workflow events (`workflow_events`) and exposes:
  - `GET /api/v1/workflow/events` (paged)
  - `GET /api/v1/workflow/stream` (SSE; ends with `workflow_done`)
  - Proof: `ctest` includes `agentd_workflow_stream_smoke`.
- Scheduler knobs (efficiency precursor): `agentd` supports configuring background engine concurrency/polling:
  - `--job-concurrency`, `--job-poll-ms`, `--workflow-concurrency`, `--workflow-poll-ms` (also reflected in `/api/v1/config`)

## P0 (next: maximize autonomous continuity + correctness)

### 1) Budgets + scheduling policy (priorities, concurrency, backpressure)

Goal:
- Make the framework “strong by default” under load: predictable progress and fairness across jobs/workflows.

Deliverables:
- Priority scheduling:
  - per-job / per-workflow `priority`
  - per-session fairness (avoid one client starving others)
- Budget + backpressure:
  - token/step/tool-call budgets enforced at scheduler level (not just per-run)
  - queue pressure metrics + admission control
- Cancellation semantics:
  - deterministic cancellation propagation (queued → cancelled; running → cooperative cancel)

Proof:
- Stress test (deterministic stub provider): submit N workflows/jobs and assert bounded completion time + fairness.

### 2) Workflow engine v2: dataflow + aggregation nodes

Problem:
- DAG ordering is useful, but real workflows need explicit **dataflow** and **aggregation strategies**.

Deliverables:
- Dataflow templates:
  - `${task.<id>.assistant_text}` (already v1)
  - `${task.<id>.json:<json_pointer>}` for structured extraction
  - explicit `inputs` map per task (safer than string templating)
- Aggregation nodes (no LLM required by default):
  - `first_ok`, `best_of_n`, `strict_all_ok`, `collect`
- Optional LLM aggregator (tools=none by default).

Proof:
- Deterministic stub-server test executes a DAG with dataflow and asserts outputs are wired correctly.

### 3) Workflow streaming + UI surface (without making UI the power source)

Status:
- Shipped v1.5: durable workflow event log + SSE stream + smoke test.

Next:
- UI view for workflow timeline (reuse trace UI patterns), plus filters (by task_id, by event type).

### 4) Memory v2: semantic retrieval + rolling consolidation

Problem:
- Stateless LLM calls and bounded context require a memory system that evolves over time:
  - consolidates outdated facts
  - correlates new information with old (conflict resolution)
  - retrieves relevant memory efficiently

Deliverables:
- Retrieval (shipped v2.0):
  - `memory_search` prefers SQLite FTS5-ranked retrieval when available (`use_index=true`), scoped to the same file set as legacy scanning (`daily_days`, core/session/structured).
  - Automatic fallback: bounded substring scan when SQLite/FTS5 is unavailable at runtime.
- Rolling consolidation + correlation (shipped v2.1, deterministic core):
  - explicit `@mem` marker promotion (daily → structured) + `POST /api/v1/memory/consolidate`
  - optional periodic scheduler (`--memory-consolidate-interval-ms`) with a conservative default (disabled)
- Rolling consolidation + correlation (next):
  - time/size based consolidation across **all** layers (core/daily/session/structured), not just daily markers
  - versioned facts: `supersedes`, `observed_utc`/`valid_from`, and multi-source evidence arrays
  - correlation graph: link memory items to `trace_id`/workflow/job ids and source excerpts

Proof:
- `ctest` covers memory tools end-to-end (`test_host_toolset`), ensuring `memory_write`→`memory_search`→`memory_get` works.
- Next: add a deterministic ranking test (index mode) + deterministic conflict-resolution tests for structured mode.

### 5) Correctness v2: validators + replayability

Deliverables:
- Expand `expect`:
  - JSON pointer assertions (already v1)
  - regex, numeric bounds, schema checks
  - tool-call constraints (e.g., forbid certain tools, require a tool call)
- “Replay mode”:
  - re-run a workflow from persisted inputs using a stub provider
  - deterministic outputs validated by expectations

Proof:
- `ctest` includes replay tests that produce identical results under stub providers.

## P1 (big wins after P0)

### 6) Tool servers (subprocess / stdio) + remote device tool bridges

Deliverables:
- Spawn a “tool server” process (stdio JSON-RPC or similar).
- Use for:
  - Playwright/browser automation (kept out-of-process)
  - device tools (ESP32 via serial/MQTT) without embedding

Proof:
- Smoke test starts a tool server and executes a tool end-to-end.

### 7) Multi-agent workflows (broker-aware)

Deliverables:
- Allow workflow tasks to target:
  - local agentd
  - broker-routed agents
- Add explicit routing policy and identity-scoped memory.

Proof:
- Integration test exercises broker fan-out with workflow DAG dependencies.
