# Agent-C handoff

Updated 2026-11-11 on the Arch host, workspace `/home/bruce/work/agent-c`.

## Verified baseline

The implementation baseline is **`a0890c07a9dc53f1993ce726dea56aa04f156248`**, deployed at **https://agent.hubstack.cn** as release **`a0890c07a9dc-79684198d197`**. Handoff preparation verified public health/manifest revision equality, all **20 artifact SHA-256 hashes**, native schema **v35** and no open budget reservations. The documentation-only stop/handoff commit follows this runtime baseline; it does not require another deployment. Both implementation and handoff are published to GitHub `origin/master`.

Latest evidence is under `build/workflow_budget_*`. Earlier implementation checkpoints were `76412ac` (atomic device assignments), `c5a3e88` (initial handoff), and `a0890c0` (workflow budget reservations).

There are no pending implementation edits. Transactional workflow budget work is now implemented and verified; the next task is arbitrary host-tool effect recovery and device receipt resolvers. Preserve these pre-existing untracked local files/directories; they are not handoff changes and must not be staged:

```text
.config/
AGENTS.md
PANDA_PUSH.md
```

The owner explicitly requested **“prepare handoff and stop”**. The unfinished native Goal is to be paused after this handoff is saved. Do not resume implementation or deploy further changes until the owner requests continuation. The broader objective remains unfinished; this is not a completion claim.

## Standing owner instructions

- Own a coherent rolling source/doc snapshot for the embedded C core, customizable SOP pipelines and autonomous agents. Prefer refactoring and fresh deployments over compatibility layers or migration products.
- Authorized work runs autonomously by default. Raise only a genuine human-action dependency that cannot be resolved by the agent. Name affected requirements; continue independent work. Preserve explicit stops, resource limits, access control and configured business decisions.
- Keep the UI lean, clean, concise and responsive. Primary messages/results render once. Metadata and technical options stay collapsed. Remove developer-facing notes from product flows.
- **Publish to GitHub only.** The latest owner instruction puts Panda publishing on hold. Read `PANDA_PUSH.md` for hosting policy; Agent-C is explicitly excluded and must not be recreated there. Preserve its remote and exclusion.
- Execution, UI and database belong on this Arch host. VPS `root@hubstack.cn` supplies shared OAuth/reverse-proxy ingress. Remove only proven Agent-C VPS workloads; none were found. Preserve every other deployment.
- Shared Hubstack OAuth uses a separate Agent-C application. Owner `lzbgt@icloud.com` is administrator through pinned verified identities, not email-only authorization.
- Voice defaults to Aliyun Omni using the Model Studio key from `~/.env`. The inspected Mac reference is `ssh zongbaolu@bruce-mac.local:~/work/lang_learn` (the actual directory uses an underscore).
- No new generic permission prompts or arbitrary commit-count constraint. The owner prefers no intermediate status; follow applicable runtime communication requirements concisely.

## Delivered work

Read [current snapshot](docs/CURRENT_SNAPSHOT.md), [architecture](docs/ARCHITECTURE_CURRENT.md) and the top of [TODOS.md](TODOS.md) for the full implemented/remaining distinction.

| Area | Implemented and checked |
| --- | --- |
| Autonomy/core | Portable proposed-completion hook; host continuation within original budgets; durable SQLite requirements/dependencies/scoped blockers; evidence and revision checks; independent runnable projection. |
| SOPs/workflows | Versioned stage/action compilation to the existing durable DAG; definition digest/provenance; deterministic actions, scheduling and independent-branch progress after failure. |
| CLI | `exec`, stdin, resume/sessions/work commands; explicit directories; versioned JSONL stdout; successful final-message files; complete coalesced SSE delivery beyond bounded audit capture. |
| HTTP effects | Stable operation identity, intent/receipt journaling, guarded/idempotent dispatch, terminal reconciliation proofs, API/UI recovery and real crash tests. |
| Device dispatch | Atomic immutable assignment, active resource/rate admission, one outbox command and queued evidence; pinned node recovery; payload/key/device conflict rejection; scoped uncertain outcomes visible without full result payloads. |
| Workflow budgets | Durable per-attempt grants, atomic usage/state settlement and refunds, conservative crash charges, bounded parallel admission and retry-preserving waits. One-shot model steps now count. Provider token/time overages remain explicit. |
| WebUI | One primary chat renderer; concise composer and subject-oriented settings; collapsed metadata; responsive tools; no second transcript in details. |
| Presentations | Scripted canvas/DOM uses its own frame, input and clock lifecycle. Typing/polling does not restart animation; detached frames stop clocks; focused canvas/composer events stay local. |
| Voice | Aliyun `qwen3.5-omni-flash-realtime` / Ethan through authenticated same-origin WebSocket; server-held key; 16 kHz microphone / 24 kHz reply PCM; bounded JPEG streaming; one caption per reply; bounded media lifetime and cleanup. |
| Deployment | Arch services and dedicated PostgreSQL, immutable artifact manifests, verified shared OAuth/admin access, scoped VPS ingress and GitHub publication. |

Earlier implementation commits: `9136daa` presentation isolation; `e408637` Aliyun streaming voice; `9899a0d` responsive live qualification; `76412ac` atomic device dispatch and scoped recovery.

Device behavior and limits are in [DEVICE_ASSIGNMENTS.md](docs/DEVICE_ASSIGNMENTS.md), HTTP effects in [WORKFLOW_EFFECTS.md](docs/WORKFLOW_EFFECTS.md), and voice in [VOICE.md](docs/VOICE.md). Voice captions are ephemeral and the voice adapter has no workspace execution tools. Platform device dispatch is not certification of exactly-once physical action, controller fencing or board power-loss recovery.

## Verification evidence

The last implementation passed **23 native/CLI/API checks**, all Go tests, **54 browser checks**, generated OpenAPI checks and the production UI build. Additional device regressions, actual public API checks and four live viewport checks passed.

Ignored local evidence:

- `build/workflow_budget_snapshot_verification.log`: current canonical full verifier.
- `build/device_effect_final_unit_checks.log`: device transactions, killed writer, concurrent admission and actual one-worker independent progress.
- `build/device_effect_extended_regressions.log`: 11 additional device/workflow/rule/quorum/resource/deadline checks.
- `build/device_effect_regressions.log`: earlier nine selected device checks.
- `build/device_blocker_browser_checks.log`: visible human action at desktop and 320 px.
- `build/device_assignment_live_verification.json`: public API with synthetic nodes only; pinned node, payload conflict, one command and terminal replay.
- `build/workflow_budget_live_verification.json`: current OAuth redirects, anonymous API/voice denial, credential isolation and successful four-action SOP.
- `build/device_effect_live_ui_checks.log`: the prior 76412ac public 1440/320/390/768 px workspace/workflow/voice readiness and canvas animation check. This public browser lane was not rerun for a0890c0 before the owner stop; its current local browser lane passed all 54 cases.
- `build/workflow_budget_repo_guards.log`: current repository guards.
- `build/workflow_budget_extended_regressions.log`: 15 targeted budget/device/clamp checks.
- `build/workflow_budget_live_verification.json`: public rollout, authentication, four-action SOP and settled reservation checks.
- `build/workflow_budget_device_verification.json`: latest public synthetic device-assignment qualification.
- `build/aliyun_voice_roundtrip_public.log`: synthetic speech ASR, image word READY, reply caption and streamed PCM through the real public gateway/provider. Physical acoustic/mobile background qualification remains separate.

Reproduce the tracked verification lane:

```sh
AGENT_E2E_BROWSER_EXECUTABLE=/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome \
AGENT_E2E_UI_PORT=4187 tools/verify_current_snapshot.sh
tools/verify_current_snapshot.sh --core-only
(cd broker && go test -race ./...)
tools/verify_repo_guards.sh
```

Use assertion-enabled `build-snapshot/` for native checks. Production `build/` is a separate Release build; do not overwrite it with Debug test configuration. The canonical verifier uses local model stubs/fake browser media, not live paid inference. `verify_device_assignment.py` is an opt-in synthetic deployment test; its source and command are documented in the device contract.

## Next task after owner resume: extend effect recovery

The next concrete priorities are **arbitrary host-tool effect adapters and device receipt resolvers**, followed by broker-authoritative work state/wakeups. Keep the other explicit requirements in `TODOS.md`; do not declare the whole project finished after this slice.

Workflow budget code is implemented:

- `daemon/src/agent_db_workflow_budget.cpp` owns transactional grants, private attempt fencing, atomic task/usage settlement, retry-preserving capacity yield and consistent charged/held/uncertain snapshots.
- Schema v35 adds `workflow_budget_reservations`. Recovery marks interrupted grants lost without silently refunding possible spend. Settled observed telemetry remains separate from conservative lost charges.
- `daemon/src/workflow_budget.cpp` derives demand from task kinds and existing run bounds. Unknown run kinds remain model work; malformed optional fields must not crash inspection.
- `daemon/src/workflow_engine.cpp` obtains grants before dispatch and settles through the attempt authority. `claim_workflow_task_budgeted` now protects fairness counts/claim across SQLite connections.
- `daemon/src/run_request.cpp` counts one-shot model attempts and applies their step cap. HTTP reconciliation/new dispatch checks prevent spending the last granted network call twice.
- [WORKFLOW_BUDGETS.md](docs/WORKFLOW_BUDGETS.md) defines guarantees, evidence and limits. Admission is atomic; output-token caps are not a certified hard total-token bound, wall-time overhead exists, and remote/uninstrumented adapter internals require further metering. Recorded overages must never be hidden by clipping to a grant.
- `workflow_budget_tests` and `workflow_budget_parallel_smoke` cover real process kills before/after settlement, rollback, duplicate/stale settlement, concurrent connections, actual parallel model/tool requests and independent progress during capacity waiting.

When extending host tools, bind stable action/effect identity, persist intent, reconcile authoritative receipts/readback and preserve scoped uncertainty. Do not make a generic permission prompt the default recovery mechanism or infer exactly-once physical execution from local assignment dedupe.

Other required work remains: broker goal/requirement/blocker authority and wakeups; SOP registry/signals/decisions/compensation; subject-oriented SOP authoring; pollable instance-scoped embedded ports; provider metadata/checkpoints and CLI decomposition; durable conversation identity/pagination.

## Deployment and publication operations

- User services: `agent-c-daemon.service` on `127.0.0.1:18123`; `agent-c-gateway.service` on overlay `10.147.16.2:18784`.
- Dedicated Docker service: `agent-c-postgres`, loopback port `15439`; preserve all unrelated containers, databases and volumes.
- Private configuration: `~/.config/agent-c/{identity.json,gateway.json,daemon.env,postgres.env}`. Runtime data/releases: `~/.local/share/agent-c/`. Keep secrets out of stdout, argv, browser config and commits.
- Aliyun key locator: `ALIYUN_MODEL_STUDIO_AK` in owner-only `~/.env`; the gateway reads it internally. Never print its value. DeepSeek is the native default provider.
- Native owner Chrome may have an unsent draft. Discover current windows before interaction; preserve drafts and other project windows. Headless live checks use an isolated context and do not prove a fresh owner OAuth callback or physical microphone quality.

After verified code changes are committed:

```sh
cmake --build build --target agentd -j 4
npm run build --prefix ui
(cd broker && go build -trimpath -ldflags='-s -w' -o ../build/agentd-gateway ./cmd/agentd-gateway)
python3 tools/deploy/install_arch.py --identity-file /home/bruce/.config/agent-c/identity.json
python3 tools/deploy/verify_arch.py --output build/arch_live_verification.json
```

Check active work before a native daemon restart. The installer preserves an unchanged native daemon during UI/gateway releases. Recheck public source revision, artifact hashes, authentication, relevant live behavior and GitHub ref. Keep deployment ingress changes scoped to Agent-C.

GitHub authentication works through the existing private credential store. Do not print that file, use force-push, or enable curl credential tracing:

```sh
env -u GIT_CURL_VERBOSE -u GIT_TRACE_CURL -u GIT_TRACE -u GIT_TRACE_PACKET \
-u GIT_TRACE2 -u GIT_TRACE2_EVENT GIT_TRACE_REDACT=1 GIT_TERMINAL_PROMPT=0 \
GIT_ASKPASS=/bin/false git \
-c 'credential.helper=store --file /home/bruce/.git-credentials' \
push origin HEAD:refs/heads/master > build/github_push.log 2>&1
git ls-remote origin refs/heads/master
```

The native Goal owns continuation. Honor the explicit owner stop: pause the unfinished Goal after saving this handoff and resume only on a subsequent owner request. Do not recreate/narrow it or mark the broader project complete. Local milestone report: `build/arch_project_report.json`; `codexw report --file ...` last returned 404 for the host project-dashboard route. Keep the local report current and recheck reporting at meaningful boundaries; that host reporting fault is not an owner blocker for repository work. No subagents were requested or used.
