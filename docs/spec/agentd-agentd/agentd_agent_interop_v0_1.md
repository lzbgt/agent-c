# Agentd ⇄ Agentd Collaboration Interop (v0.1)

Status: draft / executable via workflow `kind:"agentd_call"` (see below).

Date: 2026-02-05

This document defines a minimal, **agentic-framework-native** collaboration primitive between two `agentd` instances.
The goal is to make multi-agent systems practical even when the underlying LLM services are stateless and context bounded:

- **durable continuity** (retryable, idempotent)
- **cheap correctness surfaces** (deterministic polling + terminal statuses)
- **low coupling** (HTTP JSON only; no shared DB assumptions)

Non-goals (v0.1):
- Federated identity/trust roots and signed attestation (see Interop v0.4 in `TODOS.md`)
- A generic “remote RPC” API surface (we start with durable workflow handoff because it composes)

## 1) Transport and endpoints

Agentd-to-agentd collaboration uses **HTTP** with the existing `agentd` public API:

- Remote submit:
  - `POST /api/v1/workflow/submit`
- Remote poll:
  - `GET /api/v1/workflow?workflow_id=...&include_results=1[&include_tasks=1]`

The caller is expected to be a durable workflow task running inside another `agentd` instance.

## 2) Execution model: `workflow_submit_and_wait`

The baseline collaboration operation is:

1. Submit a workflow to the remote agent (`POST /api/v1/workflow/submit`).
2. Poll until the remote workflow reaches a terminal status (`done|error|cancelled`).
3. Surface the remote workflow’s final response JSON as part of the local task result.

This composes with existing durable workflows:
- You can “fan out” to multiple remote agents with `agentd_parallel` (agent-to-agent), `delegate_parallel` (LLM-level), or `edge_parallel` (node-level).
- You can “join” on correctness using `kind:"aggregate"` and deterministic pointers into the returned JSON.

## 3) Idempotency and retries (critical)

Because a workflow task can be retried (daemon restart, worker crash, transient network failure), the remote submission must be idempotent.

Mechanisms:
- The remote submit body should include a stable `idempotency_key`.
- The local `kind:"agentd_call"` task:
  - auto-defaults an `idempotency_key` when missing (derived from the local task `trace_id`)
  - reuses a previously returned `remote workflow_id` from its prior result JSON when present

This yields “at-least-once transport calls” but “exactly-once remote workflow creation (best-effort)”.

## 4) Security model (v0.1)

This is intentionally conservative:
- `kind:"agentd_call"` is gated by the same daemon flag as `kind:"http_json"`:
  - Start the daemon with `--workflow-enable-http-tasks` (or env `AGENTD_WORKFLOW_ENABLE_HTTP_TASKS=1`)
  - Rationale: both features are outbound HTTP and carry SSRF risk.
- Optional hardening: restrict outbound targets with:
  - `--workflow-http-allow-host <host[:port]>` (repeatable), or
  - env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`
  - `--workflow-http-allow-cidr <cidr>` (repeatable), or env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
  - `--workflow-http-deny-private`, or env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
- Do not persist secrets into workflows:
  - Use `agentd_call.bearer_env` to reference an env var name containing a bearer token.
  - Only the env var name is persisted; the secret value is read at runtime.

Hardening planned for a later interop revision:
- explicit allowlist of remote base URLs / CIDRs
- mTLS or signature-based trust roots

## 5) Workflow task: `kind:"agentd_call"`

This interop is made executable via a deterministic workflow task:

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

Notes:
- The nested `workflow` object is the body sent to the remote `/api/v1/workflow/submit`.
- The local engine will inject a default `trace_id` and `idempotency_key` when missing.

## 6) Result shape (high level)

The task returns:
- `ok`: whether the *remote workflow* finished successfully (`status:"done"`)
- `agentd.workflow_id`: remote workflow id
- `agentd.final`: best-effort parsed JSON of the final poll response (`GET /api/v1/workflow?...`)

This enables downstream deterministic references like:

- `${task.REMOTE.json:/agentd/final/workflow/status}`
- `{"$ref":"task.REMOTE.json:/agentd/final/result/results_by_task/W/assistant_text"}`
