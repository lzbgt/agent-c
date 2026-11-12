# Durable process decisions

A `kind: decision` task waits for one explicitly configured process choice. It is useful for a review required by a particular SOP, not an approval prompt inserted before ordinary authorized work.

The wait is stored in SQLite. It holds **no worker, attempt or execution budget**, so independent tasks and other workflows continue even with one worker. After an answer, the same task resumes from the immutable receipt, including across a process crash.

## Try it

In **Workflows → Create workflow**, select **SOP · review decision**, then explicitly submit. The provider-free sample has a check, a review and a final record step. It makes no external writes. Active run details refresh every five seconds to reveal newly activated reviews.

The same request is in [examples/workflow_sop_review_decision.json](../examples/workflow_sop_review_decision.json). Submit it inline or save an immutable SOP version and submit with `sop_ref`.

Open the run's **Process decisions** section. Each button shows its configured label and effect. Optionally add a note, then choose deliberately:

- `continue` completes the decision task successfully, allowing its dependents to run.
- `fail` fails the task without retrying. Dependent tasks cannot proceed; unrelated branches continue under ordinary workflow rules.
- No choice is implicit or selected by a default timeout. Expiry is a terminal failure.

The UI fences repeated clicks. If a response is lost, it keeps the exact choice, note and delivery key and offers a safe same-choice retry or receipt refresh. It never silently sends a different answer. Workflow selection, connection and credential changes discard stale response UI. A process choice does not expand tool permissions.

## Definition

```json
{
  "task_id": "review",
  "kind": "decision",
  "decision": {
    "prompt": "Release this inspected batch?",
    "choices": [
      {"id": "release", "label": "Release batch", "outcome": "continue"},
      {"id": "reject", "label": "Reject batch", "outcome": "fail"}
    ],
    "timeout_ms": 86400000
  }
}
```

The prompt is 1..2048 UTF-8 bytes. There are 1..8 choices with unique id-safe IDs of 1..64 bytes and labels of 1..128 bytes. Allowed ID characters are `A-Za-z0-9_.:-`. Text cannot contain NUL. Unknown fields and outcomes are rejected. `allow_error=true` is not accepted.

Timeout is an integer from 0 to 2,592,000,000 ms (30 days). Omitted/zero waits indefinitely; a positive timeout starts on **first activation after dependencies finish**, not on submission. Restart/re-claim never extends it. Workflow cancellation and workflow deadlines always remain authoritative.

## Read and answer

Authenticated `GET /api/v1/workflow?workflow_id=...` includes `decisions`, even with `include_tasks=0`. The projection exposes prompt, configured choices, stable identity/hash, waiting/resolved/expired/stopped state, availability, timestamps and the recorded outcome/note. It does not expose unrelated execution requests.

Send `POST /api/v1/workflow/decision` with a maximum 8 KiB body:

```json
{
  "workflow_id": "the-run-id",
  "task_id": "review",
  "decision_id": "the-server-returned-decision-id",
  "choice_id": "release",
  "idempotency_key": "a-unique-delivery-id",
  "note": "Inspection evidence checked"
}
```

Copy `decision_id` from the read response. Delivery keys are id-safe strings of 1..128 bytes; notes are optional and at most 4096 UTF-8 bytes. Never supply a client outcome or approver identity: the server derives the outcome from its integrity-checked definition. Notes/prompts are durable application data, **not a secret store**.

The first valid answer and task wakeup commit together. The response contains `ok:true`, `replayed:false` and the receipt. Repeating the exact delivery key, choice and note returns the same receipt with `replayed:true`. A replay after workflow stop is historical acknowledgment only: it cannot wake work, reset a task or clear cancellation.

A different answer/key/note after resolution, an identity mismatch, an expired decision or stopped workflow returns 409. Unknown choices/malformed bodies return 400; decisions that have not activated or do not exist return 404. Authentication is required before any lookup. Storage or integrity failures fail closed.

## Persistence and boundaries

Schema v38 adds `workflow_decisions`, unique by workflow/task, with an immutable canonical definition hash and one terminal answer. Atomic first activation changes the claimed task back to queued, refunds its attempt and sets readiness to expiry (or an indefinite wait). Answering checks current workflow/task state and wakes only that queued task in the same SQLite transaction. There is no lost wakeup between publishing the wait and accepting an answer.

The workflow engine remains single-daemon. A waiting task still counts toward admission limits, but not running-worker concurrency or execution-budget reservations. The final decision execution consumes one attempt and zero tool/step/token/elapsed budget. Existing task expectations can still reject a successful answer. Explicit owner cancellation/deadlines are not reversed.

The authentication boundary is the existing daemon access boundary. This is **not** a per-person manager-approval system, quorum, role attestation or authority to perform an otherwise unauthorized action. There is no fabricated human actor in the receipt. General external signals, branches, loops, compensation and active-run version migration remain separate future work.

Decision rows cascade with their parent workflow tasks. Keep database access, backups and authorized erasure under the existing daemon-state policy. No new deployment is implied by source publication.

## Verification

`workflow_decision_tests` exercises v37→v38 migration, validation, wait/receipt restart, exact replay, two-connection conflicting answers, timeout, stop preservation, corrupt source, zero budget demand and API facade authentication.

`agentd_workflow_decision_smoke` uses a real temporary authenticated daemon with one worker. It proves independent task/workflow progress, no held decision reservation, kill/restart, approved and rejected outcomes, concurrent answers, dependency-relative expiry, cancellation/deadline, saved-SOP execution and integrity rejection.

`ui/e2e/workflow_decisions.spec.ts` covers exact deliberate delivery, repeat clicks, uncertain same-choice retries, stopped/expired controls, stale workflow/connection responses, active detail refresh, mobile layout and the explicit review template. The canonical snapshot verifier includes these suites.
