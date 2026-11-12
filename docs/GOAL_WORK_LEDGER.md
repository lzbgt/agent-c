# Requirement-driven broker goals

A broker goal can explicitly bind to one existing daemon/session `work_state`
ledger. The host SQLite ledger remains the only writable requirement/blocker
authority. Broker metadata stores bounded observations and dispatch admission,
not another copy that can independently complete requirements.

## Use it

1. Initialize requirements through the existing policy-gated `work_state` host
   tool (or `agent work`) in the intended session. Use dependencies, scoped
   blockers and evidence-bearing completion/resolution as described in
   [AUTONOMOUS_WORK.md](AUTONOMOUS_WORK.md).
2. Ensure the team has exactly one eligible persistent member matching the
   authority's **agent and explicit deployment**. Other team members are not
   dispatched for these bound continuation turns.
3. In Broker Console → Teams → Advanced → Orchestrator runs, enable **Use
   session requirements to drive this goal** and enter the three identifiers.
   The equivalent create metadata is:

   ```json
   {"work_ledger":{"agent_id":"agent1","deployment_id":"dep1","session_id":"goal-session"}}
   ```

4. Run the normal `agentd-orchestrator` loop. Creating a goal does not start a
   coordinator service. The default running-goal poll also includes only
   coordinator-managed ledger waits.

Bindings accept safe identifiers only, never arbitrary URLs or files. The
coordinator reads the fixed broker proxy path with `X-Agentd-Deployment`.
The new authenticated daemon endpoint is
`GET /api/v1/session/work?session_id=...`. It performs no mutations, does not
create missing sessions, rejects session/ledger/sidecar symlinks and special
files, and uses the administrator-configured session root. Local root ownership
remains part of the filesystem trust boundary.

A successful ledger mutation persists a stable `ledger_id`. A legacy ledger
without one can still be read; only its next successful mutation initializes
the identity. A bound coordinator waits for an initialized, nonempty ledger.
It pins the authority, ledger identity, monotonic revision and content digest.

## Progress and owner control

- A successful team turn is **not** goal completion. All requirements must be
  done, have recorded evidence, have completed dependencies and have no open
  blocker affecting them. The ledger validates these structural conditions;
  evidence text alone is not an independent proof of semantic correctness.
- A blocker affects only its named requirements. Advisory blockers name none.
  Other runnable requirements continue in bounded, ordinary policy/budget-gated
  host turns on the authority session.
- A successful turn with unchanged requirement revision waits instead of
  repeating. A new verified ledger revision can wake a managed wait.
- Continuous coordinators use bounded **server-notified long polling** for
  requirement-blocked/no-progress waits, plus periodic reconciliation. No
  execution availability is implied when the coordinator is stopped.
- Owner pause, error/done, disabled autonomy and manual waits do not wake.
  A status-only PATCH to `waiting` clears managed wakeup. Bound heartbeats only
  renew leases; use the normal goal PATCH for status changes.
- Existing team quorum checks and host policy/approvals remain in force. Bound
  turns disable runtime allocation/spawning and pin the exact session even
  when member overrides exist. This does not grant tool, deployment or
  publication permission.

UI progress is explicitly the **last observed** ledger state. It shows the
authority, observed revision, requirement readiness and why continuation is
waiting. Connection/team/selection fences prevent late replies from painting
another goal. Bound-goal updates use the displayed metadata as an exact CAS.

## Prompt requirement wakeups

The same authenticated read endpoint accepts an optional all-or-none cursor:

```text
GET /api/v1/session/work?session_id=goal-session&after_ledger_id=work_123&after_revision=7&wait_ms=4000
```

A changed ledger identity or revision returns the normal validated projection
with `work_watch: {"schema":"agentd.work-watch.v1","changed":true}`. Unchanged
timeout returns **204 with no body**. Replies are not cached. Invalid/incomplete
cursors or durations return 400. The safe, nonempty ledger ID and unsigned
decimal revision identify an observation, not permission to run work.

The daemon subscribes before reading so a commit between the read and wait is
not lost. Successful in-process ledger commits signal subscribers; reads and
rejected mutations do not. Other-process/CLI writes, replacement and deletion
are discovered with fallback reads at most 250 ms apart, excluding read/lock
time. Path/sidecar guards are rechecked after each wake. Watches do not create,
migrate or mutate storage. A replacement is only a hint: ordinary goal
reconciliation still rejects changed authority history.

The daemon caps watch wait at **5 seconds**, plus bounded SQLite read time, and
admits at most **32 concurrent watches per process**. Excess requests receive
429 with `Retry-After: 1`; ordinary reads stay available. A disconnected client
does not instantly interrupt this synchronous handler: its slot is released on
change, timeout or error. The coordinator cancels its own requests/workers on
shutdown or when their observation is no longer selected.

Continuous `agentd-orchestrator` loops watch only explicitly bound, initialized,
coordinator-managed requirement/no-progress waits with no active or uncertain
dispatch. The default cap is **16 distinct authority/cursor watches**;
`--ledger-watch-limit=1..64` adjusts it (`0` selects the default). Identical
cursors share a watch; excess goals retain polling. A host's 32-watch cap can
still apply across multiple coordinators. `--disable-ledger-watch` keeps
periodic reconciliation only, and `--once` starts no background watcher.

Wake hints are coalesced and **never persisted as progress, completion or
dispatch authority**. Each wake reruns normal goal/ledger reads, owner and lease
checks, evidence validation, policy and admission CAS. Pause, manual waiting,
disabled autonomy, protected ownership and terminal states stay stopped.
Existing explicitly allowed stale-lease takeover semantics are unchanged.
An unchanged/stale hint cannot cause a tight redispatch or watch loop.

Periodic reconciliation still discovers goals, checks active turns and pending
receipts, and covers watch limits, older daemons, authentication/connectivity
failures and restarts. Errors back off for 1..30 seconds; immediate unchanged
responses are throttled. Restart establishes fresh watches from authoritative
state. This is **not a durable event log, replayable push stream or exactly-once
event delivery**, and no event payload can clear an uncertain host outcome.

## Admission, crashes and uncertain outcomes

Every bound transition supplies `expected_status`, `expected_owner` and
`expected_meta`. The latter is an optional exact JSONB compare-and-swap on the
existing orchestrator PATCH API; mismatches return 409. Concurrent workers using
the same owner string cannot both admit a new turn from the same snapshot.

Before submission, the coordinator persists a unique dispatch nonce. The broker
atomically consumes it **before host dispatch**, checking the current goal,
binding, status and digest of the owner/goal/template/role-plan controls.
A later owner edit invalidates an unsubmitted admission. Pause after admission
does not erase an accepted run; explicit team cancellation remains the way to
cancel it. Receipt recording preserves a concurrent owner pause or ownership
change.

### Recover the original turn, not another submission

Broker migration `013_goal_dispatch_receipts.sql` adds a broker-owned dispatch
journal. The immutable identity includes team, goal, nonce, authority,
ledger identity/revision/digest and admission control digest. The journal insert
and full control CAS commit **together before any host call**. Resetting mutable
goal metadata cannot reuse an old nonce.

After the async host response, the original team row and receipt link commit in
one transaction. A bounded persistence context survives a lost/cancelled client
request. The read-only authenticated endpoint is:

```text
GET /v1/teams/{team_id}/orchestrator/runs/{orchestrator_run_id}/dispatches/{dispatch_id}
```

It uses the existing team-owner/admin automation access gate and returns
`Cache-Control: no-store`. There is no receipt upload, overwrite or repair API.

- **admitted:** the broker consumed the nonce, but has no durable team-row receipt.
  The host may already have accepted work. This is not a safe-to-retry signal.
- **recorded:** the immutable receipt identifies the original persisted team run.
  It proves correlation, not successful work or requirement completion.
- **404:** no matching durable broker admission is visible. The coordinator may
  have stopped before sending, the response may be unavailable, or the snapshot
  may predate this journal. Absence never permits an automatic resubmission.

The coordinator follows a lost response or restart with this exact bounded
lookup, not a team list scan or a metadata claim. It validates every identity
field, binds the original team run, then resumes normal team-status and
requirement-evidence checks. A known response must agree with the saved receipt.
No host work is resent by recovery.

Pending or temporarily unavailable receipts cause a managed **read-only** wait.
Polling can discover a late committed receipt, even if requirements have changed.
It cannot launch another turn or declare completion while the original outcome
is unknown. A mismatch stops automatic recovery. UI labels distinguish pending,
unavailable, mismatched and recovered receipts without offering a blind retry.

Owner pause, manual wait, done/error, disabled autonomy and ownership changes
remain authoritative. Recording a late receipt preserves those controls; it
never cancels or restarts an already accepted host turn. Pausing a goal is not
team cancellation. Existing manual uncertainty waits are not automatically
migrated into managed waits; an owner can inspect and resume the existing goal
through its ordinary control path.

A crash after host acceptance but before receipt commit still leaves an unknown
outcome. This slice cannot reconstruct an unrecorded host job ID or prove
non-execution. Do not clear that admission to make the goal move. Broader trusted
host/transport receipt adapters remain separate work. This is conservative
duplicate suppression and original-result recovery, **not distributed
exactly-once execution**.

## Scope and remaining work

Run one active bound goal per authority session. This slice does not add a global
cross-goal authority lease; single-use admission is scoped to each goal.
Unbound goals retain their existing legacy behavior. This slice does not
centralize arbitrary team members' local ledgers, implement multi-authority
handoff, durable replayable event streams, lifetime goal resource budgets, semantic proof checking,
or reconstruction of unrecorded host outcomes. The existing per-turn host limits still
apply. Do not attach a binding to an already active unbound turn and assume its
receipt belongs to the new authority.

Source/API/UI updates require a coordinated authorized release. No production
deployment is part of this source milestone.

## Verification

`tools/verify_current_snapshot.sh` includes native ledger identity tests,
real authenticated daemon read/watch/auth/path/empty-state checks, Go coordinator
outcome tests, and the old/new orchestrator browser flows. Watch tests cover
native commit-before-wait races, external-process changes, replacement/deletion,
unsafe paths, capacity/timeout cleanup, prompt continuous-loop continuation
before a 30-second poll, owner controls, stale/untrusted hints, response bounds,
cancellation, older-daemon fallback and repeated race-detector runs.

For additional race/integration checks:

```sh
(cd broker && go test -race ./cmd/agentd-orchestrator -run 'BoundGoal|LedgerWatch' -count=1)
(cd broker && AGENT_TEST_POSTGRES_DSN='<disposable PostgreSQL DSN>' \
  go test -race ./internal/broker -run TestWorkLedger -count=1)
```

The PostgreSQL tests create/drop only uniquely named test schemas and apply the
full production broker migrations twice. They skip when the explicit test DSN is absent.
Use a disposable database, not production. The November 12 qualification used a
localhost-only temporary container from the already installed PostgreSQL 16
image and removed it afterward.

Receipt qualification also covers concurrent writers, transactional rollback,
nonce reuse after mutable metadata reset, goal/contract/role-plan races, scoped
HTTP authorization, an actual async handler with client loss, and abrupt child
process exits before/after receipt persistence. A durable synthetic host
acceptance counter proves both restart paths reject replay without another
acceptance. Coordinator tests cover lost/delayed/missing/mismatched receipts,
fresh coordinator state, owner-control races, redirect refusal and response
bounds. These are provider-free, disposable tests, not production deployment
or physical host-effect qualification.
