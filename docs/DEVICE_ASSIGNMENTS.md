# Durable device assignment

Device commands use a platform transaction before delivery. The shared `edge_enqueue_task_assign` path serves direct `/api/v1/edge/task/assign`, workflow `edge_invoke`, device workflows and event rules. It validates the advertised tool/schema and configured hazards, then atomically commits:

1. The immutable `(task_id, step_id)` operation, its selected node, idempotency key, mode and canonical serialized payload.
2. Admission against active platform resource locks and per-node/tool rate/cooldown limits.
3. One `TASK_ASSIGN` outbox entry and the corresponding queued event.

SQLite `BEGIN IMMEDIATE` serializes concurrent admissions across connections. A failed transaction leaves no orphan task, command, event or consumed rate slot. A restart sees either the committed assignment or no assignment. No schema change is needed.

## Retry and recovery

An identical request observes its existing assignment without appending another outbound command or charging rate capacity again, including after successful completion. `deduped: true` and `outbox_id: 0` mean this request published no command. Poll the task for its result; transport consumers recover using their existing outbox cursor. Retries cannot extend the original command's deadline.

Changing the node, key, mode or payload under an existing logical operation returns a conflict. Reusing one node/key for another operation also conflicts. Generic task-state updates cannot rewrite the original node/key/mode/payload. A new business action needs a new logical operation identity; changing an attempt number is not a new action.

Workflow `match_any` recovery and direct API retries consult the durable assignment before selecting a node. A scheduler result lost after dispatch cannot move the action to another device. Device-workflow dispatch uses one stable idempotency key per logical step across transport/scheduler attempts.

Workflow `edge_invoke` reads the original device task/result. `TIMED_OUT` or `CANCELED` ends automatic command retries and exposes `device_outcome_unknown` with the required action and inspected evidence. A deadline or cancellation does not establish physical rollback. The normal workflow API includes a bounded blocker projection without requiring full task results; the UI shows the needed action in **Needs your attention**. Other eligible branches continue, including with one scheduler worker; dependent work remains unexecuted. This is a task-scoped dependency, not a generic permission prompt.

## Boundaries

This is a platform dispatch guarantee. It does not certify exactly-once physical execution, power-loss behavior, device-side command deduplication or physical stopping. Nodes must retain operation identity/results through their actual recovery horizon and enforce command expiry, unit/range checks, interlocks and controller fencing locally. A repeatedly delivered transport message is not a newly admitted platform command.

Resource locks cover active platform assignments; they are not physical interlocks. Platform terminal status alone cannot establish that hardware is safe to reuse. Rate/cooldown admission is atomic, while overall workflow token/time/tool budgets still use their documented accounting semantics. Workflow-wide reservation/settlement now has its own [budget contract](WORKFLOW_BUDGETS.md); arbitrary host-tool effect adapters and strict provider/adapter resource metering remain required work.

## Signed device receipt recovery

Schema v36 stores validated successful `TASK_DONE` receipts separately in `edge_task_receipts`. A receipt is bound to the original task, step, node, idempotency key and immutable command payload. It retains the canonical result, its platform-computed SHA-256, optional attestation and receipt time. The first committed receipt is immutable: identical repeats are observations; a different result conflicts. The receipt transaction does not change the platform timeout/cancellation state or publish a command.

Recovery receipts require a cryptographically verified envelope, even when unsigned legacy ingress is enabled. The authenticated sender must be the assigned node and the key must match the original operation. Result-schema checks and configured attestation requirements still apply. Invalid supplied signatures or result-hash attestations never produce recovery evidence, and inbound underscore-prefixed fields cannot forge platform validation decisions. Keep per-node key policy and revocation configuration appropriate to the deployment; shared trusted signing keys do not provide isolation between their holders.

A matching signed late result can settle an already timed-out or canceled assignment. This proves only the destination's reported completion under its trust contract, not independent physical quality or safety. An absent receipt, unsigned success claim, expired deadline, cancellation or transport acknowledgment cannot establish completion or authorize another action.

For an uncertain workflow task, agents and the **Check device receipt** control use the existing authenticated endpoint:

```sh
curl -X POST "$AGENTD_BASE/api/v1/workflow/reconcile" \
  -H 'Content-Type: application/json' \
  -d '{"workflow_id":"wf_42","task_id":"device_action"}'
```

Use the deployment's normal API authentication. Device reconciliation accepts no result, success flag or resolver URL. It checks the original recorded receipt, then atomically requeues only that failed workflow task. It preserves completed independent work, budgets, deadlines and sticky owner cancellation; a conflicting active request returns 409. Missing evidence returns 409 without changing the workflow. Receipt observation costs no new device dispatch. The scheduler exposes both `edge_platform_state` and `edge_receipt_reconciled`, so a recovered result does not hide its earlier timeout.

This is the signed UM-BMP receipt adapter, not a generic arbitrary-device readback service. Nodes must submit their existing original result through authenticated ingestion. There is no automatic replay for `not_applied`, no inferred rollback and no certification of board power-loss behavior or destination fencing. Other device protocols and arbitrary host-tool effect adapters remain separate work.


## Verification

`device_assignment_tests` checks:

- Duplicate and terminal receipt reuse, payload/node/key conflicts and immutable task identity.
- Transaction rollback when outbox publication fails, preserving rate capacity.
- Concurrent admissions across two SQLite connections, active resource exclusion and bounded rates/cooldowns.
- Owner cancellation/deadline enforcement before publishing a new command.
- Stable node selection in workflow and direct API recovery after the preferred node changes.
- A real one-worker scheduler: an uncertain device action exposes its required action, independent work completes, dependents do not run, and no second command is published.
- A real writer killed during repeated transactions; restart retains equal task/outbox/event/rate counts.
- Signed late receipts, wrong node/key, unsigned claims, schema/attestation rejection, immutable duplicate/conflict handling, abrupt receipt-writer exit, transactional recovery rollback and restart.
- A one-worker workflow recovers its original result, releases dependent work, preserves completed independent tasks and refuses owner cancellation/deadline bypass, with exactly one platform command throughout.

Existing device schema, receipt/result, deadline, workflow submit/cancel and template expansion smoke tests remain part of the checked regression lane. These host tests do not substitute for board/device qualification.

The design follows the distinction between durable orchestration and destination-enforced idempotency described in [Temporal Activity Definition](https://docs.temporal.io/activity-definition), inspected 2026-11-10. The local transactional outbox and scoped uncertainty behavior are Agent-C design decisions; no upstream runtime was imported.

For opt-in public deployment qualification, run `python3 tools/deploy/verify_device_assignment.py`. It reads the private gateway config without printing credentials, registers two clearly named synthetic nodes, and checks pinned recovery, changed-payload refusal, one outbox command and completion reuse. It writes qualification records to the deployment but never contacts real hardware. The report defaults to `build/device_assignment_live_verification.json`.
