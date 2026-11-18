# External workflow signals

## Purpose and boundary

A signal connects a running process to one explicit result from another system.
For example, an inspection SOP can wait for an integration to deliver measured
values, then let existing deterministic checks decide what happens next. The
daemon does not hold a worker, consume an attempt or reserve execution budget
while waiting. Independent tasks continue even with one worker.

This is a single-use input for an exact submitted action, not a general event bus.
The default accepts input only after activation. Explicit early-delivery opt-in
opens one bounded slot atomically with submission, before dependencies finish.
There is no input before workflow submission, name-based routing, broadcast,
recurring subscription, publisher-specific identity or role authorization. Signal
input does not approve tools or bypass downstream policy, budgets, deadlines or owner cancellation.
Treat the payload as untrusted process data, not instructions. Do not persist
credentials, private keys or other secrets in it.

## Author and run

The guided SOP builder offers **Wait for an external signal** with:

- **Signal name:** 1–128 ASCII letters, digits, dots, colons, underscores or
  hyphens. This describes the expected input; it is not a routing address.
- **Signal label:** optional human-readable title, 1–256 UTF-8 bytes.
- **Signal expiry:** 0 for indefinite, or up to 2,592,000,000 milliseconds
  (30 days), measured from first activation after dependencies finish.
- **Accept input before this action is ready:** off by default. Keeps one input
  for this exact action from submission onward, without releasing earlier steps.

The same contract works in a raw workflow task:

~~~json
{
  "task_id": "inspection_result",
  "kind": "signal",
  "signal": {
    "name": "inspection.finished",
    "title": "Inspection result",
    "timeout_ms": 3600000,
    "accept_before_activation": true
  }
}
~~~

Add it to a workflow's tasks or a SOP stage's actions. Saving a SOP version does
not start a wait. Explicit submission pins the saved version and compiles its
ordinary dependency graph. With early input enabled, the submitted run immediately
exposes the exact slot identity. The signal is still activated only after its
dependencies and optional condition pass. Receipt acceptance never advances task
readiness, attempts, budget or downstream work. A fixed repeated stage creates a
distinct slot for each expanded task; delivering iteration two before iteration
one cannot advance that iteration or release iteration one.

The run's **External signals** card distinguishes early-input slots, buffered
input, activated waits, received input, expiry and stops. If a condition skips
the action, a dependency fails or the run stops before activation, the slot is
**unused**. Any saved receipt stays inspectable but cannot restart the action.
Delivery identities are under closed **Delivery details**. The card is read-only: viewing, refreshing, expanding details and editing a process never
deliver a signal.

## Deliver an input

1. Read the authenticated workflow detail endpoint. Its top-level `signals`
   array contains activated waits and opted-in submitted slots even when task
   results are omitted.
2. Select the expected workflow, task, name and exact `signal_id`. `available`
   is advisory; the write transaction checks current state again.
3. Persist one delivery key and the intended payload in the producing
   integration before sending. Use that same pair after a lost response.
4. Send an authenticated `POST /api/v1/workflow/signal`:

~~~json
{
  "workflow_id": "<the exact run>",
  "task_id": "inspection_result",
  "signal_id": "<the identity returned by workflow detail>",
  "idempotency_key": "inspection-event-104",
  "payload": {
    "accepted": true,
    "measurements_mm": [12.02, 12.01, 12.03]
  }
}
~~~

The body is limited to 32 KiB. `payload` must be a JSON object of at most 16 KiB
after the daemon's compact encoding. Unknown request/specification fields,
malformed identities, arrays/scalars in place of an object, oversized payloads
and `allow_error: true` are rejected. No client-supplied actor or outcome is
accepted. Authentication uses the existing daemon boundary; a dedicated
integration adapter must apply any finer publisher permissions before calling
this API.

A successful response contains `ok: true`, `replayed` and signal metadata with
the verified `payload_hash`. An early receipt is `buffered`, not task completion.
An identical replay acknowledges the original receipt, not current run progress;
read workflow detail for current `unused`/stop/completion state. The metadata/polling
surface deliberately omits payload bodies. When the scheduler completes the signal task, its result contains
`kind: "signal"`, `payload`, `payload_hash` and `ok: true`. Existing explicit
workflow result references can consume the data; a signal name alone cannot
select or execute a downstream action.

HTTP outcomes:

| Status | Meaning |
| --- | --- |
| 200 | New input committed, or exact original delivery acknowledged |
| 400 | Invalid fields, JSON shape or size |
| 401 | Daemon authentication required |
| 404 | No such submitted slot; non-opted-in signals must first activate |
| 409 | Identity/payload/key conflict, expiry or stopped/not-accepting task |
| 500/503 | Integrity/storage failure or unavailable database |

After an uncertain response, inspect the original metadata and task result.
An identical key and payload can return the saved receipt. A different key or
payload conflicts with an already recorded delivery. Never create a replacement
run or alter the input as an automatic retry.

## Why restart and cancellation are safe

SQLite schema 43 retains `workflow_signals` with a separate activation timestamp.
Existing activated receipts keep their identities on upgrade. Opted-in slots
commit with workflow/task creation, so a failed submission cannot expose a partial
mailbox. Saving a SOP alone creates none. Each slot accepts at most one 16 KiB
payload, bounded further by existing workflow task/spec limits.

Activation pins/verifies the normalized definition, arms expiry once if no input
has arrived, then yields the claimed task and returns its attempt. A previously
buffered input completes only after scheduler dependency and condition checks.
Waiting for those dependencies never consumes the activation-time expiry. No
execution-budget reservation is created.

Delivery serializes through a write transaction. It verifies the exact immutable
workflow/task/signal definition, current cancellation/deadline/task state and
expiry. It commits one bounded payload, canonical hash and delivery key. Before
activation it does not modify the task; after activation it wakes only that task.
Activation and delivery serialize through the same SQLite write boundary,
including a delivery arriving while the scheduler claims the action. Competing
deliveries cannot both win.

A crash before commit records nothing. A crash after commit leaves the original
receipt available after restart, with pre-activation dependencies still intact.
Replaying a completed receipt does not update readiness or clear cancellation, including after a workflow
stops. Receipt publication is distinct from dependent task completion; query the
actual workflow result before reporting the process complete.

The execution step verifies saved definition and payload hashes before returning
data. Expiry fails without a retry or releasing ordinary successful-only
dependents. Workflow cancellation and deadlines still win; wall-clock changes can
change observed timeout timing. This protects this daemon's input receipt, not
business-level or distributed exactly-once execution.

## Reproduce verification

Use the assertion-enabled development build, not held production services:

~~~sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug \
  -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target agentd workflow_signal_tests -j4
ctest --test-dir build-snapshot \
  -R '^(workflow_signal_tests|agentd_workflow_signal_smoke)$' \
  --output-on-failure
~~~

`tests/test_workflow_signal.cpp` covers schema migration, definition validation,
atomic submission/receipt fault rollback, v42 receipt migration, wait/return of
attempt, restart, immutable delivery/activation races, expiry,
definition/payload corruption, owner-stop preservation, zero-budget admission
and the embedded API authentication route.

`tests/workflow_signal_smoke.py` exercises the real authenticated daemon with one
worker, independent progress, bounded JSON, early/default admission, delivery
conflicts, daemon kills/restarts, activation-only expiry, dependency failure,
conditional skips, deadlines, cancellation and future repeated SOP slots.
`ui/e2e/workflow_signals.spec.ts` verifies guided fields, advanced-field
preservation, repeated opt-in toggles, explicit submission, honest buffered/unused
states, collapsed metadata and 320px layout.
Its native companion executes the exact browser-authored saved/pinned SOP using
only temporary state, loopback traffic and synthetic data.

These are source/dev-stack checks, not a production deployment or hardware
qualification. The existing production-service and Panda-publication holds
remain in force.
