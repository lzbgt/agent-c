# Agent-C handoff

Prepared 2026-11-10 on the Arch host, workspace `/home/bruce/work/agent-c`.

## Verified baseline

Implementation commit **`76412ac05524e2c03e9e1f6ef1a626cda65650b1`** is deployed at **https://agent.hubstack.cn** and published on GitHub `origin/master`. The immutable Arch release is `76412ac05524-c5fda8b92bfe`. Public health and its manifest match that implementation revision; all **20 artifact SHA-256 hashes** were checked during handoff preparation. This handoff is a documentation-only addition after that runtime baseline.

There are no pending implementation edits. The next budget task was inspected but **not started**. Preserve these pre-existing untracked local files/directories; they are not handoff changes and must not be staged:

```text
.config/
AGENTS.md
PANDA_PUSH.md
```

The native project Goal remains active and unfinished. Preparing this handoff does not mark the broader objective complete or change its scope.

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
| WebUI | One primary chat renderer; concise composer and subject-oriented settings; collapsed metadata; responsive tools; no second transcript in details. |
| Presentations | Scripted canvas/DOM uses its own frame, input and clock lifecycle. Typing/polling does not restart animation; detached frames stop clocks; focused canvas/composer events stay local. |
| Voice | Aliyun `qwen3.5-omni-flash-realtime` / Ethan through authenticated same-origin WebSocket; server-held key; 16 kHz microphone / 24 kHz reply PCM; bounded JPEG streaming; one caption per reply; bounded media lifetime and cleanup. |
| Deployment | Arch services and dedicated PostgreSQL, immutable artifact manifests, verified shared OAuth/admin access, scoped VPS ingress and GitHub publication. |

Recent implementation commits: `9136daa` presentation isolation; `e408637` Aliyun streaming voice; `9899a0d` responsive live qualification; `76412ac` atomic device dispatch and scoped recovery.

Device behavior and limits are in [DEVICE_ASSIGNMENTS.md](docs/DEVICE_ASSIGNMENTS.md), HTTP effects in [WORKFLOW_EFFECTS.md](docs/WORKFLOW_EFFECTS.md), and voice in [VOICE.md](docs/VOICE.md). Voice captions are ephemeral and the voice adapter has no workspace execution tools. Platform device dispatch is not certification of exactly-once physical action, controller fencing or board power-loss recovery.

## Verification evidence

The last implementation passed **21 native/CLI/API checks**, all Go tests, **54 browser checks**, generated OpenAPI checks and the production UI build. Additional device regressions, actual public API checks and four live viewport checks passed.

Ignored local evidence:

- `build/device_effect_snapshot_verification.log`: canonical full verifier.
- `build/device_effect_final_unit_checks.log`: device transactions, killed writer, concurrent admission and actual one-worker independent progress.
- `build/device_effect_extended_regressions.log`: 11 additional device/workflow/rule/quorum/resource/deadline checks.
- `build/device_effect_regressions.log`: earlier nine selected device checks.
- `build/device_blocker_browser_checks.log`: visible human action at desktop and 320 px.
- `build/device_assignment_live_verification.json`: public API with synthetic nodes only; pinned node, payload conflict, one command and terminal replay.
- `build/device_effect_live_verification.json`: OAuth redirects, anonymous API/voice denial, credential isolation and successful four-action SOP.
- `build/device_effect_live_ui_checks.log`: 1440/320/390/768 px workspace/workflow/voice readiness and actual canvas animation while typing.
- `build/device_effect_repo_guards.log`: repository guards.
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

## Next task: atomic workflow budget reservations

The next concrete priority is **workflow-wide reservation/settlement across parallel tasks**, followed by arbitrary host-tool effects/device receipt resolvers and broker-authoritative work state/wakeups. Keep the other explicit requirements in `TODOS.md`; do not declare the whole project finished after this slice.

Inspection findings, not an implemented design:

- `daemon/src/agent_db_jobs.cpp:claim_workflow_task_budgeted` currently enforces in-flight fairness counts under a process mutex; it does not reserve workflow tool/step/time/token capacity transactionally.
- `daemon/src/workflow_engine.cpp:execute_claimed_task` computes remaining allowance from settled cumulative task counters before execution. Concurrent workers can each see the same remaining allowance.
- `daemon/src/workflow_run_budget_clamp.cpp` clamps a run to that observed remainder. The completion-token cap does not establish a strict total-token bound because prompt usage is reported later.
- Attempt completion uses cumulative usage and task upserts; usage lost before settlement is not conservatively reserved across restart. Admission, settlement, stale-attempt rejection and crash recovery need one coherent contract.
- Device rate/resource admission is already atomic in `daemon/src/agent_db_edge_admission.cpp`; it is a separate scope and must not be advertised as workflow budget reservation.

Design explicit grants, durable reservation identity, settlement/refund rules, uncertain-attempt handling and cancellation semantics before wiring the scheduler. Verify concurrent workers/SQLite connections, retry charges, actual model/tool ceilings, crashes before/after dispatch/settlement, and preservation of independent work/stops. Do not promise a hard provider total-token cap from an output-token setting or an unverified token estimate. Update APIs/docs/tests alongside the implementation.

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

The native Goal owns continuation. Do not recreate or narrow it, mark it complete, or infer a pause from this handoff. Local milestone report: `build/arch_project_report.json`; `codexw report --file ...` last returned 404 for the host project-dashboard route. Keep the local report current and recheck reporting at meaningful boundaries; that host reporting fault is not an owner blocker for repository work. No subagents were requested or used.
