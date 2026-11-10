# Current Agent-C snapshot

Date: 2026-11-10. This document records implemented behavior and reproducible checks. It is the entry point for the current snapshot; [architecture and delivery gates](ARCHITECTURE_CURRENT.md) describe the longer-term design and [research](research/agent-systems-20261110.md) records inspected primary sources.

## Implemented in this snapshot

- **Autonomy:** default host instructions execute authorized work without permission prompts. The shared SQLite requirement/blocker ledger keeps independent work runnable, validates dependency DAGs, requires evidence for completion/resolution, and rejects stale revisions. A human dependency affects only its named requirements; advisory blockers can affect none.
- **Completion:** a portable C completion hook allows hosts to continue a proposed final response within the original budgets. The host uses it after a run adopts `work_state`; an unchanged-ledger repetition guard prevents endless false completion. Empty/unrelated ledgers are not automatically adopted.
- **SOPs:** `agentd.sop.v1` compiles ordered stages and sequential/parallel actions through the existing workflow submit API. A canonical definition hash and stage mapping are stored with the redacted spec. Strict compiler validation precedes ordinary task/policy/admission validation. Oversized persisted specs are rejected instead of truncated.
- **CLI:** command dispatch is separated from provider execution. `exec`, bounded stdin, `resume`, `sessions`, explicit working/session directories and model-free `work` commands reuse existing execution/persistence. Existing JSONL transcript export stays available. Resumed host prompts refresh when the built-in contract changes.
- **WebUI:** responsive graphite/mint workspace, grouped icon navigation, Overview return path, actual connection/session context, accessible template selection, useful empty state, keyboard focus and reduced-motion support. Advanced panels are lazy-loaded. A provider-free SOP authoring template is included.
- **Reproducibility:** npm lockfile tarballs use the canonical registry with existing integrity hashes and versions preserved. OpenAPI client types are regenerated. Local deterministic tests cover the new behavior without provider credentials.

Usage and exact contracts: [CLI](CLI.md), [work state](AUTONOMOUS_WORK.md), [SOP workflows](SOP_WORKFLOWS.md), [embedded C](EMBEDDED_C_API.md), [WebUI](WEBUI.md).

## Reproduce

Install the platform's normal CMake/C/C++/curl/OpenSSL/jsoncpp/SQLite/pkg-config dependencies, Python 3, and Node/npm as described in [OPERATIONS.md](OPERATIONS.md). The UI uses its committed npm lockfile. Install a Playwright Chromium browser if one is not already available:

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

The snapshot verification script passed 12 targeted native/CLI/API checks and six browser checks on Linux. The separate core-only Debug build/test passed. Coverage includes:

- C tool loop, continuation and unchanged step caps; host tool and pinned-prompt regressions.
- Requirement DAG validation, scoped blockers, evidence gates, rollback, stale revision rejection and concurrent independent CLI writers.
- Local model-stub exec/stdin/resume and project instruction discovery; premature completion continues until recorded work is verified; repeated no-progress completion fails with a limit.
- Real local daemon SOP submit, persisted definition/mapping, stage barriers, idempotency and invalid-input rejection.
- Existing local streaming, workflow idempotency and replay-spec regression paths.
- Desktop/mobile navigation, prompt focus, SOP template submission, workflow details, prompt uploads and scene history.
- TypeScript/Vite production build, generated OpenAPI drift check, repository guards and whitespace checks.

Local logs: `build/snapshot_verification.log`, `build/snapshot_core_only.log`, and `build/snapshot_repo_guards.log`. Browser tests regenerate `ui/test-results/workspace-desktop.png` and `workspace-mobile.png`; generated artifacts are ignored, while their source fixtures and commands are tracked. On the checked host, advanced-panel splitting reduced the main application chunk from approximately 1,087 kB to 798 kB before gzip; this is a build observation, not a latency benchmark.

## Boundaries and next work

This is a coherent implemented slice, not a claim that every item in the architecture is shipped. Current workflows retain single-daemon, at-least-once recovery. Submit dedupe and SOP hashes do not prevent duplicate external writes. Effect intent/receipt/reconciliation is the next production correctness priority.

The work ledger is host-side current state, separate from the main daemon DB. It does not yet implement broker-authoritative goals, an append-only work audit, automatic wakeup after a human edit, a dedicated REST API or a blocker UI. `agent work` can update it and `resume`/a subsequent run can adopt it. Evidence text still needs a trustworthy verifier.

SOP registry immutability, durable process decisions/signals, branches, loops, compensation, complete native provider adapters and full legacy CLI decomposition remain planned. Explicit operator policy still controls configured approvals and access. No live-provider compatibility, ESP32 hardware performance, PLC qualification, distributed exactly-once execution or full-system production certification was established by the local tests.
