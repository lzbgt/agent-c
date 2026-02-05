# Roadmap / TODOs (highest leverage)

Date: 2026-02-05

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
- Embedded bring-up helper: `agent_core` now includes UM‑BMP/UM‑EAIS interop helpers (`agent/edge_interop.h`)
  for id-safe validation/sanitization + message type constants (reduces node/platform drift).
  - Proof: `ctest` includes `agent_core_tests`.
- “Docs as truth” guardrails (DB schema doc + OpenAPI sanity checks).
  - Proof: `ctest` includes `docs_sanity_tests` + `openapi_sanity_tests`.
- UM‑EAIS contract is now executable (best-effort) via machine-readable artifacts:
  - JSON Schemas: `docs/spec/um-eais/schema/`
  - Golden transcript fixtures: `docs/spec/um-eais/fixtures/`
  - Proof: `ctest` includes `um_eais_spec_sanity_tests`, `agentd_edge_interop_transcript_replay_smoke`,
    and `agentd_edge_interop_task_loop_replay_smoke`.
- UM‑BMP crash-window correctness: node-initiated handoff messages are replayable even if `msg_id` is deduped (at-least-once),
  preventing permanent drops when a crash happens after persisting `edge_inbox_messages` but before applying side effects.
  - Proof: `ctest` includes `agentd_edge_message_dedupe_replay_handoff_smoke`.
- UM‑BMP crash-window correctness (generalized): `edge_inbox_messages` now tracks a `processed` marker so **all** message types can be safely
  retried with the same `msg_id` after a crash window (covers `TASK_DONE`, `TASK_EVENT`, etc; not only handoffs).
  - Proof: `ctest` includes `agentd_edge_message_dedupe_replay_task_done_smoke`.
- `trace_id` correlation end-to-end, plus a merged trace timeline across broker ⇄ agentd.
  - Proof: `ctest` includes `agentd_trace_id_smoke`.
- Oren AVM governance endpoints (scan-before-execute; out-of-process).
  - Endpoints:
    - `POST /api/v1/avm/job_scan` (`avm --print-job-json`)
    - `POST /api/v1/avm/policy_scan` (`avm --print-policy-json`)
    - `POST /api/v1/avm/inspect` (`avm --inspect-json`)
    - `POST /api/v1/avm/verify_strict` (`avm --verify-strict`)
    - `POST /api/v1/avm/trace_hash` (`avm --print-trace-hash`)
    - `POST /api/v1/avm/capsule_run` (exec gated; `avm --capsule --print-run-json ...`)
  - Bring-up helper: `tools/oren_avm_bringup.sh` builds/locates `../oren-lang/avm` and prints its absolute path for `AGENTD_AVM_BIN`.
  - Capsule task helper: `tools/oren_capsule_task.sh` compiles `.oren` → `.obc` and emits a ready `kind:"avm_capsule"` task JSON.
  - Proof: `ctest` includes `agentd_avm_job_scan_smoke` (stubbed AVM runner for determinism/CI).
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
  - prompt templating (simple but powerful): `${task.<id>.assistant_text}` and `${task.<id>.json:/json_pointer}`
  - v2 template expansion: templates are expanded across the full task request JSON (not only `prompt`), enabling deterministic dataflow into `edge_invoke.args` and other structured fields.
  - v2 JSON-native embedding: `{"$ref":"task.<id>.json:/ptr"}` replaces the entire value with embedded JSON (not a string), enabling structured payload/args wiring.
  - v2.1 workflow inputs: tasks may carry `inputs` and reference them via `${input.<name>...}` and `{"$ref":"input.<name>..."}` for cleaner dataflow.
  - Proof: `ctest` includes `agentd_workflow_inputs_smoke`.
  - Optional workflow submit dependency inference: `infer_depends_on: true` scans for `${task.<id>...}` / `$ref:"task.<id>..."` and merges into `depends_on`.
  - Proof: `ctest` includes `agentd_workflow_infer_deps_smoke`.
  - Proof: `ctest` includes `agentd_workflow_smoke` (validates DAG ordering + templating + restart recovery).
- Workflow engine maintainability refactor:
  - JSON Pointer helper promoted to `daemon/src/json_util.*` (`json_pointer_get`).
  - Workflow template expander extracted into `daemon/src/workflow_templates.*` (keeps `workflow_engine.cpp` under ~2000 lines).
  - Workflow aggregation/join logic extracted into `daemon/src/workflow_aggregate.*` (keeps `workflow_engine.cpp` lean and SOLID).
- Durable workflows can now run deterministic AVM capsule tasks (no LLM required):
  - Task kind: `kind: "avm_capsule"`
  - Task payload: `capsule: { obc_base64, timeout_ms, gas, mem_bytes, ... }` (same schema as `POST /api/v1/avm/capsule_run`)
  - Result shape: `result.results_by_task[task_id].avm.{run,result_hash,trace_hash,state_hash,...}`
  - Proof: `ctest` includes `agentd_workflow_avm_capsule_smoke` (runs AVM capsule, then templates its result into an LLM stub task).
- Durable workflows now support deterministic aggregation/join tasks (no LLM required):
  - Task kind: `kind: "aggregate"` (modes: `quorum_hashes`, `first_ok`, `best_of_n`, `collect`)
  - Use-case: compare deterministic hash surfaces (e.g. AVM `result_hash` / `trace_hash`) across N runs/nodes and require quorum.
  - Proof: `ctest` includes `agentd_workflow_aggregate_quorum_smoke`, `agentd_workflow_aggregate_first_ok_smoke`, and `agentd_workflow_aggregate_best_of_n_smoke`.
- Durable workflows now support UM‑EAIS edge collaboration tasks (no LLM required):
  - Task kind: `kind: "edge_invoke"` (dispatches `TASK_ASSIGN mode:"invoke"` and waits for `TASK_DONE`)
  - Also supports `mode:"agent"` (dispatches `TASK_ASSIGN mode:"agent"` with a prompt/payload for embedded `agent_core`)
  - Use-case: mix deterministic compute + LLM reasoning + real-world actuation in one durable DAG.
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_smoke`, `agentd_workflow_edge_invoke_template_args_smoke`, `agentd_workflow_edge_agent_smoke`, and `agentd_workflow_edge_agent_ref_payload_smoke`.
- UM‑EAIS platform extensions (node → platform workflow handoff) are now proven end-to-end:
  - `WORKFLOW_SUBMIT` and `WORKFLOW_CANCEL` can be ingested via `POST /api/v1/edge/message` and drive the edge workflow runner.
  - Best-effort node ACKs: outbox `WORKFLOW_ACK` is enqueued for both submit and cancel (non-HTTP transports).
  - Proof: `ctest` includes `agentd_edge_workflow_submit_message_smoke` and `agentd_edge_workflow_cancel_message_smoke`.
- Durable workflows can now be handed off via the same UM‑BMP ingress (MCU-friendly, transport-agnostic):
  - `DURABLE_WORKFLOW_SUBMIT` forwards to `POST /api/v1/workflow/submit` (platform durable orchestration)
  - `DURABLE_WORKFLOW_CANCEL` forwards to `POST /api/v1/workflow/cancel`
  - Best-effort node ACK: outbox `DURABLE_WORKFLOW_ACK {op, workflow_id, ok}`
  - Proof: `ctest` includes `agentd_edge_durable_workflow_submit_message_smoke`.
  - Correctness: default `idempotency_key` prefers `edge_wf:<workflow_id>` when `workflow_id` is caller-provided (stable retries).
- UM‑EAIS node capability cache correctness: when a node reports a new `caps_sha256`, the platform invalidates cached
  `manifest_json/tools_json/tags_json` and re-requests a full manifest (prevents stale routing).
  - Proof: `ctest` includes `agentd_edge_interop_smoke`.
- UM‑EAIS `caps_sha256` correctness hardening:
  - `caps_sha256` is now treated as a real sha256 token (`(sha256:)?[0-9a-fA-F]{64}`) across schemas + platform ingestion.
  - Embedded `agent_core` exposes the same validator (`agent_umbmp_sha256_token_is_safe`) so MCU firmware can self-check.
  - Proof: `ctest` includes `agentd_edge_caps_sha256_validation_smoke`, `agentd_edge_interop_transcript_replay_smoke`,
    and `agent_core_tests` (edge interop module tests).
- Workflow engine now supports **soft-fail** tasks via `allow_error: true`:
  - If a task ends `status="error"` and `allow_error=true`, the workflow can still complete `done` (so long as no “hard” errors remain).
  - Dependencies treat `(status=error + allow_error=true)` as satisfied; aggregation tasks can also depend on any terminal dependency to compute joins over errors.
  - Proof: `ctest` includes `agentd_workflow_aggregate_first_ok_smoke` (uses an `allow_error` failing branch + `mode:"first_ok"` join).
- Workflow engine supports expanded deterministic expectations (`expect`) for correctness:
  - `json_pointer_exists`, `json_pointer_regex`, `json_pointer_number_between` (in addition to `ok`, `assistant_text_contains`, `json_pointer_equals`).
  - Proof: `ctest` includes `agentd_workflow_expect_extended_smoke` (includes a passing task and an allow_error failing task).
- Workflow engine supports task-controlled rescheduling for polling/async patterns:
  - Tasks may return `retryable=true` and `retry_in_ms` to control the requeue delay (instead of fixed polling/backoff).
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_smoke` (wait/poll loop uses task-controlled retry delay).
- Resumable async jobs: `run_async` requests persist enough state to resume after daemon restart (at-least-once semantics).
  - Proof: `ctest` includes `agentd_job_restart_durability_smoke` (restart mid-job → finishes done).
- Memory v2 (retrieval): `memory_search` now prefers a ranked on-disk index (SQLite FTS5) when available, with automatic fallback to bounded substring scan.
  - Also: structured memory updates (`memory_put(entries)`) produce rolling JSON checkpoints under `memory/checkpoints/` for time-correlation.
  - Proof: `ctest` includes `test_host_toolset` (memory tools) plus existing daemon smokes that exercise host tools.
- Memory v2.1 (rolling consolidation, deterministic): `agentd` can promote explicit `@mem ...` markers from daily memory into structured memory via `POST /api/v1/memory/consolidate`, and can run it periodically with `--memory-consolidate-interval-ms`.
  - Proof: `ctest` includes `agentd_memory_consolidate_smoke` (idempotent; no checkpoint churn on second run).
- Durable workflows can now trigger deterministic rolling consolidation as a task:
  - Task kind: `kind: "memory_consolidate"` (scans `memory/YYYY-MM-DD.md` markers into `memory/STRUCTURED.md`)
  - Proof: `ctest` includes `agentd_workflow_memory_consolidate_smoke`.
- Memory v2.2 (versioned facts + evidence): structured memory entries now keep bounded `sources[]` (evidence) and `versions[]` (superseded history) under schema `agent_memory_v2`.
  - Proof: `ctest` includes `host_toolset_tests` assertions that validate schema upgrade + history retention.
- Durable workflows now support deterministic memory updates (correctness-gated + correlated):
  - Task kind: `kind: "memory_put"` (structured upsert via host tool `memory_put(entries=[...])`)
  - Engine injects workflow correlation evidence into `entries[].source` when missing (`workflow:<id> task:<id> trace:<id> ...`).
  - Proof: `ctest` includes `agentd_workflow_memory_put_smoke`.
- Workflow event log + streaming (durable): `agentd` persists workflow events (`workflow_events`) and exposes:
  - `GET /api/v1/workflow/events` (paged)
  - `GET /api/v1/workflow/stream` (SSE; ends with `workflow_done`)
  - Proof: `ctest` includes `agentd_workflow_stream_smoke`.
- Scheduler knobs (efficiency precursor): `agentd` supports configuring background engine concurrency/polling:
  - `--job-concurrency`, `--job-poll-ms`, `--workflow-concurrency`, `--workflow-poll-ms` (also reflected in `/api/v1/config`)
- Workflow scheduler fairness/budget caps (load resilience precursor):
  - `--workflow-max-inflight-per-workflow` (prevents one fan-out workflow monopolizing all workers)
  - `--workflow-max-inflight-per-session` (optional multi-tenant cap; default disabled)
  - Proof: `ctest` includes `agentd_workflow_inflight_cap_smoke` (asserts non-overlap under cap=1).
- Per-session fairness cap is now proven (multi-tenant guard):
  - Proof: `ctest` includes `agentd_workflow_inflight_session_cap_smoke`.
- Workflow scheduler scan fairness v2.0 (beyond caps; prevents LIMIT-starvation):
  - Oversampled workflow scan (bounded by DB clamp) so older workflows can’t be permanently excluded by `LIMIT`.
  - Session-aware scan order (round-robin across session buckets) to reduce multi-tenant starvation risk under heavy load.
  - Proof: `ctest` includes `agentd_workflow_scan_fairness_smoke`.
- Workflow admission control / submit-time backpressure (v1.5):
  - Caps total inflight workflow tasks (`queued|running`) at submit time (returns HTTP `429` with `retry_after_ms`).
  - Knobs:
    - `--workflow-admit-max-inflight-tasks-per-session` (env `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_PER_SESSION`)
    - `--workflow-admit-max-inflight-tasks-total` (env `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_TOTAL`)
  - Proof: `ctest` includes `agentd_workflow_admission_control_smoke`.
- Deterministic workflow-only delay task (`kind:"delay"`) for scheduling tests and wait gates (no LLM required).
  - Proof: `ctest` includes `agentd_workflow_inflight_cap_smoke`.
- Workflow correctness: tool-call constraints in `expect` (enforce “must call / must not call” deterministically).
  - Proof: `ctest` includes `agentd_workflow_expect_tool_calls_smoke` (uses ext_echo tool plugin).
- Workflow scheduler stats endpoint (queue pressure metrics):
  - `GET /api/v1/workflow/stats`
  - Proof: `ctest` includes `agentd_workflow_stats_smoke`.
- Workflow spec introspection:
  - `GET /api/v1/workflow?workflow_id=...&include_spec=1` returns redacted `spec_json` (+ parsed `spec` when valid).
  - Proof: `ctest` includes `agentd_workflow_get_spec_smoke`.
- Workflow deadline (scheduler-level):
  - Submit `deadline_unix_ms` to cancel queued tasks after a wall-clock cutoff; running tasks are cooperatively cancelled at safe boundaries (best-effort).
  - Proof: `ctest` includes `agentd_workflow_deadline_smoke`.
- Workflow submit idempotency + DB-backed policy columns:
  - Submit `idempotency_key` to dedupe workflow submits (safe retries / at-least-once upstream delivery).
  - Workflows now persist `deadline_unix_ms` and `idempotency_key` as dedicated DB columns (schema v17), so the scheduler does not depend on parsing `spec_json`.
  - Proof: `ctest` includes `agentd_workflow_idempotency_smoke` and `agent_db_tests` asserts the new columns exist.
- Workflow cooperative cancellation for running tasks (v1.4):
  - `POST /api/v1/workflow/cancel` now cancels running tasks at safe boundaries (tool loop + long-running host tools).
  - Deadline cancellation (`deadline_unix_ms`) also cancels running tasks best-effort.
  - Proof: `ctest` includes `agentd_workflow_cancel_running_smoke`.
- Workflow agent collaboration primitive (v1.6):
  - Task kind: `kind:"delegate"` runs a sequence of candidate sub-requests (fallback) with optional per-attempt `expect`,
    returning `delegate.attempts[]` + `delegate.chosen_id` and surfacing chosen `assistant_text`.
  - Proof: `ctest` includes `agentd_workflow_delegate_smoke`.
- Scheduler-visible parallel collaboration macro (v1.6.1):
  - Submit-time task macro: `kind:"delegate_parallel"` expands into N attempt tasks + a deterministic `kind:"aggregate"` join (mode `first_ok`).
  - Attempt tasks default to `allow_error=true` so one failed attempt doesn’t fail the workflow; the join fails only if all attempts fail.
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_macro_smoke`.
- Parallel collaboration macro join customization (v1.7.0):
  - `delegate_parallel` can now pass `delegate.aggregate` to pick a deterministic join strategy (e.g. `mode:"best_of_n"`).
  - Server overwrites `aggregate.task_ids` with the derived attempt task IDs (`<task_id>:<attempt_id>`).
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_best_of_n_smoke`.

## P0 (next: maximize autonomous continuity + correctness)

### Reweighted next 5 (highest compound impact)

Priority order (reweighted after `delegate_parallel` join customization shipped; sections below are kept stable for diff readability):

1) **Interop spec v0.2 (MCU/edge handoff)** — attestation + correlation; keep the contract executable.
2) **Budgets v0.5** — host-tool budgets + streaming token usage coverage + stats surfacing (including cheap budget telemetry polling).
3) **Scheduling policy v2.3 (DRR + cost-aware quanta)** — move beyond WRR to cost-aware, budget-composing fairness.
4) **Memory v2.3 (timeline + correlation)** — query-by-trace/workflow windows; rolling consolidation correlation as time advances.
5) **Agent collaboration v2.1** — parallel fan-out with deterministic joins (best_of_n/quorum/collect) + per-attempt budgets.

1) **Durable budget enforcement at scheduler level** (correctness + cost predictability)
   - Shipped (v0): workflow-level tool-call budget `workflow_limits.max_tool_calls_total` enforced by the workflow engine:
     - clamps LLM/tool tasks’ `max_tool_calls_total` to the remaining workflow budget
     - cancels LLM/tool tasks when the remaining workflow budget is 0 (durable + restart-safe)
     - emits `workflow_budget_exceeded` events and surfaces run telemetry (`tool_calls_total`, `steps_executed`, `elapsed_ms`) for auditing
     - Proof: `ctest` includes `agentd_workflow_budget_tool_calls_smoke`.
   - Shipped (v0.1): workflow-level step/time budgets:
     - `workflow_limits.max_steps_total` (sum of `steps_executed`)
     - `workflow_limits.max_elapsed_ms_total` (sum of `elapsed_ms`, best-effort)
     - Proof: `ctest` includes `agentd_workflow_budget_steps_smoke`.
   - Shipped (v0.2): retry-safe budget charging via DB cumulative counters:
     - `workflow_tasks.tool_calls_total_cum`, `steps_executed_cum`, `elapsed_ms_cum`
     - Fixes undercounting across retries (budget enforcement now charges every attempt)
     - Proof: `ctest` includes `agentd_workflow_budget_retry_charges_smoke` and `agent_db_tests` asserts schema v19.
   - Shipped (v0.3): bulk cancellation of queued tasks when budget exceeded:
     - prevents scheduler thrash by cancelling all `status=queued` tasks in one pass (unclaimed tasks keep `attempt==0`)
     - Proof: `ctest` includes `agentd_workflow_budget_bulk_cancel_smoke`.
	   - Shipped (v0.4): workflow-level token budgets (best-effort; provider-reported):
	     - Budget knob: `workflow_limits.max_total_tokens` (sum of `total_tokens`, provider response `usage.total_tokens`).
	     - Durable counters: `workflow_tasks.prompt_tokens_cum`, `completion_tokens_cum`, `total_tokens_cum` (retry-safe charging).
	     - Run telemetry surfaced per task: `prompt_tokens`, `completion_tokens`, `total_tokens` (aggregated across all LLM calls in the run).
	     - Proof: `ctest` includes `agentd_workflow_budget_tokens_smoke` and `agent_db_tests` asserts schema v21.
	     - Budget telemetry is now visible at the workflow level:
	       - `GET /api/v1/workflow?...` surfaces `workflow_limits`, `workflow_usage`, and `workflow_remaining` (best-effort).
	       - Proof: `ctest` includes `agentd_workflow_budget_tokens_smoke`.
	     - Limitations:
	       - only enforced when providers return `usage` in responses
	       - streaming tool-loop calls may not surface usage (depends on provider streaming schema)
	   - Next: host-tool budgets, enforce token budgets for streaming paths, and surface budget pressure in `/api/v1/workflow/stats`.

2) **Scheduling policy v2 (beyond caps)** (predictable progress under load)
   - Shipped (v2.0): oversampled scan + session-aware round-robin scan order (prevents `LIMIT` starvation under typical load).
   - Proof: `ctest` includes `agentd_workflow_scan_fairness_smoke`.
   - Shipped (v2.1): scheduler DB scan is now oldest-first (priority DESC, created_unix_ms ASC) with an index, so fairness holds even
     when queued workflows exceed the DB scan clamp (512).
   - Proof: `ctest` includes `agentd_workflow_scan_fairness_smoke` (submits >512 workflows; older session still completes early).
   - Shipped (v2.2): explicit fair-queue policy surface (weighted round-robin, session-scoped):
     - Daemon config/flags: `--workflow-fair-queue-policy wrr` (default), weight clamps.
     - Workflow submit knob (requires `allow_sessions=true`): `session_weight` (>=1).
     - Proof: `ctest` includes `agentd_workflow_wrr_session_weight_smoke` (ensures session B is not starved behind session A; session A still dominates early prefix when weight=2).
   - Next (v2.3): graduate from WRR to deficit round-robin (DRR) with cost-aware quanta (e.g. budget pressure, token cost) and durable per-session tokens.

3) **Interop spec hardening for MCU/edge handoff** (ecosystem leverage)
   - Shipped: `DURABLE_WORKFLOW_SUBMIT` / `DURABLE_WORKFLOW_CANCEL` over `POST /api/v1/edge/message` (durable orchestration handoff).
     - Proof: `ctest` includes `agentd_edge_durable_workflow_submit_message_smoke`.
   - Shipped: `agent_core` UM‑BMP interop helpers (id-safe + sanitizer + message type constants).
     - Proof: `ctest` includes `agent_core_tests`.
   - Next: consolidate UM‑EAIS + durable workflow handoff into a single **versioned interop contract** with explicit:
     - idempotency rules (`msg_id` vs `idempotency_key`) and replay guidance for lossy transports (MQTT/LoRa bridges)
     - correlation rules (`workflow_id` / `trace_id` / task trace suffixing)
     - safety defaults (inline API keys forbidden; gateway-auth required)
   - Deliverables (shipped): JSON Schemas (envelope + core + platform extensions) + golden transcript fixtures (replay-ready JSONL).
     - Proof: `ctest` includes `um_eais_spec_sanity_tests`, `agentd_edge_interop_transcript_replay_smoke`,
       and `agentd_edge_interop_task_loop_replay_smoke`.
   - Shipped: normative reliability rules + strict task-loop correlation:
     - Spec: `docs/spec/um-eais/um-eais-v0.1.md` §5.3 (reliability + idempotency)
     - Platform enforcement: `TASK_ACK/TASK_EVENT/TASK_DONE/TASK_FAILED` require `idempotency_key` (reject missing/invalid)
   - Next (v0.2): attestation + trace correlation:
     - standardize `trace.trace_id` propagation for all edge task lifecycle messages
     - extend payload conventions so nodes can report deterministic compute hashes (e.g. AVM `RESULT_HASH/TRACE_HASH`) as task done attestation.

4) **Agent collaboration v2 (budgeted parallel fan-out + join macros)** (power-unleashed)
   - Status: submit-time parallel macro shipped as `kind:"delegate_parallel"` (v1.6.1).
   - Remaining: per-attempt budgets + richer deterministic join strategies (`strict_all_ok`, `quorum_ok`, etc).

5) **Memory ↔ workflow time correlation (next after memory_put)** (time-advancing correctness)
   - Shipped: deterministic workflow `kind:"memory_put"` and deterministic workflow `kind:"memory_consolidate"`.
   - Shipped: evidence hashing for replay/correlation:
     - `memory_put` structured checkpoints now emit `checkpoint_sha256` (sha256 of checkpoint JSON bytes).
     - workflow engine emits `workflow_events` `type="memory_checkpoint"` with `{checkpoint:{path,sha256,ts_utc,...}}` when memory changes.
     - Proof: `ctest` includes `agentd_workflow_memory_put_smoke` and `agentd_workflow_memory_consolidate_smoke`.
   - Shipped: bounded correlation queries (time windows + trace_id evidence needle):
     - `GET /api/v1/memory/checkpoints`
     - `GET /api/v1/memory/correlate?trace_id=...`
     - Proof: `ctest` includes `agentd_memory_correlate_smoke`.
   - Next: extend correlation beyond structured memory:
     - join across workflow events + edge workflow events by `trace_id`
     - add “memory query plan” primitives (bounded time windows + key-prefix filters) for large fleets.

### 1) AVM capsule execution v0 (next: integrate + attest)

Goal:
- Make correctness and replayability a first-class primitive: “code as data capsule” runnable under explicit budgets,
  with deterministic hashing surfaces so results can be validated across time/nodes.

Deliverables:
- Persisted “governance bundle” object (job/policy/inspect/verify/run) so platform code can do scan → run → attest
  with one stable stored object keyed by `job_hash_sha256` / `program_hash_sha256`.
- Workflow + join:
  - (shipped) durable workflows can dispatch a capsule run as a task kind (no LLM required)
  - (shipped) deterministic aggregation/join nodes (`kind:"aggregate"`) can compare `RESULT_HASH` / `TRACE_HASH` across runs/nodes (k-of-n correctness)
  - (next) extend aggregation strategies (`first_ok`, `strict_all_ok`, `collect`, `best_of_n`) and attach node identity to votes for multi-node correctness.
- Edge interop integration:
  - extend UM‑EAIS payload conventions so a node can execute a capsule and report back hashes as “task done” attestation.

Proof:
- Deterministic smoke test with a stub AVM binary + (optional) integration smoke when `../oren-lang` is present.

### 2) Budgets + scheduling policy (fairness, concurrency, backpressure)

Goal:
- Make the framework “strong by default” under load: predictable progress and fairness across jobs/workflows.

Deliverables:
- Fair scheduling:
  - per-session fairness (avoid one client starving others even with high `priority`)
- Budget + backpressure:
  - token/step/tool-call budgets enforced at scheduler level (not just per-run)
  - queue pressure metrics + admission control
- Cancellation semantics:
  - deterministic cancellation propagation (queued → cancelled; running → cooperative cancel)
 - (shipped) per-workflow in-flight cap + round-robin workflow scan start to reduce starvation.

Proof:
- Stress test (deterministic stub provider): submit N workflows/jobs and assert bounded completion time + fairness.

### 3) Workflow engine v2: dataflow + aggregation nodes

Problem:
- DAG ordering is useful, but real workflows need explicit **dataflow** and **aggregation strategies**.

Deliverables:
- Dataflow model:
  - (shipped) `${task.<id>.assistant_text}` and `${task.<id>.json:/ptr}` template expansion across full task request JSON (prompt + structured fields)
  - (shipped) explicit `inputs` map per task (shared variables; supports `${input...}` and `{"$ref":"input..."}`; enables schema validation later)
- Aggregation nodes (no LLM required by default):
  - `first_ok`, `best_of_n`, `strict_all_ok`, `collect`
- Optional LLM aggregator (tools=none by default).

Proof:
- Deterministic stub-server test executes a DAG with dataflow and asserts outputs are wired correctly.

### 4) Workflow streaming + UI surface (without making UI the power source)

Status:
- Shipped v1.5: durable workflow event log + SSE stream + smoke test.

Next:
- UI view for workflow timeline (reuse trace UI patterns), plus filters (by task_id, by event type).

### 5) Memory v2: semantic retrieval + rolling consolidation

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

### 6) Correctness v2: validators + replayability

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
