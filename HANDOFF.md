# Agent-C developer handoff

Updated 2026-11-21. Continue work in the existing checkout; this is a rolling
project with no compatibility requirement.

## Read the current source through its contracts

The [current snapshot](docs/CURRENT_SNAPSHOT.md) owns implemented scope,
qualification and deployment distinctions. The [architecture](docs/ARCHITECTURE_CURRENT.md)
owns direction and delivery gates. [TODOS.md](TODOS.md) owns remaining work.
The [documentation index](docs/README.md) maps developer tasks to the maintained
contract. Avoid copying historical milestone lists into a second current status.

Recent implementation includes immutable saved SOPs, durable decisions, worker-free
relative/absolute timers, early external signal slots, conditional actions,
bounded model/session/image/delegated continuation, signed device receipts and
single-authority broker goal coordination with advisory wakeup. The snapshot
links their exact supported scope and limits. General signal inbox/broadcast,
role-bound business approvals, dynamic transitions/loops and compensation remain
future work; the implemented bounded contracts are not those broader features.

The current consolidation also covers browser chat uncertainty and context
retrieval. [Chat recovery](docs/CHAT_SUBMISSION_FENCE.md) describes automatic
bounded read-only original-job lookup, saved watch handoff and independent
scoped queues. Accepted-job pins and exact-job retirement now outrank delayed
server preferences; reload checks the saved watch before queue dispatch, and
metadata sync is context-cancelled and coalesced. The same contract documents
64-entry capacity, seven-day unpinned history and storage-failure holds.
[Session pages](docs/SESSION_MESSAGE_PAGES.md) describe the
optional bounded model-context inspector. Neither feature establishes permanent
conversation IDs or exactly-once external effects.

## Verify a source milestone

From the repository root:

```sh
AGENT_E2E_BROWSER_EXECUTABLE=/path/to/installed/chromium \
  tools/verify_current_snapshot.sh
tools/verify_current_snapshot.sh --core-only
tools/verify_repo_guards.sh
```

Use an actual installed browser path. On the current Arch host it is
`/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome`.
Set `AGENT_E2E_UI_PORT` to an unused port when needed. Use assertion-enabled
`build-snapshot/` and `build-snapshot-core/`; do not reconfigure the separate
production `build/` directory for Debug tests. Dependencies and broader checks
are in [Operations](docs/OPERATIONS.md).

The canonical lane now includes the recent native session paging, job admission,
trace lookup, durable cancellation and CLI session-store checks, plus browser
submission recovery, context inspection and job watching. Earlier source
qualification remains in the [dated archive](docs/reports/qualification-through-20261118.md).
Use the current snapshot's latest result instead of treating old test counts as
current acceptance. Do not use CTest numeric indices across differently filtered
suites; select exact test names.

## Preserve deployment and publication boundaries

Production Agent-C remains stopped under the
[2026-11-12 owner hold](docs/ARCH_HOST_DEPLOYMENT.md#owner-stop-2026-11-12).
The last recorded production source is historical; a source commit or local
fixture pass is not a deployment. The separately authorized local model profile
is maintained by the [AI Model runbook](https://github.com/lzbgt/tiny-multimodal-edge-ai/blob/main/docs/local-4b-agent.md)
and [Agent-C local integration](docs/LOCAL_MODEL_WEB.md).

Publish reviewed commits to GitHub `origin/master` and verify the remote revision.
Panda publication remains excluded; do not recreate its Agent-C repository.
Preserve private configuration, local host instructions, unrelated working edits
and other deployments. [Publishing policy](docs/PUBLISHING.md) owns the procedure.

## Continue the next concrete requirement

1. Continue general correction/forget propagation after the [shared memory view contract](docs/MEMORY.md#current-views-and-original-evidence).
   Reuse the store, projector, exact original reader and revision checks. Durable
   [marker promotion](docs/MEMORY.md#durable-marker-promotion) now atomically records
   consumed observations and fences replacements; current consumers invalidate
   changed marker derivations. [Source retirement](docs/MEMORY.md#source-retirement-and-retention)
   now preserves originals through revision-checked removal and bounded partial
   outcomes. Registered cache purge, live current correlation and recap provenance checks now cover those consumers. Explicit memory-root archive expiry preserves active pins and metadata intents. [Promotion receipt compaction](docs/MEMORY.md#compacted-receipts-and-recovery) now keeps consumed IDs, literal original receipts and leaf pins across atomic publication; the current source format has no automatic v1 conversion. Next close expiry-intent metadata compaction, broader bounded forgetting, generic conclusion/claim validation and direct recap pre-provider crash admission.
   Keep history/forget policy explicit; invalidation never replays an external
   effect. Linux fixtures do not qualify Windows runtime or storage power loss.
2. The [bounded retrieval foundation](docs/MEMORY.md#bounded-evidence-retrieval)
   now implements foreground lexical/rerank/one-expansion arms and a typed
   TypeSafe adapter. Freeze the independent 20-question held-out corpus and
   qualify actual evidence/answer improvement; local fixture success is not that
   measurement. Preserve original prepared/completed research receipts and do
   not replay an uncertain paid call into a new output path.
   The [architecture evaluation gate](docs/ARCHITECTURE_CURRENT.md#memory-authority-and-retrieval-evaluation)
   records the owner-authorized TypeSafe/Jev canary and 20-question pilot bounds.
   The original canary stopped on HTTP 451 during discovery, with no scoring
   POST. Public API checks explicitly report region unavailability. Keep this
   owner dependency scoped to evaluation and continue local corpus/validator
   work. After supported access becomes available, inspect the retained receipt
   and remaining budget before any scoring continuation; do not reset it.
   Retain the existing 4B answerer; no 9B substitution. Production remains stopped.
3. Extend uncovered preparation/native-tool continuity and trusted receipt
   resolvers. Native remote workflow calls now use a durable admission and
   original-workflow reads, including direct and broker proxy paths; this does
   not establish general multi-authority goal handoff or unknown host outcomes.
4. Strengthen permanent conversation identity/history, bounded consumers,
   executable requirement evidence and provenance-preserving handoffs. Keep
   content idempotency, synchronous reconciliation and cross-tab/device admission
   explicit separate contracts. Extend richer typed SOP transitions/inboxes and
   compensation only through the maintained source contracts.
5. Advance per-instance allocator/clock/cancel ports and a pollable embedded
   lifecycle for a concrete device milestone. Host checks do not qualify an MCU.

Choose work from actual source, failures and the prioritized roadmap. Update its
owning contract and reproducible verification in the same milestone. The broader
project remains active; completing a source slice is not completion of this roadmap.
