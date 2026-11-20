# Current Agent-C snapshot

Date: 2026-11-20. This document records implemented behavior and reproducible checks. It is the entry point for the current snapshot; [architecture and delivery gates](ARCHITECTURE_CURRENT.md) describe the longer-term design and [research](research/agent-systems-20261110.md) records inspected primary sources.

This rolling summary covers current source. Dated deployment and earlier test
results remain explicitly historical; the source tree does not imply that
production services were restarted.

## Implemented in this snapshot

- **Requirement-driven broker goals:** explicit agent/deployment/session binding keeps the host ledger authoritative. Success requires evidence-bearing completed requirements, independent work continues past scoped blockers, changed revisions wake managed waits and unchanged progress stops. Single-use admission and exact control CAS prevent duplicate/stale launches. Immutable broker receipts recover the original persisted team run after response loss or restart, never by resending it. Unrecorded host outcomes remain pending, mismatches stop recovery and owner controls are preserved. UI setup, progress and receipt states are included. Managed requirement waits now use bounded server-notified long polling with ordinary reconciliation fallback, coalesced hints, explicit watch caps and cancellation. Every hint triggers fresh goal/ledger checks, not dispatch permission. This remains single-authority coordination, not a durable event stream or reconstruction of an unknown host outcome. See [goal ledger contract](GOAL_WORK_LEDGER.md).

- **Model task continuity:** ordinary workflow/SOP host-model tasks have a durable effect guard. Ephemeral and session-backed tasks save exact bounded image-preparation/provider/tool transcripts and resume verified progress after provider failure or restart without repeating or recharging cached host calls. Current policy, fresh-call grants and owner stops still apply; pending/unusable receipts remain blocked. Saved-progress UI differs from unknown effects and adds no manual resume control. Persistent sessions pin a durable conversation generation and atomically publish final messages with the model receipt. Changed conversations stop without overwriting newer edits; the UI explains the conflict. The existing image-description prefetch is restored exactly instead of reinterpreting images on retry; fresh analysis shares provider-step/token accounting and cached analysis is free. Sequential delegated host-model children use distinct parent-owned journals, shared-session fences and free completed-prefix replay. Verified partial children can traverse cached records with exhausted tool/step grants while every new call remains grant-gated; elapsed/token admission and conservative lost charges are unchanged. Other uninstrumented preparation and parallel macro semantics remain excluded. See [delegated continuation](DELEGATED_CONTINUATION_DESIGN.md). See [model effect contract](WORKFLOW_EFFECTS.md#model-selected-host-tools).
- **Deterministic host actions:** `host_tool` workflows and SOP actions reuse daemon policy/explicit approvals, persist original intent/results, reject changed identities and never blindly repeat an interrupted effect. Saved results need no new tool charge or execution approval. Independent work continues; the UI explains unresolved host outcomes without offering an unsupported HTTP resolver. See [host effect contract](WORKFLOW_EFFECTS.md#deterministic-host-tools).

- **Autonomy:** default host instructions execute authorized work without permission prompts. The shared SQLite requirement/blocker ledger keeps independent work runnable, validates dependency DAGs, requires evidence for completion/resolution, and rejects stale revisions. A human dependency affects only its named requirements; advisory blockers can affect none.
- **Completion:** a portable C completion hook allows hosts to continue a proposed final response within the original budgets. The host uses it after a run adopts `work_state`; an unchanged-ledger repetition guard prevents endless false completion. Empty/unrelated ledgers are not automatically adopted.
- **Conditional process actions (source only):** individual workflow/SOP actions may compare a Boolean/text/null result from a successful earlier action. Strict validation and repeat-aware dependency mapping reject unsafe references. Non-matches persist explicit skipped receipts before templates, waits, budgets or effects; missing/wrong-typed results fail closed. Guided authoring preserves advanced settings and progress separates executed/skipped work. This is not a general branch engine or an approval. See [condition contract](SOP_WORKFLOWS.md#conditional-actions).
- **External process signals (source only):** single-use waits accept authenticated JSON objects up to 16 KiB. Explicit early-delivery opt-in opens one exact action slot atomically with submission; buffered input never advances dependencies, conditions or readiness. Expiry starts only at activation. Atomic wait/yield and immutable receipt/wakeup preserve independent work, replay, restart and owner cancellation. Future SOP iterations have separate slots; skipped/failed/stopped actions expose unused input honestly. Guided authoring and collapsed read-only metadata are included. No input before submission, general inbox, broadcast or per-publisher authorization is implied. See [signal contract](WORKFLOW_SIGNALS.md).
- **Durable process decisions:** explicit configured continue/fail choices, worker-free waits without consumed attempts or budget grants, immutable answer/wakeup transactions and idempotent receipts. Expiry and owner stops remain authoritative. The review template and run cards expose only configured choices; lost responses retain the same delivery. This is not person-level role approval. See [decision contract](WORKFLOW_DECISIONS.md).
- **Bounded conversation snapshots (source only):** authenticated, generation-fenced SQL pages cap 200 rows and 2 MiB raw text/media without truncation. The optional history inspector fetches 50 messages lazily, keeps one page, and discards a changed generation instead of mixing results. Other chat/audit consumers are separate; this is not append-only history. See [session pages](SESSION_MESSAGE_PAGES.md).
- **Chat submission recovery (source only):** a scoped secret-free receipt precedes POST. Lost asynchronous replies and reloads trigger at most three bounded authenticated GETs to find the original job, never another execution POST. Its watch is saved before clearing the guard; malformed, missing or ambiguous receipts remain held. Session/auth changes fence late responses, and keyed browser state no longer carries queues into another scope. Native insert-only admission, restart uncertainty and durable cancellation are checked in the same verification lane. See [chat recovery](CHAT_SUBMISSION_FENCE.md).
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

For a supported installed Chromium, set `AGENT_E2E_BROWSER_EXECUTABLE` to its executable path. `AGENT_E2E_UI_PORT` chooses an unused test port. The snapshot verifier disables trace/video recording by default; set `AGENT_E2E_RECORD=1` to enable them. It uses local model stubs and mocked browser APIs, not live provider keys or a production deployment.

For the embedded-safe host build/test lane:

```sh
tools/verify_current_snapshot.sh --core-only
```

The verifier configures **Debug** builds so assertion-based existing C tests execute. Do not interpret passing Release tests compiled with `NDEBUG` as equivalent coverage. Build artifacts live under ignored `build-snapshot/` and `build-snapshot-core/`; override `AGENT_SNAPSHOT_BUILD_DIR` if needed.

## Verification evidence

The canonical verifier now includes CLI session-store preservation, native
session paging and durable job admission/lookup/cancellation, alongside browser
context paging, chat/workflow submission recovery and job-watcher regressions.
This removes the gap between supported source contracts and the default checks.
On 2026-11-20, the complete expanded verifier passed **49 native/CLI/API
checks**, all Go broker/gateway packages, generated OpenAPI/type checks, the
production UI build and **130 browser checks**. The separate core-only Debug
lane passed. Repository guards, local documentation links/anchors and whitespace
checks also passed. No production service or real provider was exercised.

Recovery checks cover lost acceptance, delayed/missing/ambiguous metadata,
response size and state validation, StrictMode reload, storage failure,
cross-session/auth late responses and accepted-watch preservation with a full
future-timestamp cache. Keyed-storage tests verify selected-scope loading and
rejection of stale setters. The promoted native fixtures exercise actual daemon
admission, restart uncertainty, trace lookup and durable cancellation.

The session-store fixture now claims an exclusive temporary directory instead
of reusing deterministic random names; five repeated runs passed. Connection
tests explicitly open the new connection's own workflow view, and the keyboard
fixture waits for an enabled composer before testing IME-safe submission. These
changes preserve the actual isolation and input assertions.

Local receipts: `build/consolidated_snapshot_complete_20261120.log`,
`build/consolidated_core_20261120.log`,
`build/consolidated_repo_guards_20261120.log` and
`build/consolidated_session_store_repeat.log`. The
[source qualification receipt](verification/consolidation-20261120.json)
pins source `a575cf4` and the checked native/UI artifact identities. Its later
documentation-only receipt commit does not change those runtime sources.

Earlier milestone counts and fault-injection narratives are retained in
[the dated qualification archive](reports/qualification-through-20261118.md).
They are not substitutes for checking the current revision. Linux host/browser
checks do not establish a hosted CI matrix, MCU boot or live-provider behavior.

## Boundaries and next work

This is a coherent implemented slice, not a claim that every item in the architecture is shipped. Current workflows retain single-daemon, at-least-once task recovery. The new effect protocol protects deterministic HTTP POST tasks under its destination/receipt contracts. Submit dedupe and SOP hashes alone still do not prevent duplicate external writes. Device assignment publication is atomic. Ordinary workflow host-model transcripts, session fences, image preparation and sequential delegated children have bounded recovery contracts. Uninstrumented preparation/tool paths, trusted receipt resolvers, additional device adapters, physical execution guarantees and strict provider/adapter resource metering remain correctness work. Workflow reservation/settlement is now atomic, with observed usage and conservative uncertainty kept distinct.

The work ledger remains the host-side writable authority, separate from the main daemon DB. Broker-bound goals read it through an authenticated read-only endpoint, expose current observations in the UI and use managed polling to continue after verified changes. The broker's separate immutable dispatch journal records admission and original team-run correlation, not requirements or host completion. Bounded advisory server notifications and polling fallback are implemented. Durable replayable notification streams, multi-authority coordination, an append-only work audit and general mutation APIs remain open. `agent work` updates the existing authority; evidence text still needs a trustworthy verifier.

Voice calls are ephemeral and have no workspace execution tools or durable work-ledger integration. Qualification includes synthetic ASR, image understanding and streamed reply PCM through the gateway; physical device acoustics and background behavior remain separate checks.

General signal inboxes, broadcast and publisher-specific identity, person/role-bound approvals, active-run version migration, general branch transitions, dynamic loops, compensation, complete native provider adapters and full legacy CLI decomposition remain planned. Explicit operator policy still controls configured approvals and access. No live-provider compatibility, ESP32 hardware performance, PLC qualification, distributed exactly-once execution or full-system production certification was established by the local tests.
