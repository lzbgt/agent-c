# Browser chat submission uncertainty fence

The direct/native chat run hook now persists a scoped, secret-free trace ID and timestamp before a synchronous or asynchronous run POST. A transport error, malformed response, reload, or asynchronous response without an accepted job leaves the fence in place. Further queue dispatch and direct sends stop. Failed queued work is not inserted back into the automatic queue; its text is restored to an empty composer for inspection. Remaining queued messages stay queued. Storage failure sends nothing and pauses the current queue.

An accepted async job is recorded under its original session scope before clearing its matching fence. A synchronous terminal response clears the fence even when the run reports failure. A late response from a previous session does not publish job state into the newly selected session. The recovery notice displays the trace receipt; deliberate local release requires a warning that the original request may have executed. Release neither retries nor cancels that request, but permits remaining queued/new messages.

## Limits

This is a browser-local conservative safety fence, not server-side idempotency. It does not provide an atomic lock between tabs/devices, preserve full queued prompts/attachments across reload, or recover an unknown accepted job automatically without an explicit lookup. Trace IDs support investigation, not proof of nonexecution. No background retry or unbounded lookup is added. Team chat has its own separate submission path and is unchanged. Clearing browser storage removes this local protection. Prompts, attachments, auth and provider keys are not written into the receipt.

## Read-only accepted-job recovery

Check existing chat job issues authenticated GET /api/v1/job with the persisted trace_id and exact session_id. Native lookup uses the existing trace index, reads at most two metadata rows, and never returns prompts, request secrets, results or events. One match can resume watching that existing job; zero matches (404), multiple matches (409), identity mismatch and unavailable storage remain held. Lookup never submits or retries a run. Synchronous runs generally have no async-job receipt and therefore remain held for manual investigation.

The durable row is a correlation receipt, not proof of exactly-once effects. Existing run_async persistence is best-effort, and restart recovery can requeue running jobs. Transactional admission, immutable key/request binding and execution recovery must be addressed separately before promising server-side idempotency. No admission or restart behavior is changed by this feature.

## Verification

`ui/e2e/chat_submission_fence.spec.ts` mounts the actual React run hook with synthetic intercepted transport. It checks exact one-POST behavior after lost response and reload, retained later queue entries, secret-free trace metadata, and zero POSTs when receipt storage fails. It also verifies missing receipts retain the fence and a unique receipt attaches the original job with no new POST. tests/job_trace_lookup_smoke.py exercises real local async acceptance, auth, exact session scope, restart, ambiguity, metadata-only responses and unchanged job rows across repeated lookups. Production services and real providers are not used.
