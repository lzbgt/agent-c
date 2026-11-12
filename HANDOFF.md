# Agent-C handoff

Updated 2026-11-12 in the existing Arch checkout on `master`.

## Delivered now: guarded deterministic host actions

Workflow and SOP actions can invoke a registered host tool with `kind: host_tool`, without a model. Durable intent and original-result capture prevent a scheduler restart from blindly repeating a host-side effect. Saved results release dependents without another tool invocation; ambiguous outcomes expose a scoped blocker while independent work continues.

The action uses existing daemon host/sandbox policy, allow/deny lists and explicit approvals. Policy checks precede dispatch admission; cancellation during approval causes no effect. Saved results do not require another execution approval. Request identity pins tool arguments, execution root and session context. Caller-declared host idempotency and invented reconciliation URLs are rejected.

Known nonzero exits are saved failures, not automatic retries. Timeouts, lost results and uncertain persistence remain unresolved. This is a useful deterministic workflow boundary, not a universal journal around LLM-selected tools or proof of business/physical exactly-once execution. Generic trusted host receipt adapters and strict non-cooperative metering remain outstanding.

Contracts and usage: [durable effects](docs/WORKFLOW_EFFECTS.md), [current snapshot](docs/CURRENT_SNAPSHOT.md), [roadmap](TODOS.md).

## Verification

Focused checks passed on Arch: native host-tool identity/policy/recovery tests, existing effect and SOP tests, and both real process-crash smoke tests. The host smoke proves one original write across crash-before-dispatch, crash-after-effect and saved-receipt-before-task-completion boundaries; timeout/nonzero exits, budgets, owner stops and independent branches are covered. The UI production build, regenerated OpenAPI types and responsive workflow-detail browser test passed.

The final canonical verifier passed **25 native/CLI/API checks**, all Go broker/gateway tests, generated OpenAPI checks, the UI production build and **54 browser checks**. Six additional host/policy/approval/budget regression tests, repository guards and whitespace checks passed. Reproduce:

```sh
AGENT_E2E_BROWSER_EXECUTABLE=/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome \
AGENT_E2E_UI_PORT=4187 tools/verify_current_snapshot.sh
```

Evidence: `build/host_tool_snapshot_verification.log`, `build/host_tool_extended_tests.log`, `build/host_tool_repo_guards.log`, `build/host_tool_build.log`, `build/host_tool_focused_tests.log`, `build/host_tool_openapi.log`, `build/host_tool_ui_build.log`, `build/host_tool_ui_tests.log`. Use assertion-enabled `build-snapshot/`; do not change the production Release `build/` configuration.

## Source versus runtime and publication boundaries

**No deployment was performed.** Public health still reports source `a0890c07a9dc53f1993ce726dea56aa04f156248`. The signed-device receipt milestone at `ac37208` and portability repairs through `2e49ff7` were already on GitHub before this work; this host-action slice is a further source milestone. A release needs applicable deployment authorization and the maintained deployment pipeline.

GitHub `origin/master` is the authorized publication destination. Panda remains excluded and on hold; never recreate its Agent-C repository. Preserve the existing unrelated untracked `.config/`, `AGENTS.md` and `PANDA_PUSH.md`, and never change `project.md` or stage credentials.

The existing native Agent-C session is paused. No parallel implementer, clone, worktree or editable shadow source was created. Arch owns execution/UI/database; the VPS provides shared OAuth and reverse-proxy ingress. Unrelated services, credentials and deployments remain untouched.

## Qualification gaps are not a product-work gate

The previous full offline suite and hosted matrix are not green. At `2e49ff7`, size-guard and self-contained eval passed; macOS, Windows, Ubuntu and container lanes had remaining failures. Known causes include the other OpenSSL const formatter, GNU warning flags on MSVC, a newer DTLS memory-BIO API in container tests, absent certificate/browser fixtures, consensus/memory regressions and a container module-download timeout.

Those are reported qualification gaps, not owner-only blockers and not permission to stop substantive product progress. Focused tests are not a full-matrix pass. Check CI for the exact new published revision. Do not use CTest numeric `--rerun-failed` indices across differently filtered suites; use anchored test names. Earlier details remain in `build/ci_2e49ff7_*` and the prior Git history.

## Next meaningful work

1. Trusted host receipt adapters and a stable per-tool journal inside model-driven turns; additional device adapters and strict resource meters.
2. Broker-backed goal/requirement/blocker authority and event-driven wakeups, preserving owner stops.
3. Immutable SOP registry, durable signals/timers, explicit business decisions and compensation.
4. Instance-scoped pollable embedded ports, provider/checkpoint contracts, remaining CLI decomposition and durable conversation identity.

Keep all unfinished acceptance criteria in `TODOS.md`; this is not completion of the rolling project.
