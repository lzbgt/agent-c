# Durable effects and reconciliation

`kind: http_json` POST actions now have a durable effect boundary. The scheduler can recover a task after a crash without blindly repeating its external write. GET remains a read-only HTTP task. Device assignments have a separate atomic identity/admission/outbox boundary and signed original-result receipt resolver described in [DEVICE_ASSIGNMENTS.md](DEVICE_ASSIGNMENTS.md). Device recovery shares the reconcile endpoint but accepts no URL or caller-supplied outcome. Explicit deterministic `host_tool` actions have the guarded journal boundary below. Ordinary workflow model runs with host tools have a guarded boundary and, for ephemeral or session-backed ordinary tasks, verified transcript continuation described below. Capsules and delegated model sub-attempts do not acquire that guarantee merely by belonging to the same workflow.


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

This protects explicit deterministic `host_tool` actions, including SOP actions. The separate model-run boundary below covers ordinary model-selected host calls conservatively. Neither boundary certifies all subprocess descendants have stopped, proves a business transaction committed, or guarantees exactly-once physical/external execution. A new workflow/task identity is a new action, not a safe retry of an uncertain old one.

Verification: `workflow_host_tool_tests` covers identity conflicts, policy/scope/argument limits, approval cancellation, cached receipts without fresh approval, malformed/interrupted results and capacity/owner stops. On POSIX hosts `agentd_workflow_host_tool_recovery_smoke` kills real daemons before dispatch, after a real file write, and after durable receipt capture. It also checks nonzero exits, timeout, budget admission, preserved cancellation, SOP invocation and one-worker independent progress.

## Model-selected host tools

Ordinary model workflow tasks (including model actions compiled from an SOP) have a guarded effect boundary when their effective toolset is `host`. Tasks without provider-specific vision prefetch support bounded provider/tool transcript continuation, either ephemeral (`no_session: true`) or tied to a persistent session with the conversation fences below. Vision-prefetch tasks retain the conservative whole-run guard. No new request flag, claimed idempotency key or client-supplied workflow identity is accepted. Direct chat/CLI runs, delegate sub-attempts, capsules and provider-native tools are not covered by this boundary.

The runtime probes the stable workflow/task record before reserving fresh budget. Provider-only failures remain retryable and pure tasks create no model effect row. Immediately before the first actual host-tool invocation, after existing policy and approval checks, it persists an admitted `dispatching` boundary. This includes read-only calls conservatively. The boundary does not alter the existing model-run filesystem scope, sandbox settings, tool registry or extension permissions. Cancellation and deadlines still win.

A successfully finished model run saves a bounded complete run receipt. Recovery reuses that answer and its original event evidence without calling the provider or tools, asking for fresh execution approval, or needing another provider/tool budget grant. The replay reports zero new usage; historical usage appears in `result.effect.original_usage`. The authoritative `budget_accounting.charged`/uncertain ledger retains charges for grants interrupted by a crash, even when legacy task cumulative counters could not be saved.

If a host call is admitted without a durable terminal receipt, a tool times out or returns an invalid/unbounded result, the replay context does not match, or a checkpoint is corrupt, the task returns `needs_input: true`, `retryable: false` and a `model_run_outcome_unknown` blocker. Later calls in the same run are refused after an uncertain tool result. A time-grant expiry cannot turn that refusal back into permission to retry. Independent work can continue within remaining budgets; dependent work does not advance.

### Verified transcript continuation

For supported host-model tasks, the host adapter records the exact provider decisions (including original tool-call IDs) and raw bounded terminal tool results. Before each fresh host call it commits a pending-dispatch marker using the effect revision; after the call it commits the result before returning it to the core. The first marker is still lazy and policy-first. Provider-only tasks create no effect row. Provider requests are fingerprinted, not copied into this journal. A bounded host-adapter sidecar also retains the original reasoning content keyed by tool-call IDs when the current OpenAI-compatible adapter captured it (for example DeepSeek/Moonshot nonstreaming tool turns). It is restored before a fresh provider request; missing required adapter metadata fails closed. This does not synthesize metadata that a provider/streaming decoder never captured. The portable C kernel has no new storage dependency.

A provider failure after verified tool results keeps the prefix and follows the task's existing retry policy. Recovery rebuilds the same core transcript from saved responses/results, then calls the provider only at the new frontier. Cached tool results need no execution approval or fresh host-call grant. Fresh calls still pass the current policy/approval gate and a new revision/owner-stop/deadline fence. Tool registry, initial messages, provider identity, execution scope and context/compaction options must match. Context-too-long control responses are retained so deterministic compaction can be replayed; an ordinary failed provider response is not pinned forever.

New tool-loop provider adapter invocations and actual host dispatches are charged; replayed calls report zero new usage. A task with zero remaining tool grant can still recover an old receipt and spend an available provider step to finish. A new model-requested tool is refused before host I/O if no grant remains. Core traversal includes cached calls, but explicit adapter limits independently bound fresh work. A terminal core/policy limit preserves verified progress without automatically retrying or increasing budgets. Provider-internal HTTP retries, token/time overshoot and adapter subprocess descendants are not certified strict meters. Interrupted grants retain conservative charges even when a transcript is recoverable.

The checkpoint is canonical-hash checked and bounded to 512 provider/tool records and 8 MiB, with 2 MiB per provider record including its adapter sidecar, and per tool result. A pending call, persistence failure or mismatching replay remains fail-closed; no guessed receipt is accepted. Owner stops are never revived. The final run receipt still recovers without any fresh grant, even when no attempts remain.

A failed attempt with a matching verified frontier exposes informational `effects[].resume_available: true` without returning the transcript payload in metadata-only polling. The UI says **Verified progress saved**, rather than claiming the host outcome is unknown. Exhausted automatic attempts/budgets remain stopped; this slice adds no manual-resume or budget-expansion endpoint.

Host/extension results at this boundary must be bounded JSON objects with a boolean `ok` field (2 MiB maximum). Process timeout/cancellation and negative exit status are uncertain. A known nonzero exit remains available to the model as a failed tool result; it does not by itself authorize a workflow retry. Complete run receipts are bounded to 8 MiB. No credentials are added to the identity record; the persisted source request is hashed before dynamic budget clamps. Existing workflow run evidence may contain tool inputs/outputs, so use host credential references rather than literal secrets.

The polling effect metadata marks this boundary as `boundary: model_run`. The operator UI identifies an incomplete model run and intentionally provides no HTTP receipt-URL form. Inspect the original run, tool receipts and downstream effects. The reconcile endpoint does not accept invented model-run receipts. A replacement workflow/task identity would be a new action, **not** a safe retry.

This is not universal transcript recovery, exactly-once physical execution, proof of business commit, or certification that every subprocess descendant has stopped. Direct chat/CLI, provider-specific vision prefetch, delegated sub-attempts, capsules and provider-native tool execution do not acquire transcript continuation here. Vision prefetch runs before the durable tool-loop seam, so it is deliberately excluded rather than silently regenerated during replay. Legacy incomplete whole-run records cannot be retroactively resumed. Trusted host receipt adapters and those remaining execution paths are roadmap work.

Verification: `workflow_model_transcript_tests` covers exact cached identities/results, reopen recovery, zero-grant frontiers, current-policy fresh dispatch, context errors, changed requests/scopes, corrupt checkpoints, missing receipts, record limits, persistence failures and stale revision/owner fences. The real-process fixture additionally verifies transient/persistent provider failures, batched tools, exact-budget continuation, refusal of a new unbudgeted effect and death while waiting for the next provider under a newly restrictive policy. A rejecting DeepSeek-compatible stub requires the exact original reasoning content after a resumed multi-tool batch. A synthetic image-prefetch path verifies the conservative exclusion without calling a live provider. `workflow_model_effect_tests` covers lazy intent, stable identity, cached usage, interrupted/malformed receipts, stale-runner CAS, failed admission/persistence, capacity and stop fences. `agentd_workflow_model_effect_recovery_smoke` uses a local stub provider and real host subprocesses, kills/restarts daemons around admission/receipt/task settlement, verifies provider-only retry, exact-budget recovery without fresh approval, configured deny/approval gates, multiple calls, timeout, independent progress, cancellation and deadlines.

### Session-backed conversation continuity

The existing explicit workflow `allow_sessions: true` opt-in and session binding remain required; this feature does not turn ephemeral tasks into persistent ones. Persistent ordinary host-model workflow/SOP tasks load messages and their durable session generation in one SQLite read transaction. A checkpoint pins that session ID and generation. Failed attempts do not publish user messages, generated pinned prompts, partial answers or duplicate audit/UI mirrors to the conversation. Recovery rebuilds from the original session and exact cached provider/tool records. It still fingerprints regenerated instructions, memory/context and provider requests; changed context is refused, not silently rebased.

Schema v39 adds `session_revisions`. Every normal message replacement and session deletion advances the generation, including identical-content rewrites. The generation survives deletion/recreation, so an old checkpoint cannot match a newly created conversation with the same ID. These rows retain identifiers/generation counters, not deleted message bodies, and belong in the application's backup/erasure planning. Migrated existing sessions receive an initial generation.

Before a fresh provider request the runtime checks the generation. Before every fresh host dispatch it checks it in the same transaction that admits the pending effect. An already admitted call can still finish after an edit; its actual receipt is saved, but a stale continuation cannot issue another call or overwrite the new conversation. There is no global per-session execution lease: independently authorized concurrent tasks may already have performed host work. A generation conflict is not proof that those physical effects were undone.

On successful completion, final messages and the completed model-run receipt commit in **one transaction**, with the effect revision, session generation, workflow cancellation and deadline fences. A storage failure rolls back both. A crash before that commit leaves the original conversation and verified checkpoint; a crash afterward reuses the completed receipt without appending again, even if someone has since edited the conversation. A provider-only success uses a generation-checked replacement and still creates no host-effect row. Final messages are bounded to 20,000 rows and 8 MiB of role/content/multimodal payload.

Conversation drift produces `blocker.kind: model_session_changed`, preserves original tool receipts and leaves the newer conversation untouched. The UI labels this **Conversation changed**, without an HTTP receipt form or resume/reset control. `result.effect` includes the pinned `session_id`, baseline `session_revision`, `session_committed` and, on conflict, `session_conflict`. Completed-receipt replay reports the original commit; it does not claim to own the session's current revision. No automatic rebase, replacement task, reset, permission expansion or new budget is implied.

The atomic guarantee covers the conversation plus model receipt, not every auxiliary view. Audit/artifact/scene mirrors remain best-effort after successful commit; a crash in that gap can omit them. This is not exactly-once audit/event publication or exactly-once physical execution. The same conservative exclusions for provider-specific vision prefetch, delegated attempts and direct chat/CLI still apply.

Verification: `workflow_model_session_tests` exercises generation migration semantics, identical rewrites, delete/recreate fences, two-connection CAS races, pre-provider/pre-tool/final conflicts, SQL rollback of both conversation and receipt, completed replay, bounded results and owner stops. The real-daemon recovery fixture retains prior conversation content, retries a provider failure with pinned prompts, kills before and after atomic publication under a restrictive policy, and edits the session through the authenticated UI-event API while a provider response is in flight.

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
