# AVM Capsule Run Endpoint (v0) — agentd ↔ Oren AVM Integration

Date: 2026-02-05

Status: v0 (implemented; rolling)

## 1) Goal (why this exists)

This project’s “power” is not in traces/observability. Power comes from:

- autonomous scheduling continuity (durable + resumable)
- memory that evolves over time with consolidation/correlation
- correctness surfaces that survive stateless LLM calls

Oren’s AVM provides a *deterministic execution substrate* that can be governed and validated by hash surfaces.

This spec defines a minimal `agentd` HTTP endpoint that can:

- accept a `.obc` capsule as bytes (base64)
- run it in **safe capsule mode** (`--capsule`)
- enforce explicit budgets + deterministic knobs
- return machine-readable results + hash tokens

## 2) Goals (v0)

- Allow scoped AVM flags passthrough via an explicit allowlist.
- Enable host-effects under explicit policy and budget controls (FS/PROC/NET).
- Expose full record/replay plumbing (log + snapshot persistence).
- Integrate multi-node quorum/attestation protocol in agentd (with broker coordination).

## 3) Security model

The endpoint is intentionally **gated** because it executes untrusted bytecode:

- Requires daemon auth (when enabled).
- Requires `yolo_default=true` (daemon is allowed to spawn subprocesses).
- Requires `AGENTD_AVM_BIN` to be set (explicit operator choice of which `avm` binary to run).
- Requires `AGENTD_AVM_EXEC=1` (explicit “execution enabled” operator gate).

The endpoint always runs AVM with `--capsule` so AVM applies deny-by-default + strict verification defaults and uses Virtual* backends unless explicitly overridden (v0 forbids overrides).

## 4) API (v0)

### Endpoint

- `POST /api/v1/avm/capsule_run`

### Request JSON (v0)

Required:

- `obc_base64: string` — base64 of `.obc` bytes

Optional (budgets / determinism):

- `timeout_ms: int` — wall-time deadline passed to AVM (`--timeout-ms`), default 2000, clamp `[1..60000]`
- `gas: int` — instruction-step budget (`AVM_GAS`), default 200000, clamp `[1..50_000_000]`
- `mem_bytes: int` — heap budget (`AVM_MEM_BYTES`), default 8_000_000, clamp `[64_000..512_000_000]`
- `io_bytes: int` — FS I/O budget (`AVM_IO_BYTES`), default 0 (no I/O), clamp `[0..512_000_000]`
- `log_bytes: int` — record/replay log budget (`AVM_LOG_BYTES`), default 0, clamp `[0..512_000_000]`
- `deterministic: bool` — sets `AVM_DETERMINISTIC=1` when true (default true)
- `rng_seed: int` — optional (`AVM_RNG_SEED`)
- `time_start_ns: int` — optional (`AVM_TIME_START_NS`)

Optional (allowlist hints, still virtualized):

- `allow_domains: string` — CSV passed to AVM `--allow-domains` (default empty)

### Response JSON (v0)

Success response fields:

- `ok: bool`
- `run: object` — parsed JSON object emitted by AVM `--print-run-json` (current AVM schema: `avm.run.v1`)
- `result_hash: string?` — parsed token `RESULT_HASH <hex>`
- `trace_hash: string?` — parsed token `TRACE_HASH <hex>`
- `state_hash: string?` — parsed token `STATE_HASH <hex>`
- `exit_code: int`
- `timed_out: bool`
- `truncated: bool`
- `stdout: string` — combined stdout/stderr from the AVM subprocess (bounded)

Failure response:

- `ok=false` and error fields, with HTTP status:
  - `403` (yolo disabled or exec disabled)
  - `400` (invalid request)
  - `502` (avm failed)
  - `504` (timed out)

## 5) Implementation notes (agentd)

- Use `posix_spawnp` (no shell) to run the AVM binary.
- Enforce a separate outer timeout in agentd to avoid runaway subprocesses (outer timeout slightly above `timeout_ms`).
- Capture stdout+stderr into a bounded buffer (default cap 1MB).
- Parse the first JSON object found in output as the `run` value.
- Parse hash tokens from remaining output as best-effort fields.

## 6) Fast bring-up (developer workflow)

In this workspace, `oren-lang` typically lives at `../oren-lang`.

- Build/locate AVM:
  - `export AGENTD_AVM_BIN="$(tools/oren_avm_bringup.sh)"`
- Enable execution:
  - `export AGENTD_AVM_EXEC=1`

### Optional: build a capsule task from `.oren` source (one-liner)

If you have the `oren-lang` repo available, you can compile a `.oren` source file into `.obc` and
emit a ready-to-paste workflow task JSON object:

```bash
tools/oren_capsule_task.sh --src /abs/path/to/prog.oren --task-id AVM
```

This produces an `{"task_id":"AVM","kind":"avm_capsule","capsule":{...}}` object suitable for
`POST /api/v1/workflow/submit`.

## 7) Workflow integration (shipped)

Durable workflows support deterministic capsule tasks (no LLM required):

- Task kind: `avm_capsule`
- Task payload: `capsule: { obc_base64, timeout_ms, gas, mem_bytes, ... }`
- Execution: agentd runs the configured AVM binary out-of-process under the same operator gates as `/api/v1/avm/capsule_run`.

Minimal example:

```bash
curl -fsS -H "Content-Type: application/json" -d '{
  "tasks": [
    { "task_id": "AVM", "kind": "avm_capsule", "capsule": { "obc_base64": "...", "timeout_ms": 1000 } }
  ]
}' http://127.0.0.1:8080/api/v1/workflow/submit
```
