# Durable effects and reconciliation

`kind: http_json` POST actions now have a durable effect boundary. The scheduler can recover a task after a crash without blindly repeating its external write. GET remains a read-only HTTP task. Device assignments have a separate atomic identity/admission/outbox boundary and signed original-result receipt resolver described in [DEVICE_ASSIGNMENTS.md](DEVICE_ASSIGNMENTS.md). Device recovery shares the reconcile endpoint but accepts no URL or caller-supplied outcome. Explicit deterministic `host_tool` actions have the guarded journal boundary below. Tools chosen inside an LLM turn and capsules do not acquire that guarantee merely by belonging to the same workflow.


## Deterministic host tools

A workflow or SOP action can invoke one registered host tool without an LLM:

```json
{
  "task_id": "maintain",
  "kind": "host_tool",
  "max_attempts": 3,
  "host_tool": {
    "name": "proc_exec",
    "arguments": {"argv": ["python3", "/srv/maintenance/check_inventory.py"]},
    "timeout_ms": 30000
  }
}
```

This uses the daemon's existing `--tools host`, host policy, tool allow/deny lists and explicitly configured approval rules. Process execution is available only when the daemon already permits it (`yolo`); requests cannot elevate that setting. In scoped mode the deterministic file-tool boundary is rooted in the daemon state directory, rejects symlink escapes and does not attach the legacy unrestricted session context. Unrestricted session-backed tools use the existing session behavior. This boundary does not enable extension tools or silently change policy settings.

The stable effect identity belongs to the workflow/task, not its scheduler attempt. The request fingerprint pins the protocol, tool name/arguments, configured timeout, execution root and session context. Changed requests or scopes cannot reuse that identity. Arguments and bounded results are persisted application data: use host-side credential locators rather than inline secrets.

The journal admits a dispatch only after configured policy/approval checks. A denied or cancelled approval leaves no admitted effect. A saved terminal result is replayed without invoking the tool, charging another tool call or asking for another execution approval. It is exposed as `host_tool.result`; process output is also the task's `assistant_text`. A completed nonzero exit is a saved failure, not permission to repeat a command that may already have partially changed something.

Crash before admission can resume. Crash after admission without a saved terminal response, timeout, interrupted process, malformed/oversized result or failed receipt persistence produces `host_tool_outcome_unknown`, never an automatic repeat. Independent workflow branches continue; dependents wait. The UI explains the uncertainty and deliberately offers no HTTP receipt URL for host tools. The reconciliation endpoint rejects invented host receipts and URLs. There is no generic host resolver or caller-declared idempotency mode in this slice. Inspect the original operation and downstream effects; a future trusted adapter must establish a terminal outcome before recovery can be automated.

The normal workflow reservation/settlement ledger remains active. Each actual invocation reports one tool call and zero provider tokens; receipt replay reports zero tool calls. Interrupted grants retain their conservative charge. The configured timeout (1–300000 ms) and workflow elapsed grant cancel cooperative tools and bound process execution; non-cooperative adapters can still overrun and are not certified strict meters. Output capture is capped, not silently retried.

This protects explicit deterministic `host_tool` actions, including SOP actions. It does **not** journal arbitrary tools chosen inside an LLM turn, certify all subprocess descendants have stopped, prove a business transaction committed, or guarantee exactly-once physical/external execution. A new workflow/task identity is a new action, not a safe retry of an uncertain old one.

Verification: `workflow_host_tool_tests` covers identity conflicts, policy/scope/argument limits, approval cancellation, cached receipts without fresh approval, malformed/interrupted results and capacity/owner stops. On POSIX hosts `agentd_workflow_host_tool_recovery_smoke` kills real daemons before dispatch, after a real file write, and after durable receipt capture. It also checks nonzero exits, timeout, budget admission, preserved cancellation, SOP invocation and one-worker independent progress.

## Default HTTP contract

A POST defaults to `effect.mode: guarded`. After URL/policy/credential validation, the runtime derives a stable effect ID from workflow/task identity and a canonical request hash from the protocol, method, URL, body, case-normalized headers and bearer environment reference. The secret bearer value is not included. The business account/namespace represented by that reference must remain consistent across recovery.

The daemon commits `prepared`, then admits `dispatching` before the HTTP request. It saves the bounded response as `completed` or records an ambiguous transport outcome as `unknown`. A completed exchange is a saved HTTP receipt, not proof that every business invariant passed. Non-2xx responses remain failures. A saved response is reused when a scheduler attempt is replayed, including when a later validator rejects that response.

For a guarded write, a missing response, lost connection, oversized response or recovered `dispatching` record never authorizes an automatic repeat. A saved non-2xx response also does not trigger repeated writes. The task either queries its configured receipt adapter or returns an explicit `external_effect_unknown` blocker with effect/workflow/task IDs, observed recovery information and the action needed to establish the outcome. It does not open a permission dialog.

Workflow finalization now keeps independent runnable branches eligible after another branch fails, including with a single worker. Once those branches settle, a hard failure still makes the overall workflow fail. Dependents of the failed action remain unexecuted. This behavior does not turn a hard failure into a successful process.

## Destination-supported idempotency

```json
{
  "task_id": "create_order",
  "kind": "http_json",
  "max_attempts": 3,
  "http_json": {
    "url": "https://erp.example/orders",
    "body": {"order_ref": "batch-42"},
    "effect": {"mode": "idempotent"}
  }
}
```

Use `idempotent` only when the destination enforces the idempotency key for the same operation and request for the entire retry horizon. The runtime sends these server-owned headers:

- `Idempotency-Key`: stable `fx_…` identity across dispatch attempts.
- `X-Agent-Effect-Hash`: canonical request fingerprint.
- `X-Agent-Effect-Attempt`: monotonically increasing dispatch number.
- `X-Agent-Effect-Protocol`: `agentd.effect.v1`.

Caller overrides and duplicate case-insensitive header names are rejected. A header alone does not make a service idempotent. The explicit mode permits repeating an uncertain or transiently failed request with the same key; the destination contract must prevent a duplicate business effect. Workflow submission dedupe is still a different boundary.

## Receipt adapter

A guarded action can provide `effect.reconcile_url`. It must use the write URL's exact scheme/authority and no fragment; implicit/explicit default-port aliases are intentionally not treated as the same origin. The same authorization reference is used, and the ordinary outbound/DNS-pinning policy is checked for the GET too. Reconciliation is bounded to ten seconds and 512 KiB per lookup.

The runtime appends `effect_id`, `attempt` and `request_hash` query parameters. A response must identify the exact operation and attempt:

```json
{
  "schema": "agentd.effect.receipt.v1",
  "effect_id": "fx_<identity>",
  "request_hash": "sha256:<request-fingerprint>",
  "attempt": 1,
  "state": "committed",
  "final": true,
  "result": {"status": 200, "response_json": {"order_id": "42"}}
}
```

`committed` with a matching terminal identity and 2xx result settles the original action without another POST. `pending` only schedules another lookup within the task retry policy. `not_applied` with `final: true` permits a new dispatch **only under the adapter contract that the previous attempt is closed/fenced and can no longer commit**. An absent lookup, a 404, mismatched hash/attempt, an unversioned response or `final: false` is insufficient. Do not implement this adapter as an eventually consistent search for an order that may still be in flight.

The receipt endpoint is trusted application infrastructure, not model-authored evidence. An adapter that lies about terminal closure can invalidate the guarantee. Real business/device adapters need their own transactional identity/receipt implementation and retention horizon.

## Resume after an observed fix

The WebUI workflow detail panel shows actual effect state and offers **Reconcile outcome** for a failed uncertain action. Agents or operators can use the same authenticated endpoint without a permission round trip:

```sh
curl -X POST "$AGENTD_BASE/api/v1/workflow/reconcile" \
  -H 'Content-Type: application/json' \
  -d '{"workflow_id":"wf_42","task_id":"create_order","reconcile_url":"https://erp.example/receipts"}'
```

Add the deployment's normal authentication header when needed. Omit `reconcile_url` to reuse a configured adapter. This persists an operational resolver override in the effect row and requeues the existing task, retaining its request identity, usage counters and completed independent tasks. It extends exhausted task attempts by one if necessary, while workflow budgets and deadlines remain effective. It does not clear owner cancellation. Duplicate/conflicting active requests return 409; invalid/cross-origin URLs return 400.

The endpoint does not accept a model's claim that an effect happened and does not force a POST. The next attempt must still obtain a matching terminal receipt or satisfy the explicit destination-idempotency contract. A verified receipt automatically replaces the old failed result and releases downstream work. Operator stops remain authoritative. Workflow finalization compares the task snapshot transactionally so a stale finalizer cannot overwrite this requeue or a cancellation.

## Storage and verification

Schema v34 adds `workflow_effects`, keyed by effect ID with a unique workflow/task pair, request hash, mode, state, revision, dispatch count, timestamps, receipt and resolver. Revisions reject stale claims/receipts. SQLite uses WAL with `synchronous=FULL`; process-crash recovery is tested, while disk/filesystem hardware durability is not certified. Receipt capture is capped at 32 MiB; metadata polling excludes receipt contents from its SQL projection. Treat the DB as sensitive application data and include the new table in backup/erase procedures.

GET `/api/v1/workflow` includes an `effects` metadata array even when an attempt died before writing a task result. Network-call usage counts actual GET/POST calls in a recovered attempt; replaying a cached receipt counts zero network calls. Workflow attempt grants now reserve capacity and settle usage atomically; interrupted grants remain conservatively charged by the [budget ledger](WORKFLOW_BUDGETS.md). That charge is distinct from measured network usage and is not an invoice inferred from the effect journal.

`workflow_effect_tests` checks stable identity, payload conflicts, stale dispatch/receipt rejection, valid terminal closure, rejected nonfinal/mismatched proofs, idempotent recovery and preservation of owner stops. `agentd_workflow_effect_recovery_smoke` uses a real local HTTP service and process kills at three boundaries: before dispatch, after destination commit/before response, and after durable receipt/before task completion. It also proves single-worker independent progress, human/agent-triggered reconciliation without another write, and cancellation rejection. These tests establish this HTTP protocol slice; they are not an exactly-once guarantee for arbitrary external systems or a distributed execution lease.
