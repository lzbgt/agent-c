# Agent-C handoff

Updated 2026-11-11 in the existing Arch checkout, `/home/bruce/work/agent-c`, on `master`.

## Current source and runtime

The owner resumed project work on Arch through Local Work Bridge after the earlier stop/handoff at `8124798d43f2fe599c70819a9b22dcdfba8a567b`. The current source adds the signed UM-BMP device receipt recovery slice below. This is a rolling-project milestone, not completion of the overall roadmap.

**No deployment was performed.** The public runtime at https://agent.hubstack.cn remains implementation revision `a0890c07a9dc53f1993ce726dea56aa04f156248`, release `a0890c07a9dc-79684198d197`. Public health was checked again during this work. A future release needs applicable deployment authorization and the maintained deployment pipeline. GitHub is the publication destination; Panda publication remains on hold.

The existing native session was left paused to avoid duplicate work. No new clone, worktree or editable shadow source was created. Preserve the unrelated pre-existing untracked `.config/`, `AGENTS.md` and `PANDA_PUSH.md`; they must not be staged. Do not change `project.md`.

## Delivered receipt recovery

- Schema v36 adds immutable `edge_task_receipts`, bound to the original task, step, node, idempotency key and command payload.
- A matching, cryptographically authenticated `TASK_DONE` can retain its validated original result after a platform timeout/cancellation. Unsigned messages, wrong identities, invalid result schemas, invalid/incomplete supplied attestations and conflicting receipts do not become recovery evidence.
- The receipt is separate from platform status. Its persistence does not clear a timeout/cancellation or send another device command. A saved receipt survives abrupt process exit.
- The existing authenticated workflow reconciliation endpoint resolves only a recorded original device receipt. It accepts no model-supplied result or arbitrary device resolver URL. Transactional requeue preserves sticky owner cancellation, workflow deadlines, budgets and completed independent work.
- The scheduler consumes the receipt without another dispatch, records the original platform state and releases dependent work. The responsive **Check device receipt** control prevents duplicate active requests and reports missing evidence.
- Device trust and physical-execution limits remain explicit. This adapter does not certify physical readback quality, board power-loss behavior, stopping or exactly-once action.

Contracts: [device assignments and receipts](docs/DEVICE_ASSIGNMENTS.md), [HTTP effects](docs/WORKFLOW_EFFECTS.md), [workflow budgets](docs/WORKFLOW_BUDGETS.md), [current snapshot](docs/CURRENT_SNAPSHOT.md).

## Verified source

On the final source, the canonical verifier passed **23 native/CLI/API checks**, all Go broker/gateway tests, generated OpenAPI checks, the production UI build and **54 browser checks**. Twelve additional edge authentication, attestation, CBOR transport, result-schema, message-replay, deadline and workflow regressions passed. Repository guards and whitespace checks passed.

Evidence remains in ignored build outputs:

- `build/device_receipt_snapshot_verification.log`
- `build/device_receipt_extended_regressions.log`
- `build/device_receipt_repo_guards.log`

The receipt tests exercise real signed ingress, immutable duplicate/conflict handling, result-schema and forged-attestation rejection, an abruptly exiting writer, restart, requeue rollback, cancellation/deadline rejection and one-worker dependent-work recovery with one original outbox command. Browser coverage includes 320-pixel layout, repeated clicks, absent receipts and cancellation visibility. No production data or real device was used.

Reproduce:

```sh
AGENT_E2E_BROWSER_EXECUTABLE=/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome \
AGENT_E2E_UI_PORT=4187 tools/verify_current_snapshot.sh
cmake --build build-snapshot --target agent_ed25519_tool agent_core_umbmp_cbor_encode -j 4
tools/verify_repo_guards.sh
```

Use assertion-enabled `build-snapshot/` for native verification. Keep the production Release `build/` configuration separate.

## Publication CI and broader qualification

The receipt feature was published to GitHub as `ac37208a00d3aceb711098eeaaf93b86d3d4b770`. Its first hosted CI run exposed additional build/CI problems. This follow-up preserves all checks while repairing remote-ref/commit baseline resolution in the vendored guard, a packet-metadata header's accidental dependency on optional SRTP, the retired macOS runner and pinned Windows compiler, and the local-only proxy default leaking into container CI. Explicitly empty proxy values now disable the local default; leaving them unset preserves local behavior.

The complete assertion-enabled native target build passed. Ten isolated vendored-guard regressions, four focused native device/DB/media tests, all three Compose proxy-configuration cases, YAML parsing and repository guards passed. Packet accounting also compiled and ran without OpenSSL/SRTP linkage.

**The wider suite is not green.** The offline 334-test run had 298 passes, 27 intentionally skipped network tests and nine failures. Managed consensus recovery passed on its bounded retry; eight failures remain: trace workflow assignment returns 409, autonomous consensus does not always report a vote grant, four certificate tests require a missing local CA fixture, and two WebRTC smoke tests cannot complete with the available browser/tool setup. The audio peer log specifically reports a missing Playwright headless-shell executable. Do not present the focused feature pass as a full-suite or CI pass.

The failed-test retry was stopped after CTest's filtered numeric indices selected a Compose build. It did not start new containers; existing container uptime and public health remained unchanged. For further retries use anchored test names and keep the same exclusions, rather than reusing numeric selections across differently filtered test lists.

The next hosted run at `cd64854bc83c56e3859d1287b907b4cf1e331586` passed the repository guard and self-contained eval pack, and advanced past the original runner/proxy failures. It then exposed a missing `<algorithm>` include on Linux, OpenSSL's const subject-name API on macOS, CMake package discovery without pkg-config on Windows, and missing OpenSSL/vendored-module inputs in the container images. The current follow-up fixes those exact build errors without reducing coverage. A full Arch native build, six DTLS/memory/receipt regressions, ten guard tests and two dependency-discovery regressions passed. The package-discovery check uses the installed JsonCpp library through a native-target test fixture because Arch's package does not ship its CMake config. These are local results, not a claim that hosted CI is green.

Additional evidence is in `build/ci_portability_full_build.log`, `build/ci_portability_full_ctest.log`, `build/ci_portability_ctest_rerun_failed.log`, `build/ci_portability_voice_retry.log` and `build/ci_portability_guards.log`. The latest compiler checks are in `build/ci_compiler_portability_build.log`, `build/ci_package_build.log` and `build/ci_compiler_portability_guards.log`. Check hosted CI against the exact new remote revision after publication. The immediate qualification follow-up is to repair the remaining test fixtures/contracts and verify remote Linux, macOS, Windows and container checks; none of this authorizes deployment.

## Next product work

1. Extend durable effect identity, intent, reconciliation and scoped uncertainty to arbitrary host tools; add adapters beyond signed UM-BMP original-result receipts.
2. Close strict provider/adapter resource-metering gaps. Recorded overages and conservative crash charges must remain honest.
3. Connect goal/requirement/blocker authority to the broker and event-driven wakeups while preserving explicit stops.
4. Continue immutable SOP registry, signals/timers/decisions/compensation and subject-oriented SOP authoring.
5. Continue embedded instance-scoped pollable ports, provider/checkpoint contracts and CLI decomposition, plus durable conversation identity/pagination.

Keep every unfinished acceptance criterion in `TODOS.md`; do not narrow the project to this slice.

## Operational constraints

Execution, UI and the dedicated database belong on Arch. The VPS provides shared Hubstack OAuth and reverse-proxy ingress. Preserve unrelated deployments, containers, credentials and data. Shared identity/admin pins and server-held provider secrets must not be printed or committed. Voice remains Aliyun Omni with its existing server-side credential locator; no live media or paid inference was needed here.

Read `PANDA_PUSH.md`, preserve Agent-C's Panda exclusion and never recreate that repository. Publish ordinary commits only to the existing GitHub `origin/master`, without force-pushing. For an authorized future deployment, use the existing installer/verifier documented in [Arch deployment](docs/ARCH_HOST_DEPLOYMENT.md), check active work before a daemon restart, and independently verify the public revision/artifact hashes/authentication afterward.
