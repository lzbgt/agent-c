# Roadmap / TODOs (highest leverage)

## Current architectural priorities (2026-11-21)

The authoritative capability/verification snapshot is [docs/CURRENT_SNAPSHOT.md](docs/CURRENT_SNAPSHOT.md). Current direction and acceptance gates are in [docs/ARCHITECTURE_CURRENT.md](docs/ARCHITECTURE_CURRENT.md). The dated subsystem backlog is preserved separately below; it does not override these priorities.

- [x] Revision-checked atomic memory publication: shared host writer lock, exact-byte hashes, retained originals, strict expected revision, consolidation/retention current-head CAS and workflow conflict refusal. Local process-crash and concurrency checks cover this source slice; no production race or power-loss qualification is claimed.
- [x] Shared current/historical/evidence views: hard eligibility before FTS/fallback/salience, scope-restricted scoring, content-revision refresh, original source/revision/span references and checked context snapshots. Native/API head reads and recap preparation use the same authority; unversioned recap/assistant hints are not injected.
- [x] Durable marker promotion: atomic fact/receipt publication, consumed intent IDs, per-record revision-bound replacement, explicit partial outcomes and source-dependent eligibility across current consumers. Synthetic process-crash, conflict and capacity checks qualify this source slice.
- [x] Revision-checked daily/checkpoint retirement: retain originals before removal, preserve concurrent corrections, bound/cancel passes, keep newest checkpoints and report independent partial/uncertain results through API/UI. Auxiliary pruning uses the same writer lock. Active-file caps exclude archives.
- [x] Registered cache purge before source publication, FTS scope/view scrubbing, live current correlation and exact JSON evidence decoding. Versioned recap manifests revalidate inputs on reads; immutable incomplete provider outcomes survive HTTP/UI failure paths without automatic replay.
- [x] Explicit bounded memory-root archive expiry: preserve active revision pins, capture complete inventories, persist metadata intents before unlink and expose honest partial/uncertain API/UI results. Ordinary writes preserve history; expiry defaults to zero. External consumers need active memory pins.
- [x] Bounded promotion receipt compaction: preserve every consumed ID, retain one-hop original full receipts and marker revision pins, publish compaction/facts atomically, separate new-intent admission from duplicate scans and expose omitted detail. Real multi-batch daemon and synthetic crash/corrupt-original/expiry checks cover this source format; no live state conversion or total archive reduction.
- [x] Foreground bounded retrieval foundation: common question-term FTS baseline, typed pluggable relevance scorer, one source-grounded unseen-passage expansion, exact evidence trace, whole-record context budgets and explicit lexical fallback. TypeSafe adapter has fixed official endpoint/model pin, per-batch attempt/judgment/token reservations, bounded retries/cancellation and literal credential loading. Local/loopback/CLI fixtures qualify reliability, not retrieval quality or final answers.
- [x] Freeze independent synthetic 20-case corpus/labels before measurement; validate exact evidence/eligibility/scope and record A with explicit denominators, missing arms and cold/warm index observations. A recovered 26/28 records with macro recall 90.625% and complete evidence 14/16; no B/C or final-answer claim. Do not tune on this frozen screen.
- [ ] P0 (next owner priority): expiry-intent metadata compaction, generic conclusion/claim validation, direct recap pre-provider crash admission, bounded forgetting and evidence-preserving handoffs. Never replay a completed external effect. Currentness is declared eligibility, not semantic verification. Windows runtime and filesystem power-loss qualification remain separate.
- [ ] P0 (retrieval quality gate): measure B/C evidence and unchanged-4B supported answers on the frozen screen. A and reference/metric validation are recorded. TypeSafe canary discovery received HTTP 451 with zero scoring POSTs; supported provider access is an operation-only owner dependency. No paid replay or regional bypass. Corpus/policy must stay frozen; use separate tuning data for future improvements. Caps, metrics and promotion gates are in [the architecture](docs/ARCHITECTURE_CURRENT.md#memory-authority-and-retrieval-evaluation).
- [ ] P1: claim-to-versioned-passage web evidence, executable versioned requirement validators, direct CLI/local-chat lifecycle coverage and provenance-preserving correction/handoff consolidation. Reuse current journals and keep portable C boundaries.
- [x] Stage/action SOP compiler through the existing workflow submit path, version/hash provenance and WebUI template.
- [x] Shared durable requirement/blocker ledger; independent work remains runnable; evidence-gated state transitions.
- [x] Portable proposed-completion hook with host continuation within existing run budgets.
- [x] CLI exec/stdin/resume/sessions/work commands and autonomous default instructions.
- [x] Versioned CLI JSONL stdout, successful final-message publication, complete coalesced SSE delivery and bounded audit capture independent of live events.
- [x] Modern responsive workspace shell with accessible navigation and deferred advanced panels.
- [x] HTTP POST effect IDs, durable intent/receipt/reconciliation, API/WebUI recovery and real process-crash tests.
- [x] Deploy the verified snapshot on Arch behind the shared VPS reverse proxy/OAuth; qualify the pinned system administrator and inspect only proven Agent-C VPS workloads (none were present to remove).
- [x] Make each workspace tool the primary page, split workflow runs/authoring/schedules, preserve composer drafts and verify phone/tablet layout and modal keyboard access.
- [x] Put messages and results first, group metadata behind closed details, simplify the composer, and lead settings with subject options while preserving authorized browser actions.
- [x] Remove duplicate conversation renderers and developer setup notes; simplify Memory, voice, traces, comparisons, decisions and workflow lists around content/actions.
- [x] Give canvas/DOM presentations their own event and clock lifecycle; preserve animation while typing/polling, dispose detached clocks and remove the composer overlay.
- [x] Aliyun Omni realtime audio/image voice with server-held Model Studio credentials, bounded device/media lifecycles, one-caption replies and synthetic provider roundtrip qualification; see [voice](docs/VOICE.md).
- [x] Atomic device assignment/outbox/rate admission, immutable operation identity, pinned recovery and visible scoped device uncertainty; verify crash recovery and one-worker independent progress.
- [x] Transactional workflow budget reservations/settlement, conservative lost charges, bounded parallel admission and retry-preserving capacity waits; one-shot model step accounting.
- [x] Signed UM-BMP device receipt resolver: immutable original-result receipts, late-outcome recovery, no redispatch, preserved owner stops and API/WebUI control.
- [x] Deterministic `host_tool` workflow/SOP actions: guarded intent/result capture, no blind retry after interruption, existing policy/approvals and budget accounting, saved-result recovery and scoped UI uncertainty.
- [x] Conservative whole-run guard for ordinary workflow/SOP model-selected host tools: policy-first lazy admission, no blind retry after incomplete runs, saved-answer recovery before fresh grants, scoped operator blocker and real daemon crash tests.
- [x] Bounded durable provider/tool transcript continuation for ephemeral ordinary host-model workflow/SOP actions: exact replay of saved model decisions/tool receipts, zero-cost cached calls, current-policy fresh dispatch and safe provider-failure/crash recovery. Unreceipted calls remain blocked.
- [x] Session-backed ordinary host-model continuation: durable conversation generations, no partial-attempt writes, stale-edit fences and atomic final conversation/model receipt publication; real daemon crash/edit recovery verified.
- [x] Durable image-description preparation in host-model workflows: exact cached interpretation/fallback, image/context fingerprints, shared fresh-step/token accounting, unchanged permissions and real daemon crash/session-edit recovery.
- [x] Sequential delegated host-model continuation: distinct parent-owned child journals, completed-prefix replay, shared-session fences and verified partial checkpoints with exhausted tool/step grants (2026-11-18). Cached work stays free; fresh calls, uncertain outcomes and unfinished later children remain guarded.
- [ ] P0: remaining uninstrumented preparation/native-tool paths, trusted host receipt resolvers and additional device adapters; strict provider/adapter resource metering. Sequential delegated host-model children are covered by the bounded contract above, not universal delegated/parallel replay.
- [x] Bind broker goals to an explicit host requirement/blocker authority: evidence-gated completion, independent work, revision-driven managed wakeup, no-progress waits, exact control CAS and single-use dispatch admission; preserve owner stops and expose UI setup/progress.
- [x] Durable native `agentd_call` admission and exact original-workflow recovery: database-scoped remote identity, no submission replay after response loss/restart, bounded authenticated lookup, saved terminal receipts before settlement, current network grants, owner stops and scoped API/UI reconciliation.
- [x] Trusted broker dispatch receipt recovery: immutable single-use admission, full control CAS, atomic original team-row/receipt persistence, bounded authenticated lookup and owner-preserving recovery after response loss/restart; no host resubmission.
- [x] Bounded server-notified requirement wakeup: native commit signals, authenticated capped long-poll reads, cross-process fallback, coalesced/cancelled coordinator watches and fresh authoritative reconciliation; preserve owner stops and degrade to polling.
- [ ] P0: multi-authority handoff and trusted reconstruction of unrecorded host dispatch outcomes. Current bound-goal scope uses one existing authority with advisory notifications plus polling; durable replayable event streams remain open and missing receipts never authorize retries.
- [x] Immutable daemon-local SOP library: save/list/read exact versions, canonical conflict detection, hash-pinned run snapshots, restart integrity and composer save/use flow.
- [x] Durable explicit process decisions: configured continue/fail choices, worker-free waits, immutable idempotent answers, expiry/cancellation, pinned-SOP integration and operator UI.
- [x] Restart-safe worker-free delay timers: atomic arm/yield, preserved due times, cancellation/deadlines, idle-budget semantics and visible scheduled waits.
- [x] Absolute-time workflow/SOP timers with strict timestamp admission, fixed persisted due times beyond the relative delay cap, worker-free restart/cancellation behavior and typed API/UI support.
- [x] Activated single-delivery external JSON signals: atomic worker-free wait/wakeup, bounded payloads, exact immutable receipts, restart/expiry/owner-stop safety, repeated-SOP identity and guided/read-only UI.
- [x] Bounded conditional workflow/SOP actions (2026-11-18): strict typed result equality, automatic dependencies, repeat-aware pinned references, fail-closed validation and explicit effect-free skip receipts; guided editing and honest executed/skipped progress.
- [x] Opt-in pre-activation signal slots (2026-11-18): one immutable bounded input per submitted action, atomic creation, dependency/condition-safe buffering, activation-only expiry, unused receipts and guided/read-only UI.
- [ ] P1: general signal inboxes, broadcast/publisher authorization and richer timer contracts (calendar/timezone/recurrence), general branch transitions/dynamic loops, person/role-bound approvals, compensation and active-run version migration.
- [ ] P1: instance-scoped pollable core ports and board-level resource/power-loss evidence.
- [ ] P1: provider metadata/checkpoint contracts, mid-turn steering and remaining legacy CLI decomposition.
- [x] Guided SOP stage/action authoring: names, sequencing/parallelism, stage/action ordering, model instructions, waits and review questions; preserve advanced fields and keep raw execution configuration behind advanced controls.
- [x] Typed decision/UTC-timer authoring: configured choice IDs/labels/outcomes, expiry, exact millisecond UTC waits and real daemon execution of browser-authored saved SOPs.
- [ ] P1: richer typed SOP authoring for advanced device/host/integration actions and role-bound approval contracts.
- [x] Workflow composer submission reconciliation: persisted secret-free scoped receipt before dispatch, authenticated read-only lookup, response-loss/reload recovery and no automatic execution retries.
- [x] Bounded canonical-session snapshot paging and opt-in history inspector: keyset SQL, durable revision fences, strict row/byte caps, restart-safe cursors, typed client validation and one-page browser retention. See docs/SESSION_MESSAGE_PAGES.md; not append-only history.
- [x] Conservative chat submission guards, insert-only durable async admission, authenticated trace/session lookup, restart uncertainty and durable cancellation.
- [x] Automatic bounded read-only original-job recovery after lost chat replies/reload; durable watch handoff, context/auth late-response fencing, independent keyed browser queues and collapsed recovery metadata. No execution POST is retried.
- [x] Preserve locally accepted current-job identity across delayed preference replies and clock skew; checked local pins and exact-job retirement outrank server hints. Reload/queue guards, independent scopes, storage failures, bounded history and context-cancelled metadata sync are covered without replaying execution.
- [x] Consolidated canonical checks include the recent CLI snapshot, session paging, durable job and browser recovery contracts. The rolling snapshot and developer handoff supersede outdated milestone summaries; dated evidence remains archived.
- [ ] P1: append-only conversation IDs/history and migration of remaining UI consumers to bounded pages; replace model-context snapshots as the long-term chat authority. Server-side content idempotency, cross-tab/device admission and synchronous-run reconciliation remain separate contracts.

- [ ] P1: investigate the single intermittent blank workspace after reload observed in the 390-pixel scene test during archive qualification. Six focused repeats and a separate full browser rerun are recorded; no product fix/root cause is claimed. The canonical verifier now retains a trace for any future failure.

## Additional subsystem qualification

- [ ] Provider coverage: keep fixture/streaming matrices aligned with advertised adapters; update live-provider pins only within an authorized qualification scope. Historical credential failures are not current owner blockers without fresh verification. The production shutdown remains effective.
- [ ] Native voice transport: run the documented non-skipped graduation gate on each additional release platform before promoting its native-plugin default. The earlier macOS M2 result does not qualify other hosts. See [native voice graduation](docs/VOICE_NATIVE_PLUGIN_GRADUATION.md).

## Historical subsystem backlog

The [earlier goals, shipped slices and subsystem details](docs/reports/subsystem-roadmap-history-20261120.md)
remain available with their original evidence and unresolved requirements. Before
promoting an old item, compare its source and current contract, then merge the
remaining acceptance into this list. Do not reimplement a delivered capability
because its old priority label survived in a historical note.
