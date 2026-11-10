# Current Agent-C snapshot

Date: 2026-11-10. This document records implemented behavior and reproducible checks. It is the entry point for the current snapshot; [architecture and delivery gates](ARCHITECTURE_CURRENT.md) describe the longer-term design and [research](research/agent-systems-20261110.md) records inspected primary sources.

## Implemented in this snapshot

- **Autonomy:** default host instructions execute authorized work without permission prompts. The shared SQLite requirement/blocker ledger keeps independent work runnable, validates dependency DAGs, requires evidence for completion/resolution, and rejects stale revisions. A human dependency affects only its named requirements; advisory blockers can affect none.
- **Completion:** a portable C completion hook allows hosts to continue a proposed final response within the original budgets. The host uses it after a run adopts `work_state`; an unchanged-ledger repetition guard prevents endless false completion. Empty/unrelated ledgers are not automatically adopted.
- **SOPs:** `agentd.sop.v1` compiles ordered stages and sequential/parallel actions through the existing workflow submit API. A canonical definition hash and stage mapping are stored with the redacted spec. Strict compiler validation precedes ordinary task/policy/admission validation. Oversized persisted specs are rejected instead of truncated.
- **CLI:** command dispatch and output policy are separated from provider execution. `exec`, bounded stdin, `resume`, `sessions`, explicit working/session directories and model-free `work` commands reuse existing execution/persistence. `--json` emits versioned lifecycle/progress events; `-o` publishes final text after successful execution/session saving. Existing JSONL transcript export stays available. Resumed host prompts refresh when the built-in contract changes. See [CLI_EVENT_PROTOCOL.md](CLI_EVENT_PROTOCOL.md).
- **Streaming:** coalesced SSE packets drain through bounded parser batches; transport packet boundaries no longer truncate the checked streams. The host event log enforces its 4,000-event/2-MiB capture bounds while live callbacks continue after capture fills.
- **WebUI:** a lean responsive chat presents user messages and assistant outputs once, with a compact Attach/Send composer. System context, tools, traces and other metadata are grouped under closed Activity & details. Settings leads with model, capabilities and conversation; technical controls and management are closed by default. Real files, notes and visual results stay visible. ChatThread is the sole conversation renderer, including when details are expanded; obsolete copied context, per-run replies and the second team transcript are removed. Memory, voice calls, traces, run comparison, decisions and workflow lists lead with content/actions while tuning and evidence stay closed. Developer setup notes are removed from default pages. Tool pages remain primary, workflow runs/creation/schedules are separate, and authorized browser effects run independently of collapsed presentation. See [UI_DESIGN.md](UI_DESIGN.md).
- **Voice:** Aliyun Omni 3.5 Flash/Ethan streams mono 16 kHz microphone PCM, 24 kHz reply PCM and bounded JPEG images through the authenticated gateway. One transcript renderer avoids duplicate text/audio captions. Device capture and clocks stop on call end/navigation, and credentials stay server-side. [Voice contract and qualification](VOICE.md).
- **External HTTP effects:** guarded POST actions persist intent and response identity; uncertain writes reconcile through a same-origin receipt adapter or emit a scoped blocker. An explicit destination-idempotency contract permits safe retries. Recovery is inspectable and resumable through the API/WebUI, and independent workflow branches continue after a failure even with one worker. See [WORKFLOW_EFFECTS.md](WORKFLOW_EFFECTS.md).
- **Device actions:** assignment identity, active resource/rate admission, outbox publication and queued evidence commit together. Retries reuse the original node/operation; uncertain terminal device outcomes expose a required action without blocking independent scheduler branches. See [DEVICE_ASSIGNMENTS.md](DEVICE_ASSIGNMENTS.md).
- **Workflow budgets:** durable attempt grants serialize capacity admission across workers; task/usage settlement releases grants atomically, while crash-lost grants remain conservatively charged. Capacity waits preserve attempts and independent work. One-shot model steps are metered. Token/time overages remain explicit; see [WORKFLOW_BUDGETS.md](WORKFLOW_BUDGETS.md).
- **Authenticated deployment:** an Arch gateway serves the modern UI and proxies private daemon execution with server-held credentials. It uses Panda's shared Hubstack OAuth with a separate application, immutable verified provider-proof pins and optional Agent-C product account pins, browser-bound one-use attempts, central revision checks and explicit member access state. A canonical schema fingerprint requires fresh deployment on incompatible change. See [ARCH_HOST_DEPLOYMENT.md](ARCH_HOST_DEPLOYMENT.md) for installation and rollout qualification.
- **Reproducibility:** npm lockfile tarballs use the canonical registry with existing integrity hashes and versions preserved. OpenAPI client types are regenerated. Local deterministic tests cover the new behavior without provider credentials.

Usage and exact contracts: [CLI](CLI.md), [work state](AUTONOMOUS_WORK.md), [SOP workflows](SOP_WORKFLOWS.md), [embedded C](EMBEDDED_C_API.md), [WebUI](WEBUI.md).

## Live deployment

The verified snapshot is deployed at **https://agent.hubstack.cn**. Execution, UI and its dedicated PostgreSQL run on Arch; the existing VPS provides only Agent-C ingress and the shared identity service. The verified owner completed Apple SSO and reached the connected workspace. Public authenticated qualification ran a four-action SOP and a bounded `deepseek-v4-pro` request. The public authenticated voice WebSocket also passed a synthetic ASR/image/PCM round trip with the expected image word. No existing Agent-C VPS runtime was found to remove; other deployments were preserved. See [deployment operations and qualification](ARCH_HOST_DEPLOYMENT.md).

## Publishing destination

The owner's latest instruction authorizes publishing this verified snapshot to **GitHub** at [lzbgt/agent-c](https://github.com/lzbgt/agent-c). Panda publishing remains on hold while that service is being fixed. Existing remotes and the host's exclusion/history policies are preserved; no Panda destination is recreated. See [PUBLISHING.md](PUBLISHING.md).

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

The snapshot verification script passed 23 targeted native/CLI/API checks, Go broker/gateway tests and 54 browser checks on Linux. The separate core-only Debug build/test passed. Gateway race tests and actual dedicated PostgreSQL persistence/schema checks also passed. Coverage includes:

- C tool loop, continuation and unchanged step caps; host tool and pinned-prompt regressions.
- Requirement DAG validation, scoped blockers, evidence gates, rollback, stale revision rejection and concurrent independent CLI writers.
- Local model-stub exec/stdin/resume and project instruction discovery; premature completion continues until recorded work is verified; repeated no-progress completion fails with a limit.
- Versioned CLI lifecycle/stream events, escaped Unicode, tool/no-tool/multimodal paths, failure-safe final artifacts and complete 608,000-character streams beyond bounded event capture.
- Real local daemon SOP submit, persisted definition/mapping, stage barriers, idempotency and invalid-input rejection.
- HTTP effect crash boundaries, receipt reuse, destination deduplication, terminal reconciliation proofs, stale finalizers and preservation of owner stops; API/WebUI recovery of one affected branch.
- Device assignment rollback, concurrent capacity admission, immutable operation identity, stable node recovery, killed-writer atomicity and one-worker scoped blocker progress. Required human actions remain visible without full execution payloads.
- Concurrent workflow grants, capacity yields, rollback/refund, stale settlement, killed-holder recovery and real parallel model/tool/receipt-stub admission; observed overages remain visible.
- Existing local streaming, workflow idempotency and replay-spec regression paths.
- Greeting messages without metadata clutter; repeated identical turns, streaming handoff, accepted draft clearing, IME input, collapsed settings, authorized hidden browser effects and advanced broker controls.
- Presentation clocks and input stay in their own browsing context; typing, unrelated renders and equal-value polling preserve the current animation. Keyboard focus remains local to each presentation/composer, meaningful input changes replace the runtime, and detaching a presentation stops its clocks even without returned cleanup.
- Desktop and 320/390/768px navigation, populated workflow layout, primary tool visibility, draft preservation, modal focus/Escape, reduced-height composer access, SOP submission, workflow details, graph editing, schedules, prompt uploads and scene history.
- Shared identity browser binding/replay refusal, central revocation/outage/instance checks, administrator/member separation, proxy secret isolation and desktop/mobile sign-in.
- Voice microphone/camera lifecycle, late device permissions, image conversion/refresh, one-caption replies and provider failure; gateway input bounds, pinned credentials, capacity, revocation and shutdown.
- TypeScript/Vite production build, generated OpenAPI drift check, repository guards and whitespace checks.

Local logs: `build/workflow_budget_snapshot_verification.log`, `build/cli_snapshot_core_only.log`, `build/arch_broker_all_tests.log`, `build/arch_gateway_postgres_tests.log` and `build/arch_repo_guards.log`. Browser tests regenerate workspace and deployment sign-in screenshots under `ui/test-results/`; generated artifacts are ignored, while their source fixtures and commands are tracked. Live voice evidence is in `build/aliyun_voice_roundtrip_public.log`; `build/omni_voice_live_ui_checks.log` covers four public viewport checks without starting device capture. On the checked host, the combined UI refactor reduced the main application chunk from approximately 1,087 kB to 752 kB before gzip; this is a build observation, not a latency benchmark.

## Boundaries and next work

This is a coherent implemented slice, not a claim that every item in the architecture is shipped. Current workflows retain single-daemon, at-least-once task recovery. The new effect protocol protects deterministic HTTP POST tasks under its destination/receipt contracts. Submit dedupe and SOP hashes alone still do not prevent duplicate external writes. Device assignment publication is now atomic, but arbitrary host-tool effects, device-specific receipt resolvers, physical execution guarantees and strict provider/adapter resource metering remain production correctness work. Workflow reservation/settlement is now atomic, with observed usage and conservative uncertainty kept distinct.

The work ledger is host-side current state, separate from the main daemon DB. It does not yet implement broker-authoritative goals, an append-only work audit, automatic wakeup after a human edit, a general requirement/blocker REST API or UI. The HTTP effect slice has its own reconciliation API/UI. `agent work` can update it and `resume`/a subsequent run can adopt it. Evidence text still needs a trustworthy verifier.

Voice calls are ephemeral and have no workspace execution tools or durable work-ledger integration. Qualification includes synthetic ASR, image understanding and streamed reply PCM through the gateway; physical device acoustics and background behavior remain separate checks.

SOP registry immutability, durable process decisions/signals, branches, loops, compensation, complete native provider adapters and full legacy CLI decomposition remain planned. Explicit operator policy still controls configured approvals and access. No live-provider compatibility, ESP32 hardware performance, PLC qualification, distributed exactly-once execution or full-system production certification was established by the local tests.
