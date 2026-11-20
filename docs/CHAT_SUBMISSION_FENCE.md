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
durable browser handoff. Accepted jobs remain pinned until their matching native
terminal result is received and their retirement is saved. A synchronous
terminal response clears its receipt even when the run reports failure. An
uncertain synchronous response stays held.

The unused async-to-sync fallback helper was removed. A transport or parsing
error never switches execution endpoints or issues a second POST.

## Keep the accepted job authoritative

`agentui.jobsBySession` is the single checked local job-watch store. Its outer
JSON value is a string containing the JSON scope map. Scope includes the selected
connection and session. React reads this store; it does not keep a competing
best-effort preference copy that can overwrite a newer acceptance.

| Local entry | Meaning | Preference reconciliation |
| --- | --- | --- |
| `local_state: "accepted"` | This browser saved a positive native acceptance or validated original-job lookup. | Preserve the exact job ID and local cursor regardless of remote timestamps. |
| `local_state: "retired"`, `retired_unix_ms` | A terminal native result for this exact job was received and the checked local write completed. | Preserve the marker; do not restore a remote watch for this scope. |
| No local state tag | A saved recovery hint, without native acceptance proof. | Keep an existing local selection; import remote hints only for absent scopes. |

A server preference is a hint, not a run receipt. Its parser strips local state
tags and unknown fields, ignores invalid entries and accepts only safe
nonnegative integer cursors/times. A future timestamp never chooses a different
job. A delayed preference GET merges against a fresh local read after its reply,
so acceptance in another session while that GET was pending remains intact.

A native completion retires only the matching job ID. A late completion for an
older job cannot retire its replacement. The watcher saves retirement before
clearing the selected job; a failed storage write retains the original watch
and continues read-only reconciliation. Direct sends and automatic queue
dispatch synchronously inspect saved watches, including before mutation
submission. This closes the reload interval before React restores the selected
job. A missing result or unavailable/invalid local storage never releases queued
execution. Other scopes continue independently.

The store holds at most **64 entries**. Accepted entries do not expire and cannot
be evicted to make room for hints. Attempting to save a 65th accepted scope
fails before changing the stored map; an already accepted request retains its
submission receipt for reconciliation. Resolve original jobs and save their
terminal results to release pinned capacity. Retired markers outrank hints when
capacity is needed. Unpinned hints and retirement markers expire after **seven
days** using their saved update/start or retirement time, and remaining unpinned
entries can be evicted at the capacity limit. The retirement history is bounded:
a marker removed by expiry, eviction or clearing storage cannot prevent a stale
server hint from returning. Such a hint still authorizes only native reads, never
an execution retry. Wall-clock changes can affect this cache retention; they
cannot expire an accepted pin or select another job over a retained local entry.

Metadata uploads omit retirement entries and local authority tags. They are
coalesced to the latest local state, with at most one client request in flight
per preference context and a **five-second monotonic-clock interval** between
starts. Preference GETs and POSTs have **five-second deadlines**. Changing the
preference endpoint, client ID or authorization aborts that context's requests,
cancels its queued timer and fences late replies. A new context reads and uploads
its own current hints. Failed sync falls back to local watch operation; this is
best-effort metadata persistence, not a server write-order or cross-device
admission protocol. A timed-out server write may still commit after client abort.

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
    e2e/chat_recovery.spec.ts e2e/run_watch_identity.spec.ts \
    e2e/job_watcher_recovery.spec.ts e2e/local_job_controls.spec.ts --workers=2
```

The tests mount the actual React run hook with intercepted transport. They cover
automatic original-job adoption, delayed/missing receipts, bounded reads,
StrictMode reload, storage failure, invalid metadata and changed-session/auth
late responses. The combined watch/dispatch/stream fixture also checks delayed
future-dated preferences in direct and broker modes, old accepted pins on reload,
exact native completion, independent scopes, local storage faults, authority
tag stripping, capacity/retention and coalesced synchronization after clock
rollback. Native Python fixtures start temporary authenticated daemons
with loopback providers and disposable SQLite state; they exercise admission
failure, process death, duplicate IDs, restart uncertainty and durable Stop.
No production service or real provider is used. A browser fixture pass is not
live-provider, physical-device or deployed-system qualification.

Implementation: [`useRunExecution`](../ui/src/hooks/useRunExecution.ts),
[`useChatJobRecovery`](../ui/src/hooks/useChatJobRecovery.ts),
[`chatJobLookup`](../ui/src/api/chatJobLookup.ts),
[`jobStore`](../ui/src/jobStore.ts),
[`watch preferences`](../ui/src/runWatchPrefs.ts),
[`watch synchronization`](../ui/src/hooks/useRunWatchState.ts),
[`native result watching`](../ui/src/hooks/useJobStreaming.ts),
[`run admission`](../daemon/src/run_endpoints.cpp) and
[`job lookup/cancellation`](../daemon/src/job_endpoints.cpp).
