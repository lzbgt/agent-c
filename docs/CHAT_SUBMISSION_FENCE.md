# Recover an uncertain chat submission

The browser must distinguish a failed connection from a request that did not
execute. A daemon can accept work and lose the HTTP reply afterward. Reposting
that message could repeat a model call, host command or other external effect.
Agent-C saves a local submission receipt before sending and reconciles existing
work through authenticated reads. It never treats a missing reply as permission
to repeat execution.

This contract covers direct/native session chat and broker-proxied native runs.
Team chat has a separate submission path. Workflow submission and model/tool
checkpoint recovery have their own contracts.

## Browser submission and handoff

Before either synchronous or asynchronous POST, `useRunExecution` saves a
version-1 receipt under the exact connection/session scope. It contains only:

- the generated trace ID;
- the exact session ID;
- `async` or `sync` submission mode;
- the creation time in Unix milliseconds.

The receipt contains no prompt, attachment, provider key or authentication token.
The storage write is read back before dispatch; a failure sends zero POSTs.
An uncertain request holds further direct sends and automatic queue dispatch in
that scope. Failed queued work is not reinserted into the automatic queue; its
text is restored to an empty composer if its original scope is still selected.
Late failures never replace another scope's draft. Other queued entries stay
queued, and other session scopes remain independent.

An accepted asynchronous job is synchronously saved to the existing browser job
watch store before its matching submission receipt is cleared. This write is
also read back. If saving the watch fails, the receipt remains held and recovery
can read the original job after storage becomes usable. React and server-side
watch preferences remain projections; scheduling their update alone is not
durable browser handoff. The cache keeps at most 64 watches and reserves a slot
for current acceptance before considering cached timestamps; a clock rollback
cannot evict the just-accepted job. A synchronous terminal response clears its
receipt even when the run reports failure. An uncertain synchronous response
stays held.

The unused async-to-sync fallback helper was removed. A transport or parsing
error never switches execution endpoints or issues a second POST.

## Automatic read-only recovery

After a lost asynchronous reply, or when the browser reloads a valid asynchronous
receipt, the UI automatically asks:

```text
GET /api/v1/job?trace_id=<saved-trace>&session_id=<saved-session>
```

The daemon requires its normal authentication. Native lookup uses the trace
index and reads at most two metadata rows. It never returns prompts, request
secrets, results or events, and never dispatches work. One matching row is a
correlation receipt for the original accepted job. Zero rows return 404;
multiple rows return 409. Neither result proves that the request did not execute.

Each automatic cycle makes at most **three GETs**. The second and third reads
wait **250 ms** and **1000 ms** respectively. Each read, including its body, has
a **five-second deadline** and a **64 KiB response cap**. Temporary network
failures and 404/408/429/500/502/503/504 may consume the remaining reads. An
authentication rejection, ambiguous match, invalid JSON, oversized body,
identity mismatch or unknown job state ends that cycle without adoption.

The response must match the saved trace and session, identify a durable native
row, and name a nonempty bounded job ID with a known job state. Recovery saves
that job's watch, selects the original job and clears only the matching receipt.
It performs no execution POST and does not refund or repeat model/tool work.
The normal job watcher then obtains current progress or the terminal result.

Connection, session, receipt or authentication changes abort in-flight reads and
retry delays. A late reply cannot attach to the new context or clear its hold.
React StrictMode effect restarts may abort an initial probe; cancellation does
not count as a completed automatic cycle. A completed unsuccessful cycle does
not restart on unrelated renders. Reloading or re-entering a context can start
a new bounded cycle; this is not a durable global rate limit.

The normal chat view shows a short recovery status. Technical receipt fields and
deliberate local release stay inside closed **Recovery details**. **Check run**
makes one additional read-only lookup. Local release requires a warning because
the original request may have executed; it neither cancels nor retries it.

## Durable native admission and cancellation

`POST /api/v1/run_async` requires insert-only durable admission before starting
an execution thread. Persistence failure returns 503 with no dispatch; an
unpersistable request returns 413. An existing durable `job_id` returns 409
without overwriting its original request or outcome. Admission reserves status
`running` before the thread starts, so the queue engine cannot claim it too.

Restart changes previously running jobs to `interrupted` with
`restart_outcome_unknown`. It does not replay uncertain effects, including a
crash between admission and actual execution. Only never-claimed queued jobs
remain eligible. The queue engine preserves running status when adding trace
metadata to a claimed job.

`POST /api/v1/job/cancel?job_id=...` commits a cancellation transition before
reporting success. A DB-only queued job becomes cancelled atomically. A running
job retains its state and receives a sticky stop-request flag; stopping an
already-started effect remains best-effort. Persistence failure returns 503
with `live_cancel_requested` rather than claiming durable cancellation.
Unknown/already-terminal jobs return 409 and are never resurrected.

## Boundaries and recovery decisions

This is a conservative browser submission guard with bounded reconciliation,
not server-side content idempotency or exactly-once external execution. It does
not atomically lock multiple browser tabs/devices. Clearing browser storage
removes local protection. Malformed, older unversioned or wrong-session receipts
remain held; they are not silently upgraded or used to select a job.

A duplicate supplied `job_id` is rejected, not accepted as equivalent content.
Deleting durable history removes that identity protection. A lost terminal
effect receipt remains uncertain. Cancellation neither proves nonexecution nor
undoes effects. A synchronous request generally has no asynchronous job receipt;
inspect its trace rather than starting a replacement blindly.

## Reproduce qualification

From the repository root, run the canonical provider-free lane:

```sh
AGENT_E2E_BROWSER_EXECUTABLE=/path/to/installed/chromium \
  tools/verify_current_snapshot.sh
```

The browser path is host-specific; dependency installation is in
[Operations](OPERATIONS.md). The canonical lane now includes the native session
paging, job lookup/admission/cancellation and CLI session-store checks, plus
browser recovery, context paging, workflow submission and job-watcher tests.

For focused browser verification, run from `ui/`:

```sh
AGENT_E2E_PREFER_DEVSTACK=0 AGENT_E2E_RECORD=0 \
AGENT_E2E_BROWSER_EXECUTABLE=/path/to/installed/chromium \
  npx playwright test e2e/chat_submission_fence.spec.ts \
    e2e/chat_recovery.spec.ts --workers=2
```

The tests mount the actual React run hook with intercepted transport. They cover
automatic original-job adoption, delayed/missing receipts, bounded reads,
StrictMode reload, storage failure, invalid metadata and changed-session/auth
late responses. Native Python fixtures start temporary authenticated daemons
with loopback providers and disposable SQLite state; they exercise admission
failure, process death, duplicate IDs, restart uncertainty and durable Stop.
No production service or real provider is used. A browser fixture pass is not
live-provider, physical-device or deployed-system qualification.

Implementation: [`useRunExecution`](../ui/src/hooks/useRunExecution.ts),
[`useChatJobRecovery`](../ui/src/hooks/useChatJobRecovery.ts),
[`chatJobLookup`](../ui/src/api/chatJobLookup.ts),
[`jobStore`](../ui/src/jobStore.ts),
[`run admission`](../daemon/src/run_endpoints.cpp) and
[`job lookup/cancellation`](../daemon/src/job_endpoints.cpp).
