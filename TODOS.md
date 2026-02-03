# Roadmap / TODOs (highest leverage)

Date: 2026-02-03

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

## P0 (next: maximize autonomous continuity + correctness)

### 1) Durable async job runner (resume `run_async` jobs)

Problem (fact from code):
- Async jobs are DB-backed for *status inspection*, but inflight jobs are marked `interrupted` on restart and do not resume.

Deliverables:
- Add a background “job runner” that can resume queued/running async jobs after daemon restart.
- Persist enough job execution state to re-run safely (same request body, same session/memory policy).
- Add best-effort cancellation propagation across restarts.

Proof:
- New smoke test: submit `run_async`, restart daemon mid-run, verify job finishes (not `interrupted`).

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

Deliverables:
- `GET /api/v1/workflow/stream` (SSE):
  - task state transitions
  - deterministic “workflow_event” records
- UI view for workflow timeline (reuse trace UI patterns).

Proof:
- Smoke test validates SSE event ordering and final terminal event.

### 4) Memory v2: semantic retrieval + rolling consolidation

Problem:
- Stateless LLM calls and bounded context require a memory system that evolves over time:
  - consolidates outdated facts
  - correlates new information with old (conflict resolution)
  - retrieves relevant memory efficiently

Deliverables:
- Retrieval:
  - SQLite FTS5 (if available) for `memory_search` indexing, or embedding index as a pluggable backend.
  - ranking/scoring and dedupe.
- Rolling consolidation worker:
  - periodic consolidation checkpoints (time-based + size-based)
  - versioned facts (supersedes/obsolete markers)
  - deterministic conflict rules (already partially present in structured memory mode).

Proof:
- Unit tests show retrieval ranking and deterministic conflict resolution.

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

