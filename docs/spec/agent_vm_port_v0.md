# Agent VM Port v0 (Draft Integration Contract)

Date: 2026-02-05

Status: Draft (integration contract proposal)

Purpose:
- Define a minimal, explicit “port” boundary so a future deterministic VM/language runtime (e.g. `oren-lang`)
  can plug into `agentd` **without** entangling the core with filesystem/env/network dependencies.

This is designed to maximize future leverage even if the VM project is not prioritized today:
we can build the *surface* now and swap in implementations later.

Update (shipped path):
- `agentd` workflows now support deterministic VM execution as a task kind: `kind: "avm_capsule"`.
  This is the first concrete “VM port” surface in production code (out-of-process AVM runner).
  See `docs/spec/avm_capsule_run_v0.md` and the smoke test `agentd_workflow_avm_capsule_smoke`.

## 1) Design constraints (must hold)

1) Host-controlled safety:
   - The VM cannot bypass platform policy.
   - The VM must call host callbacks for tool execution.

2) Determinism is a feature:
   - VM execution should be reproducible given the same inputs and the same host callback outputs.

3) Resource-bounded:
   - Host supplies explicit budgets (time/steps/memory/output bytes).

4) No env/fs assumptions:
   - The contract must work on embedded/VM targets that have no POSIX or `getenv()`.

## 2) Core abstraction: Program + Invocation

### 2.1 Program representation

The host treats a “program” as an opaque artifact:
- `program_kind`: string (e.g. `"oren_bytecode"`, `"wasm"`, `"dsl_json"`)
- `program_bytes`: bytes (or base64 for JSON transport)
- `program_sha256`: optional integrity fingerprint

Concrete mapping for `oren-lang` AVM:
- `program_kind = "oren_obc"`
- `program_bytes = <.obc bytes>`
- optional governance surfaces in AVM include:
  - `RESULT_HASH`, `STATE_HASH`, `TRACE_HASH`
  - job object scanning without execute: `avm --print-job-json <file.obc>`

The host is responsible for:
- storing/loading program artifacts
- selecting the runtime that can execute them

### 2.2 Invocation

Every invocation is:
- `(program, entrypoint, input_json, context)` → `(ok, output_json, events[])`

Where:
- `entrypoint`: string (default `"main"`)
- `input_json`: JSON object string (host-defined schema per use-case)
- `context`: host-provided execution context fields (ids, timestamps, budgets)

## 3) Host callback surface (minimal)

The VM may request host actions; the host decides if allowed.

### 3.1 `tool_call`

Request:
- `name`: tool name
- `args`: JSON object
- `timeout_ms`: optional

Response:
- `ok`: bool
- `error`: string?
- `data`: JSON object?

Notes:
- This maps directly to existing tool schemas in `agentd`.
- Host must enforce safety/rate limits (VM cannot override).

Concrete mapping for `oren-lang` AVM:
- tool calls are “effectful domains” and should be surfaced as `CALL_NATIVE2(domain, op, nargs)` in bytecode.
- the host runtime enforces:
  - capability allow mask (`allowed_domains`)
  - budgets (gas/time/mem/io/log)
  - record/replay (when deterministic/replay modes are used)

### 3.2 `emit_event`

Append a durable event record:
- `type`: string
- `data`: JSON object

Host decides where it lands:
- workflow events table
- edge_workflow_events table
- trace timeline, etc.

### 3.3 `get_time`

Return:
- `now_utc_ms`

Notes:
- VM must not assume wall clock exists.

## 4) First use-cases (highest leverage)

These are the most “multiply gain” use-cases for a VM layer in this repo.

### 4.1 Workflow aggregation nodes (platform)

Replace “LLM aggregator” with deterministic aggregation:
- `first_ok`
- `all_ok`
- `quorum`
- `collect`

### 4.2 Retry/backoff policy (edge workflows)

Given:
- step state history + attempts + deadlines + error categories

Compute:
- next retry time
- whether to escalate or cancel

### 4.3 Memory consolidation transform

Given:
- structured memory entries + evidence links

Compute:
- consolidated entry + supersedes links

## 5) Verification requirements

To keep correctness high:
- A VM module must be runnable in a deterministic “stub host” harness.
- The harness must be able to replay the same invocation and get the same outputs/events.

## 6) Non-goals for v0

- Embedding the VM in `agent_core` immediately
- A full package manager or “ecosystem registry”
- Allowing VM code to read arbitrary host files

## 7) Budget and policy knobs (aligning with AVM realities)

Even if the VM runtime is not AVM, these knobs are high-leverage and should exist in the port.

Recommended context keys:
- `allowed_domains_mask` (bitmask; deny-by-default in secure modes)
- `gas_limit`, `deadline_utc_ms`
- `mem_bytes`, `io_bytes`, `log_bytes`
- `deterministic` (bool)
- `record_log_bytes` / `replay_log_bytes` (bytes) to support “log as data”

Concrete alignment with `oren-lang` nested universes:
- `oren_avm_run_obc_bytes(child_obc_bytes, cfg_map)` already uses:
  - `allowed_domains`, `gas_limit`, `deadline_ns`, `mem_bytes`, `io_bytes`, `log_bytes`
  - virtual backend toggles and fixtures as bytes (`vfs_fixtures`, `proc_fixtures`, `net_fixtures`)
  - returns `result_hash`, `state_hash`, and `record_log` bytes
