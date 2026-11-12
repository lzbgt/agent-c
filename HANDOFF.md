# Agent-C handoff

Updated 2026-11-12 in the existing Arch checkout on `master`.

## Delivered now: durable explicit process decisions

A `decision` action waits for a configured continue/fail choice without holding a worker, spending an attempt or reserving execution budget. First activation and yielding commit together; one immutable answer and queued-task wakeup commit together. Independent work continues even with one worker. Restart, expiry and owner cancellation preserve the original decision and cannot trigger a different answer or revive stopped work.

The authenticated API exposes decision metadata and exact idempotent receipts. It derives outcomes from the saved definition, rejecting client outcomes/actors and unknown choices. This is a business-process choice under the daemon access boundary, not per-person manager authorization. Generic signals, branches/loops, compensation and active-run migration remain unimplemented.

The UI provides an explicit review SOP template, configured choice buttons and recorded outcomes. Lost responses preserve the exact choice/note/delivery key. Scope changes discard stale responses; the workflow view also fences late lookups and refreshes active detail to discover newly activated reviews. Notes are not a secret store.

Contracts and example: [WORKFLOW_DECISIONS.md](docs/WORKFLOW_DECISIONS.md), generated OpenAPI, [provider-free sample](examples/workflow_sop_review_decision.json).

## Previously delivered: immutable saved SOP versions

The daemon-local SOP library saves, lists and reads exact `(id, version)` definitions. Identical canonical source is idempotent; changed source at an existing identity is rejected. A submitted `sop_ref` copies integrity-checked source and its hash into the normal workflow snapshot. A newer version does not alter an existing run, including across restart.

The composer can save the current SOP without running it, browse paged versions and load an exact hash-pinned reference. Submission remains explicit. Interrupted/repeated saves preserve drafts; conflicts and old-daemon errors stay visible without blocking ordinary authoring. Known inline credential fields are rejected, not persisted as redacted execution source.

Saving validates SOP structure, not every task payload and not execution authority. Normal task/policy/budget/admission checks and cancellation remain effective. Inline SOP submission remains ad hoc; no delete/overwrite endpoint, implicit latest version, general signal node or active-run migration is claimed.

Contracts: [SOP workflows](docs/SOP_WORKFLOWS.md) and generated OpenAPI. Native migration/race/restart/integrity tests, authenticated real-daemon flows and the full snapshot lane passed. CLI daemon, in-process API and embedded HTTP service routes are covered.

## Previously delivered: guarded deterministic host actions

Workflow and SOP actions can invoke a registered host tool with `kind: host_tool`, without a model. Durable intent and original-result capture prevent a scheduler restart from blindly repeating a host-side effect. Saved results release dependents without another tool invocation; ambiguous outcomes expose a scoped blocker while independent work continues.

The action uses existing daemon host/sandbox policy, allow/deny lists and explicit approvals. Policy checks precede dispatch admission; cancellation during approval causes no effect. Saved results do not require another execution approval. Request identity pins tool arguments, execution root and session context. Caller-declared host idempotency and invented reconciliation URLs are rejected.

Known nonzero exits are saved failures, not automatic retries. Timeouts, lost results and uncertain persistence remain unresolved. This is a useful deterministic workflow boundary, not a universal journal around LLM-selected tools or proof of business/physical exactly-once execution. Generic trusted host receipt adapters and strict non-cooperative metering remain outstanding.

Contracts and usage: [durable effects](docs/WORKFLOW_EFFECTS.md), [current snapshot](docs/CURRENT_SNAPSHOT.md), [roadmap](TODOS.md).

## Verification

Focused decision checks cover v37→v38 migration, atomic wait/wakeup, conflicting answers, exact immutable receipt replay, restart, owner-stop preservation, expiry, corrupt definitions and zero wait budget/attempt consumption. The real one-worker daemon proves unrelated progress and pinned-SOP execution. Browser coverage includes deliberate choices, repeat-click fencing, uncertain same-choice retry, 320px layout, stopped controls and late selected-workflow/connection responses. Existing SOP, host/device effect and workflow-policy regressions remain in the canonical lane.

The final canonical verifier passed **29 native/CLI/API checks**, all Go broker/gateway tests, generated OpenAPI checks, the UI production build and **69 browser checks**. Two additional service/auth tests, repository guards and whitespace checks passed. Reproduce:

```sh
AGENT_E2E_BROWSER_EXECUTABLE=/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome \
AGENT_E2E_UI_PORT=4187 tools/verify_current_snapshot.sh
```

Evidence: `build/decision_snapshot_verification.log`, `build/decision_focused_tests.log`, `build/decision_entrypoints.log`, `build/decision_repo_guards.log`. Earlier saved-SOP and host-action evidence remains under `build/sop_registry_*` and `build/host_tool_*`. Use assertion-enabled `build-snapshot/`; do not change the production Release `build/` configuration.

## Source versus runtime and publication boundaries

**No deployment was performed.** Public health still reports source `a0890c07a9dc53f1993ce726dea56aa04f156248`. The signed-device receipt milestone at `ac37208` and portability repairs through `2e49ff7` were already on GitHub before this work; the host-action milestone at `869312b`, the saved-SOP milestone at `8394a83` and this process-decision slice are further source milestones. A release needs applicable deployment authorization and the maintained deployment pipeline.

GitHub `origin/master` is the authorized publication destination. Panda remains excluded and on hold; never recreate its Agent-C repository. Preserve the existing unrelated untracked `.config/`, `AGENTS.md` and `PANDA_PUSH.md`, and never change `project.md` or stage credentials.

The existing native Agent-C session is paused. No parallel implementer, clone, worktree or editable shadow source was created. Arch owns execution/UI/database; the VPS provides shared OAuth and reverse-proxy ingress. Unrelated services, credentials and deployments remain untouched.

## Qualification gaps are not a product-work gate

The previous full offline suite and hosted matrix are not green. At `2e49ff7`, size-guard and self-contained eval passed; macOS, Windows, Ubuntu and container lanes had remaining failures. Known causes include the other OpenSSL const formatter, GNU warning flags on MSVC, a newer DTLS memory-BIO API in container tests, absent certificate/browser fixtures, consensus/memory regressions and a container module-download timeout.

Those are reported qualification gaps, not owner-only blockers and not permission to stop substantive product progress. Focused tests are not a full-matrix pass. Check CI for the exact new published revision. Do not use CTest numeric `--rerun-failed` indices across differently filtered suites; use anchored test names. Earlier details remain in `build/ci_2e49ff7_*` and the prior Git history.

## Next meaningful work

1. Trusted host receipt adapters and a stable per-tool journal inside model-driven turns; additional device adapters and strict resource meters.
2. Broker-backed goal/requirement/blocker authority and event-driven wakeups, preserving owner stops.
3. General durable signals/timers, person/role-bound approvals, branches/loops, compensation and active-run version migration.
4. Instance-scoped pollable embedded ports, provider/checkpoint contracts, remaining CLI decomposition and durable conversation identity.

Keep all unfinished acceptance criteria in `TODOS.md`; this is not completion of the rolling project.
