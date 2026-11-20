# Current Agent-C snapshot

Date: 2026-11-21. This document records implemented behavior and reproducible checks. It is the entry point for the current snapshot; [architecture and delivery gates](ARCHITECTURE_CURRENT.md) describe the longer-term design and [research](research/agent-systems-20261110.md) records inspected primary sources.

This rolling summary covers current source. Dated deployment and earlier test
results remain explicitly historical; the source tree does not imply that
production services were restarted.

## Implemented in this snapshot

- **Requirement-driven broker goals:** explicit agent/deployment/session binding keeps the host ledger authoritative. Success requires evidence-bearing completed requirements, independent work continues past scoped blockers, changed revisions wake managed waits and unchanged progress stops. Single-use admission and exact control CAS prevent duplicate/stale launches. Immutable broker receipts recover the original persisted team run after response loss or restart, never by resending it. Unrecorded host outcomes remain pending, mismatches stop recovery and owner controls are preserved. UI setup, progress and receipt states are included. Managed requirement waits now use bounded server-notified long polling with ordinary reconciliation fallback, coalesced hints, explicit watch caps and cancellation. Every hint triggers fresh goal/ledger checks, not dispatch permission. This remains single-authority coordination, not a durable event stream or reconstruction of an unknown host outcome. See [goal ledger contract](GOAL_WORK_LEDGER.md).

- **Atomic memory publication (source only):** memory replacement/upsert requires the exact SHA-256 revision read from the file, or explicit absence for a new source. Memory tools share a root-wide OS writer lock; required prior originals are retained before synced atomic replacement. Exact historical reads verify their content hash and never substitute current bytes. Consolidation and retention carry the snapshot they derived from; native workflow conflicts stop retry while independent work continues. Checkpoints are auxiliary immutable objects, including separate paths for updates in the same second. Scoped retrieval and original references now build on this store; generic correction/forget propagation remains open. See [memory publication](MEMORY.md#revision-checked-publication).

- **Scoped memory views and original evidence (source only):** current/historical/evidence projection is shared by FTS, substring fallback, structured queries, salience and all context modes. Current state is filtered before scoring; only the selected scope/view enters BM25. Content revisions replace timestamp-only refresh. References bind source, revision and byte span/JSON Pointer with exact original reads and checked output frontiers. API/native current queries use the head; historical checkpoints are explicit. Current context excludes unversioned recap/assistant hints and reports unavailable retrieval; recap publication checks inputs after the provider response. See [memory views](MEMORY.md#current-views-and-original-evidence). Generic conclusion/claim and forget propagation remain open.

- **Durable marker promotion (source only):** facts and consumed-observation receipts publish atomically in the existing structured authority. Stable intent IDs prevent an old marker from undoing a later correction. Replacement markers require a per-record revision; conflicts, malformed input and capacity deferral stay explicit while independent facts can commit. Current consumers validate original marker dependencies before scoring/output; history retains provenance. The current v2 ledger compacts old full receipts into exact consumed IDs and one-hop retained originals, preserving leaf revision pins. New-intent admission is separate from bounded duplicate scans; omitted report detail is explicit. No automatic v1 conversion or total archive reduction is implied. See [marker promotion](MEMORY.md#durable-marker-promotion) and [compaction recovery](MEMORY.md#compacted-receipts-and-recovery).

- **Revision-checked source retirement (source only):** retention captures revisions, retains originals and removes selected active heads under the shared writer lock. Checkpoint count policy keeps newest timestamps; failures/uncertainty/deferred work preserve full API/UI reports and truthful confirmed counts. Preview is read-only, passes are bounded/cancellable, and auxiliary checkpoint pruning reuses the same authority. Active-byte limits exclude archives; hard forgetting and archive expiry remain separate. See [source retirement](MEMORY.md#source-retirement-and-retention).

- **Cache and recap invalidation (source only):** source publication purges registered disposable caches/sidecars under a cross-process lease; FTS view rebuilds use checked SQLite/FTS secure deletion. Correlation projects eligible current sources directly. Recap originals use atomic publication and versioned derivation manifests; list reads revalidate source IDs, revisions and eligible references. Stale/unverified results remain historical. Incomplete provider outcomes preserve full API/UI reports and never auto-retry. Exact JSON decoding keeps evidence fields literal and rejects unsupported current schemas. See [cache boundaries](MEMORY.md#snapshot-and-cache-boundaries) and [recaps](MEMORY.md#memory-recaps-llm-summaries). Input currentness does not verify claims; direct recap pre-call crash admission and generic forgetting remain open.

- **Explicit archive expiry (source only):** ordinary writes preserve original history. A positive `archive_max_days` cleanup policy captures active memory heads/literal revision references under the writer lock, protects matching originals, validates complete bounded inventories and persists metadata intents before unlink. Confirmed/uncertain/deferred results remain distinct through API/UI; zero preserves history. This covers memory-root retention, not external consumers without active pins, universal hard forgetting or forensic erasure. See [archive expiry](MEMORY.md#explicit-archive-expiry). Expiry-intent metadata compaction remains open.

- **Bounded retrieval research (source only):** a foreground command shares captured scope/eligibility and a question-term FTS baseline across lexical, rerank and one-expansion arms. Pluggable typed scoring validates exact IDs and finite probabilities; source-grounded terms carry seed → term → resulting-evidence traces. Whole-record context limits, cancellation/source fences, conservative TypeSafe token/attempt/judgment caps and explicit lexical fallback are implemented. Prepared/per-arm receipts survive interruption and refuse paid replay into an existing output. Local/loopback tests qualify reliability; held-out retrieval and unchanged-4B supported-answer evaluation remain pending. Normal search/context defaults remain lexical. See [research contract](MEMORY.md#bounded-evidence-retrieval).

- **Native remote workflow recovery (source only):** `agentd_call` saves a parent-owned admission before one remote POST, uses a database-scoped server-owned key and resumes only authenticated original-workflow reads. Exact identity/state checks pin the returned workflow; completed receipts save before parent settlement and replay without fresh network grants. Missing/corrupt outcomes remain scoped blockers; API/UI checks cannot replace the target, bypass budgets or revive owner stops. Direct and broker proxy fixtures use real temporary daemons and no provider. See [remote admission](WORKFLOW_EFFECTS.md#durable-remote-workflow-admission).

- **Model task continuity:** ordinary workflow/SOP host-model tasks have a durable effect guard. Ephemeral and session-backed tasks save exact bounded image-preparation/provider/tool transcripts and resume verified progress after provider failure or restart without repeating or recharging cached host calls. Current policy, fresh-call grants and owner stops still apply; pending/unusable receipts remain blocked. Saved-progress UI differs from unknown effects and adds no manual resume control. Persistent sessions pin a durable conversation generation and atomically publish final messages with the model receipt. Changed conversations stop without overwriting newer edits; the UI explains the conflict. The existing image-description prefetch is restored exactly instead of reinterpreting images on retry; fresh analysis shares provider-step/token accounting and cached analysis is free. Sequential delegated host-model children use distinct parent-owned journals, shared-session fences and free completed-prefix replay. Verified partial children can traverse cached records with exhausted tool/step grants while every new call remains grant-gated; elapsed/token admission and conservative lost charges are unchanged. Other uninstrumented preparation and parallel macro semantics remain excluded. See [delegated continuation](DELEGATED_CONTINUATION_DESIGN.md). See [model effect contract](WORKFLOW_EFFECTS.md#model-selected-host-tools).
- **Deterministic host actions:** `host_tool` workflows and SOP actions reuse daemon policy/explicit approvals, persist original intent/results, reject changed identities and never blindly repeat an interrupted effect. Saved results need no new tool charge or execution approval. Independent work continues; the UI explains unresolved host outcomes without offering an unsupported HTTP resolver. See [host effect contract](WORKFLOW_EFFECTS.md#deterministic-host-tools).

- **Autonomy:** default host instructions execute authorized work without permission prompts. The shared SQLite requirement/blocker ledger keeps independent work runnable, validates dependency DAGs, requires evidence for completion/resolution, and rejects stale revisions. A human dependency affects only its named requirements; advisory blockers can affect none.
- **Completion:** a portable C completion hook allows hosts to continue a proposed final response within the original budgets. The host uses it after a run adopts `work_state`; an unchanged-ledger repetition guard prevents endless false completion. Empty/unrelated ledgers are not automatically adopted.
- **Conditional process actions (source only):** individual workflow/SOP actions may compare a Boolean/text/null result from a successful earlier action. Strict validation and repeat-aware dependency mapping reject unsafe references. Non-matches persist explicit skipped receipts before templates, waits, budgets or effects; missing/wrong-typed results fail closed. Guided authoring preserves advanced settings and progress separates executed/skipped work. This is not a general branch engine or an approval. See [condition contract](SOP_WORKFLOWS.md#conditional-actions).
- **External process signals (source only):** single-use waits accept authenticated JSON objects up to 16 KiB. Explicit early-delivery opt-in opens one exact action slot atomically with submission; buffered input never advances dependencies, conditions or readiness. Expiry starts only at activation. Atomic wait/yield and immutable receipt/wakeup preserve independent work, replay, restart and owner cancellation. Future SOP iterations have separate slots; skipped/failed/stopped actions expose unused input honestly. Guided authoring and collapsed read-only metadata are included. No input before submission, general inbox, broadcast or per-publisher authorization is implied. See [signal contract](WORKFLOW_SIGNALS.md).
- **Durable process decisions:** explicit configured continue/fail choices, worker-free waits without consumed attempts or budget grants, immutable answer/wakeup transactions and idempotent receipts. Expiry and owner stops remain authoritative. The review template and run cards expose only configured choices; lost responses retain the same delivery. This is not person-level role approval. See [decision contract](WORKFLOW_DECISIONS.md).
- **Bounded conversation snapshots (source only):** authenticated, generation-fenced SQL pages cap 200 rows and 2 MiB raw text/media without truncation. The optional history inspector fetches 50 messages lazily, keeps one page, and discards a changed generation instead of mixing results. Other chat/audit consumers are separate; this is not append-only history. See [session pages](SESSION_MESSAGE_PAGES.md).
- **Chat submission recovery (source only):** a scoped secret-free receipt precedes POST. Lost asynchronous replies and reloads trigger at most three bounded authenticated GETs to find the original job, never another execution POST. Its watch is saved before clearing the guard; malformed, missing or ambiguous receipts remain held. Local accepted-job pins and exact-job retirement outrank delayed server hints independently of timestamps. Reload reads the checked watch before queue dispatch; completion storage failures retain the original watch. Preference synchronization coalesces writes with monotonic throttling, request deadlines and context cancellation. Accepted watches cannot expire or be evicted; unpinned history remains bounded. Session/auth changes fence late responses, and keyed browser state no longer carries queues into another scope. Native insert-only admission, restart uncertainty and durable cancellation are checked in the same verification lane. See [chat recovery](CHAT_SUBMISSION_FENCE.md).
- **Workflow submission recovery (source only):** secret-free client receipt persistence precedes POST; lost responses and reloads retain identity and block another composer submission. Authenticated session-scoped GET lookup finds the original workflow without executing. Missing/unsupported lookup never triggers a retry. Explicit warned local receipt release is not workflow cancellation. Native crash/restart and browser fault injection verify the boundary.
- **Saved SOP library:** authenticated daemon-local immutable versions, canonical conflict detection, metadata pagination and exact-version reads. `sop_ref` pins verified source into each run snapshot; creating a new version cannot alter an active run. The composer saves and loads references without implicit execution. Known inline credential fields are rejected; ordinary task/policy validation still runs on submission.
- **Guided SOP authoring (source only):** the composer edits process names/versions, ordered stages, sequential or parallel actions, model instructions, waits and review questions without requiring raw JSON. Decisions expose choice IDs/labels/outcomes and expiry; fixed waits use exact UTC milliseconds independent of browser timezone. Exact browser-authored definitions are tested through real temporary-daemon save/pin/run and continue/fail delivery. Advanced action fields survive guided edits; raw configuration remains available. Editing never saves or runs implicitly, and new model actions start with tools disabled. This is not a deployment claim.
- **SOPs:** `agentd.sop.v1` compiles ordered stages and sequential/parallel actions through the existing workflow submit API. A canonical definition hash and stage mapping are stored with the redacted spec. Strict compiler validation precedes ordinary task/policy/admission validation. Oversized persisted specs are rejected instead of truncated.
- **CLI:** command dispatch and output policy are separated from provider execution. `exec`, bounded stdin, `resume`, `sessions`, explicit working/session directories and model-free `work` commands reuse existing execution/persistence. `--json` emits versioned lifecycle/progress events; `-o` publishes final text after successful execution/session saving. Existing JSONL transcript export stays available. Resumed host prompts refresh when the built-in contract changes. See [CLI_EVENT_PROTOCOL.md](CLI_EVENT_PROTOCOL.md).
- **Streaming:** coalesced SSE packets drain through bounded parser batches; transport packet boundaries no longer truncate the checked streams. The host event log enforces its 4,000-event/2-MiB capture bounds while live callbacks continue after capture fills.
- **WebUI:** a lean responsive chat presents user messages and assistant outputs once, with a compact Attach/Send composer. System context, tools, traces and other metadata are grouped under closed Activity & details. Settings leads with model, capabilities and conversation; technical controls and management are closed by default. Real files, notes and visual results stay visible. ChatThread is the sole conversation renderer, including when details are expanded; obsolete copied context, per-run replies and the second team transcript are removed. Memory, voice calls, traces, run comparison, decisions and workflow lists lead with content/actions while tuning and evidence stay closed. Developer setup notes are removed from default pages. Tool pages remain primary, workflow runs/creation/schedules are separate, and authorized browser effects run independently of collapsed presentation. See [UI_DESIGN.md](UI_DESIGN.md).
- **Voice:** Aliyun Omni 3.5 Flash/Ethan streams mono 16 kHz microphone PCM, 24 kHz reply PCM and bounded JPEG images through the authenticated gateway. One transcript renderer avoids duplicate text/audio captions. Device capture and clocks stop on call end/navigation, and credentials stay server-side. [Voice contract and qualification](VOICE.md).
- **External HTTP effects:** guarded POST actions persist intent and response identity; uncertain writes reconcile through a same-origin receipt adapter or emit a scoped blocker. An explicit destination-idempotency contract permits safe retries. Recovery is inspectable and resumable through the API/WebUI, and independent workflow branches continue after a failure even with one worker. See [WORKFLOW_EFFECTS.md](WORKFLOW_EFFECTS.md).
- **Device actions:** assignment identity, active resource/rate admission, outbox publication and queued evidence commit together. Retries reuse the original node/operation; uncertain terminal device outcomes expose a required action without blocking independent scheduler branches. Matching signed late device receipts are retained independently of platform timeout/cancellation and can recover the original workflow through the API/UI without another command. See [DEVICE_ASSIGNMENTS.md](DEVICE_ASSIGNMENTS.md).
- **Workflow budgets:** durable attempt grants serialize capacity admission across workers; task/usage settlement releases grants atomically, while crash-lost grants remain conservatively charged. Capacity waits preserve attempts and independent work. One-shot model steps are metered. Token/time overages remain explicit; see [WORKFLOW_BUDGETS.md](WORKFLOW_BUDGETS.md).
- **Authenticated deployment:** an Arch gateway serves the modern UI and proxies private daemon execution with server-held credentials. It uses Panda's shared Hubstack OAuth with a separate application, immutable verified provider-proof pins and optional Agent-C product account pins, browser-bound one-use attempts, central revision checks and explicit member access state. A canonical schema fingerprint requires fresh deployment on incompatible change. See [ARCH_HOST_DEPLOYMENT.md](ARCH_HOST_DEPLOYMENT.md) for installation and rollout qualification.
- **Reproducibility:** npm lockfile tarballs use the canonical registry with existing integrity hashes and versions preserved. OpenAPI client types are regenerated. Local deterministic tests cover the new behavior without provider credentials.

Usage and exact contracts: [CLI](CLI.md), [work state](AUTONOMOUS_WORK.md), [bound goals](GOAL_WORK_LEDGER.md), [SOP workflows](SOP_WORKFLOWS.md), [embedded C](EMBEDDED_C_API.md), [WebUI](WEBUI.md).

## Deployment state and recorded qualification

The last recorded production qualification at **https://agent.hubstack.cn** used implementation revision **a0890c07a9dc53f1993ce726dea56aa04f156248**. This is historical evidence, not a claim that the service is currently running or that installed binaries match the current source. The production daemon, gateway and dedicated PostgreSQL container remain stopped under the [owner hold recorded on 2026-11-12](ARCH_HOST_DEPLOYMENT.md#owner-stop-2026-11-12). Later source integration and the separately authorized local 4B profile do not resume or update that production deployment.

The signed device receipt, guarded host action, saved SOP library, process-decision, model-transcript and requirement-driven goal source described here was not part of that recorded production release. Its deployment requires a separate authorized rollout and qualification. The production topology places execution, UI and its dedicated PostgreSQL on Arch; the existing VPS provides only Agent-C ingress and the shared identity service.

During the recorded qualification, the verified owner completed Apple SSO and reached the connected workspace. Public authenticated checks ran a four-action SOP and a bounded `deepseek-v4-pro` request. The voice WebSocket passed a synthetic ASR/image/PCM round trip with the expected image word. No existing Agent-C VPS runtime was found to remove; other deployments were preserved. See [production operations and qualification](ARCH_HOST_DEPLOYMENT.md). Current local-model operation and its dated reports belong to the [AI Model runbook](https://github.com/lzbgt/tiny-multimodal-edge-ai/blob/main/docs/local-4b-agent.md), with Agent C integration in [LOCAL_MODEL_WEB.md](LOCAL_MODEL_WEB.md).

## Publishing destination

Reviewed snapshots publish to [lzbgt/agent-c on GitHub](https://github.com/lzbgt/agent-c) through the existing `origin` remote. Panda publication is excluded. Preserve remote definitions, private configuration and unrelated working changes; see [publishing policy](PUBLISHING.md).

## Reproduce

Install the platform's normal CMake/C/C++/curl/OpenSSL/jsoncpp/SQLite/pkg-config dependencies, Python 3, Go 1.24+ and Node/npm as described in [OPERATIONS.md](OPERATIONS.md). The UI uses its committed npm lockfile. Install a Playwright Chromium browser if one is not already available:

```sh
cd ui
npm ci
npx playwright install chromium
cd ..
tools/verify_current_snapshot.sh
```

For a supported installed Chromium, set `AGENT_E2E_BROWSER_EXECUTABLE` to its executable path. `AGENT_E2E_UI_PORT` chooses an unused test port. The snapshot verifier retains traces on browser failure and disables video by default; set `AGENT_E2E_RECORD=1` for additional recording. It uses local model stubs and mocked browser APIs, not live provider keys or a production deployment.

For the embedded-safe host build/test lane:

```sh
tools/verify_current_snapshot.sh --core-only
```

The verifier configures **Debug** builds so assertion-based existing C tests execute. Do not interpret passing Release tests compiled with `NDEBUG` as equivalent coverage. Build artifacts live under ignored `build-snapshot/` and `build-snapshot-core/`; override `AGENT_SNAPSHOT_BUILD_DIR` if needed.

## Verification evidence

On 2026-11-21 (Asia/Shanghai), implementation **`f32f8c2`** passed **79
native/CLI/API checks in 159.12 seconds**, all canonical Go packages (cached where
eligible), OpenAPI/type checks, the UI production build and **144 browser checks
in 2.1 minutes** in one canonical pass. The
[retrieval-foundation receipt](verification/memory-retrieval-foundation-20261121.json)
pins source, commands, artifact/log SHA-256 identities, budgets, observed timings
and limits. Repository, document-link and whitespace checks accompany the
qualification. These are source checks, not a deployment or retrieval-quality
promotion.

The new fixtures qualify an unseen-record bridge, exact evidence reads,
current/historical and scope restrictions, question/input/context limits,
malformed or foreign IDs/scores, lexical fallback, source corrections and total
cancellation/deadline. Synthetic TypeSafe tests exercise conservative unknown
usage, bounded retry/admission, model pins and literal credential parsing.
Loopback HTTP covers response overflow, timeout and cancellation. The actual
foreground CLI fixture preserves prepared/completed originals, refuses an
existing output and narrows case scope without provider access. Existing memory,
workflow, CLI, Go and desktop/mobile browser failure cases remain in the lane.

One small synthetic Linux retrieval fixture took **0.044712 seconds** with
child peak RSS **12,788 KiB** after the canonical run. This is not aggregate
RAM, a full 64-MiB corpus test, model/VRAM/MCU qualification, an SLO or retrieval
quality. Reproduce the [focused research checks](MEMORY.md#verify-failure-boundaries)
or the canonical command above. Ignored logs include
`build/memory_retrieval_snapshot_final.log`, `build/memory_retrieval_focused_final.log`,
`build/memory_retrieval_before_commit.log`, `build/memory_retrieval_resources.log`,
`build/memory_retrieval_documentation.log` and `build/memory_retrieval_repo_guards.log`.

The separately authorized TypeSafe canary had a two-attempt/two-judgment ceiling
and 65,536 reserved input-token maximum (estimated $0.002752512 at the checked
price). It stopped on its **first authenticated discovery GET with HTTP 451**.
**No scoring POST was sent; candidate judgments and known model-input usage are
zero.** Two direct unauthenticated API diagnostics also returned the explicit
region-unavailable message. The key was parsed as host-side dotenv data and used
only in the discovery Authorization header; it was not printed, passed in argv,
stored in artifacts or sent to a model. The exact original receipt is retained;
no paid replay or regional bypass was attempted. Supported provider access is a
reported operation-scoped owner dependency, while local corpus preparation,
reliability and other authorized work continue. Account/key/model availability,
the frozen 20-question A/B/C screen and unchanged-4B supported-answer/citation
metrics remain pending. No quality improvement is claimed.

Normal memory search/context defaults remain lexical. Portable C and the 4B
profile are unchanged. No live history, memory format or production configuration
was changed; daemon/gateway remain inactive/disabled and PostgreSQL is exited
with restart disabled. The separate core-only lane, MCU, storage power loss,
Windows runtime, paid inference and direct research-command process-death recovery
were not exercised by this qualification. The command reuses the store's
separately tested atomic publication and refuses an existing output.

The preceding [compaction receipt](verification/memory-promotion-compaction-20261120.json)
pins `f38cb31` and its real 4,096-marker daemon batches, original/leaf expiry pins
and process-crash cases. The [archive-expiry receipt](verification/memory-archive-expiry-20261120.json)
pins `6c40ad9`; its initial blank-after-reload browser failure, focused diagnostics
and separate passing rerun remain historical evidence. The current clean pass
does not establish a fix for that intermittent issue. Failure tracing stays
enabled in the canonical lane.

Prior [derived-memory](verification/memory-derived-state-20261120.json) and
[retirement](verification/memory-retirement-20261120.json) receipts pin `f17968f`
and `a419c20`; their fault cases remain in the current lane.
Prior [promotion](verification/memory-promotion-20261120.json),
[retrieval](verification/memory-views-20261120.json) and
[publication](verification/memory-authority-20261120.json) receipts pin `9ec12c7`,
`90b05f8` and `f355886`; their failure cases remain in the current canonical lane.
The [remote-workflow](verification/remote-workflow-20261120.json),
[watch-state](verification/run-watch-identity-20261120.json) and earlier
[consolidation](verification/consolidation-20261120.json) receipts identify their
own qualified revisions. The separate core-only lane and repeated session-store
qualification in the consolidation receipt were not repeated here; the core test
remains in the host lane. Older artifact hashes never qualify new binaries.
Earlier narratives are retained in
[the qualification archive](reports/qualification-through-20261118.md).

## Boundaries and next work

Durable promotion provenance now prevents old `@mem` observations from undoing
later corrections and invalidates marker-derived heads when support changes.
Revision-checked retirement now preserves originals and excludes removed support
from current retrieval. Registered cache purge, live correlation and recap input validation cover those consumers. Explicit memory-root archive expiry preserves active reference pins. Promotion compaction now preserves consumed IDs, full original decisions and marker revision pins while allowing further bounded admission. The next authority gates are broader bounded forgetting, expiry-intent metadata compaction, generic conclusion/claim validation and direct recap crash admission.
Invalidation must never replay a completed external effect. Declared currentness
and exact references do not prove arbitrary prose or generated claims true.
The bounded lexical multi-hop foundation now exists for foreground research;
quality promotion still requires the independent screen and unchanged 4B answerer. The
[architecture evaluation gate](ARCHITECTURE_CURRENT.md#memory-authority-and-retrieval-evaluation)
records the canary/pilot caps and checked public endpoint/auth/typed-response and
listed price. The discovery-only canary encountered HTTP 451 before scoring.
Supported access, account/model verification and measured retrieval/answer quality
remain pending; the owner dependency does not block independent local work.


This is a coherent implemented slice, not a claim that every item in the architecture is shipped. Current workflows retain single-daemon, at-least-once task recovery. The new effect protocol protects deterministic HTTP POST tasks under its destination/receipt contracts. Submit dedupe and SOP hashes alone still do not prevent duplicate external writes. Device assignment publication is atomic. Ordinary workflow host-model transcripts, session fences, image preparation and sequential delegated children have bounded recovery contracts. Uninstrumented preparation/tool paths, trusted receipt resolvers, additional device adapters, physical execution guarantees and strict provider/adapter resource metering remain correctness work. Workflow reservation/settlement is now atomic, with observed usage and conservative uncertainty kept distinct.

The work ledger remains the host-side writable authority, separate from the main daemon DB. Broker-bound goals read it through an authenticated read-only endpoint, expose current observations in the UI and use managed polling to continue after verified changes. The broker's separate immutable dispatch journal records admission and original team-run correlation, not requirements or host completion. Bounded advisory server notifications and polling fallback are implemented. Durable replayable notification streams, multi-authority coordination, an append-only work audit and general mutation APIs remain open. `agent work` updates the existing authority; evidence text still needs a trustworthy verifier.

Voice calls are ephemeral and have no workspace execution tools or durable work-ledger integration. Qualification includes synthetic ASR, image understanding and streamed reply PCM through the gateway; physical device acoustics and background behavior remain separate checks.

General signal inboxes, broadcast and publisher-specific identity, person/role-bound approvals, active-run version migration, general branch transitions, dynamic loops, compensation, complete native provider adapters and full legacy CLI decomposition remain planned. Explicit operator policy still controls configured approvals and access. No live-provider compatibility, ESP32 hardware performance, PLC qualification, distributed exactly-once execution or full-system production certification was established by the local tests.
