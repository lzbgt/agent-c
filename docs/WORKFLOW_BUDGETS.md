# Workflow budget reservations

Workflow tasks acquire a durable per-attempt resource grant before executing a provider/tool action. Independent workers cannot reserve the same remaining capacity. Workflow limits remain opt-in; the autonomous default does not introduce permission prompts. With no configured limits, attempts retain conditional settlement without writing zero-value reservation rows.

## Admission and settlement

The four dimensions are tool calls, model/tool steps, elapsed milliseconds and total tokens. `workflow_budget_reservations` stores `(workflow_id, task_id, attempt)`, a private fencing token, the granted amounts and `reserved`, `settled` or `lost` state. The token is never returned in API/result payloads.

Admission uses `BEGIN IMMEDIATE` to check the current task/attempt, workflow definition, stop/deadline, settled usage, open grants and conservative lost charges in one SQLite transaction. It grants at most the requested amount and currently available capacity. Concurrent fairness counts and task claims also share a write transaction across connections.

Demand derives from existing task/run bounds. Known deterministic actions reserve their small operation counts; model runs use their requested/default step and tool caps. Unbounded demand takes the remaining configured allowance. Model total-token demand reserves the remaining total-token allowance because output-token settings do not bound prompt usage. Give parallel actions bounded step/tool/time settings when those resources are limited; two appropriately bounded actions can run concurrently.

When capacity is held by another attempt, the task yields back to queued without consuming a retry or an allocation. Tasks that cannot consume that dimension, such as a pure delay under a tool/step limit, remain eligible. An exhausted configured limit retains the workflow's explicit budget-stop behavior. Owner stops remain effective, and stale scheduler running updates cannot reopen a stopped workflow. Expiry of a per-attempt elapsed grant fails/retries that attempt within remaining budget; it does not fabricate a global owner stop or workflow deadline.

Settlement commits the task result/state, cumulative observed usage and reservation release together. The private token plus current running attempt/request reject stale or duplicate settlement. Unused grant capacity becomes available only with that commit. A failure between task persistence and grant release rolls back both. Replaying a completed HTTP receipt still charges zero network calls.

HTTP POST/reconciliation checks the remaining granted network-call capacity before a new dispatch. A receipt lookup and subsequent redispatch cannot spend the same final call twice; capacity refusal before dispatch preserves a prepared effect and does not invent an ambiguous external write. One-shot `tools:none` model attempts now count steps and obey their step cap, including bounded context-retry attempts, instead of reporting zero steps.

Ordinary host-model workflow vision preparation now shares the same provider-step meter as the tool loop. A fresh image-description prefetch spends one step and contributes provider-reported token usage, even if the returned caption is rejected as oversized. Configured output-token caps are sent to the preparation provider. A one-step grant can perform preparation but cannot then make another provider request or host action. Verified preparation/provider/tool replay costs no fresh calls or tokens; a reduced remaining output-token cap does not force a cached image to be interpreted again. This covers the existing bounded prefetch adapter, not its internal HTTP retries, missing usage reports or all delegated/native-provider work. See [durable image preparation](WORKFLOW_EFFECTS.md#durable-image-preparation).

## Recovery and uncertainty

On daemon recovery, open grants become `lost`; their full reserved amounts remain conservatively charged, while ordinary held capacity is released. Old holders cannot settle after recovery or overwrite a newer attempt. Successfully settled usage is not charged again. Repeated recovery is idempotent.

A lost charge is an upper-bound accounting decision, not a claim that the provider or device consumed exactly that amount. The system does not silently refund uncertain work based on a model assertion. Native execution continues to have single-daemon DB ownership and at-least-once task recovery; reservations do not make arbitrary host-tool effects exactly once.

## Observed usage versus enforceable limits

Admission capacity is atomic. Core step/tool caps, deterministic HTTP call caps and new device-assignment dispatch counts enforce their covered operation counts. Observing an existing device assignment costs no new tool/step grant; physical adapter internals remain separate. Elapsed-time cancellation has scheduler/network overhead, and provider-reported total tokens can exceed a reservation because prompt usage, internal retries, compaction and provider behavior are not certified by an output-token cap. Remote delegated work and other uninstrumented adapter internals do not acquire a strict resource guarantee from a local reservation.

The runtime records observed overages in each result's `workflow_budget.overrun`; it charges the full observed usage rather than clipping it to the grant. Subsequent admissions see that debt. A grant is not a hard monetary, real-time or provider total-token ceiling. Strict provider/input-token accounting and broader adapter metering remain required work.

## API and storage

`GET /api/v1/workflow` includes `budget_accounting`, independent of `include_results`:

- `charged`: cumulative settled observed usage plus conservative lost grants.
- `held`: active reserved capacity.
- `uncertain`: the lost-grant portion of charged usage.
- `active_attempts` and `uncertain_attempts`: reservation counts.

Keys within those vectors match the workflow limit names. `workflow_remaining` reports `max(0, limit - charged - held)`. Existing `workflow_usage` remains observed cumulative task telemetry, so it does not mislabel lost grants as provider invoices. Result evidence contains granted/observed amounts and overrun names, with technical data kept out of primary chat.

Schema v35 adds the reservation table and an active-state index; no task data transformation or compatibility layer is introduced. Include it in DB backup/erase procedures. The implementation uses WAL and `synchronous=FULL`; actual process-kill tests establish checked host recovery, not hardware power-loss certification.

## Reproduce

`workflow_budget_tests` exercises concurrent connections, bounded parallel grants, retry-preserving waits, refund/rollback, duplicate and stale settlement, owner stops, fairness claims, observed overages and real process kills before/after settlement. `workflow_budget_parallel_smoke` uses a real daemon and local model/tool/receipt stubs to prove:

1. A held grant prevents duplicate provider admission while independent pure work completes.
2. Bounded grants allow actual concurrent provider requests.
3. Capacity waiting does not consume retries; completed operations settle once.
4. A shared one-call tool limit is not spent by two independent model/tool rounds.
5. Receipt lookup plus redispatch cannot overrun the last HTTP call grant.

The snapshot verifier includes these cases. Existing step/tool/token/stream/retry/bulk-cancel budget regressions remain checked separately. No live provider keys are needed.

The transaction design follows [SQLite transaction semantics](https://www.sqlite.org/lang_transaction.html) and its [synchronous/WAL contract](https://www.sqlite.org/pragma.html#pragma_synchronous), inspected 2026-11-11. Conservative lost-charge policy and resource scopes are Agent-C design decisions, not guarantees inherited from another agent framework.
