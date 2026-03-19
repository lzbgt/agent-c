# Roadmap / TODOs (highest leverage)

Date: 2026-02-19

This roadmap is biased toward “power unleashed” coming from the **agentic framework itself**:

- autonomous high-efficiency scheduling (priorities, budgets, retries, backoff)
- task continuity across time (durable + resumable execution)
- memory architecture that compensates for stateless/context-bounded LLMs
- rolling consolidation + correlation as time advances
- result correctness (deterministic checks, replay, validations)

Observability (trace/timeline) matters, but it is **not** the origin of capability; it is the proof/debug surface.

## Active highest goals (current focus)

## codexw external integration lane

These items are for the sibling `codexw` broker/WebUI integration path. Treat
`docs/spec/codexw_broker_webui_handoff_v0.md` as the work package and upstream
contract map.

- [x] Broker/WebUI: implement the `codexw` session lifecycle path (create / attach / inspect / renew / release) with explicit owner / observer / rival lease UX and `attachment_conflict` handling.
  - broker now exposes real `/v1/agents/{agent_id}/sessions/...` aliases, the WebUI uses them for lifecycle control, and lease conflict handling is covered by broker-backed Playwright regression.
- [x] Broker/WebUI: make SSE replay/resume with `Last-Event-ID` a first-class reconnect path for `codexw` sessions instead of relying on best-effort live-only streams.
  - WebUI now persists a bounded broker-session event buffer and replay cursor locally, rehydrates the live conversation after refresh, and reconnects through `/v1/agents/{agent_id}/sessions/{session_id}/events` with `Last-Event-ID`.
- [x] Broker/WebUI: ship shell-first host examination for `codexw` sessions (`shell list/start/detail/poll/send/terminate`) and keep transcript/event correlation visible.
  - broker now exposes native `/v1/agents/{agent_id}/sessions/{session_id}/shells...` and orchestration aliases, and the WebUI connection settings include a broker session operator surface for orchestration plus shell start/detail/poll/send/terminate alongside the existing session stream diagnostics.
- [x] Broker/WebUI: expose `codexw` service and capability surfaces as operator tooling before inventing parallel synthetic abstractions.
  - broker now exposes `/services...` and `/capabilities...` session aliases, and the WebUI operator section exposes service list/detail/attach/wait/run and capability list/detail against the current broker-backed session.
- [x] Broker/WebUI: keep artifact-centric UX explicitly separate until `codexw` provides a real artifact list/detail/content API; gather missing cases as requirements rather than faking an artifact browser.
  - broker mode now treats the session artifact list as conditional; when `/api/v1/session/artifacts` is unsupported, the history panel shows an explicit artifact-boundary note instead of a fake empty artifact browser, and broker runtime ack flow skips artifact-catalog assumptions on that path.
- [x] Broker/WebUI: add a real codexw-to-codexw cross-deployment collaboration/handoff lane so deployment-to-deployment work transfer is broker-mediated, session-aware, and replayable instead of a manual operator convention.
  - team run handoff events now support explicit replayable cross-deployment records (`handoff_id`, state, source/target deployment, source/target session), and the WebUI team run status panel can emit plus accept/decline those handoffs directly.

1) **macOS agentd host (M2)** — keep host daemon runnable/operable on macOS (Windows/Linux deferred).
   - [x] Added macOS launchd install/uninstall helper scripts for agentd.
   - [x] Added macOS broker+WebUI compose guidance.
   - [x] Added macOS full-stack verification script (`tools/verify_mac_full_stack.sh`).
   - [x] Added macOS local verification script (agentd + WebUI without Docker).
   - [x] macOS local verify script can run diagnostics provider tests when `MAC_LOCAL_PROVIDER_TEST=1`.
   - [x] macOS provider smoke: DeepSeek reasoner tool-call + Moonshot tool-call.
   - [x] macOS local provider tests verified against `~/.env` (DeepSeek + Kimi/Moonshot).
   - [x] macOS packaging/codesign checklist for production distribution (launchd + notarization).
2) **Production deployment readiness (agentd + broker + WebUI)** — stable, secure, operable deployment.
   - [x] Added `docs/DEPLOYMENT.md` checklist + topology guidance.
   - [x] Added service manager templates (systemd + macOS launchd).
   - [x] WebUI: user-gesture autoplay unlock so scene audio can start after first interaction.
   - [x] Add Windows service template.
   - [x] Production hardening review for docker stack (secrets, TLS, backups, metrics).
   - [x] Compose build reliability: document Docker Desktop resource-limit troubleshooting (unpigz/runc) and prebuilt-image path.
   - [x] WebUI runtime defaults via `agentui-config.js` + `VITE_AGENTUI_*` env overrides (no rebuild required).
   - [x] Broker tunables: max body/header bytes + HTTP timeouts (flags + env).
   - [x] Agentd HTTP hardening: header size cap + read timeouts (env) with smoke test.
   - [x] Diagnostics endpoints: `/api/v1/diagnostics`, `/api/v1/diagnostics/providers`, `/api/v1/diagnostics/provider_test`.
- [x] Diagnostics reference doc: `docs/DIAGNOSTICS.md` with usage examples.
   - [x] WebUI diagnostics panel + provider tests (DeepSeek/Moonshot) to confirm keys and run health checks.
   - [x] WebUI run settings panel (model/base_url/proxy/timeout, run limits, OpenRouter picker).
   - [x] WebUI App.tsx refactor (<2000 LOC) with modular panels/hooks.
   - 2026-03-08: split `ui/src/components/SettingsDrawer.tsx` into dedicated settings section components so the drawer dropped below the 2000 LOC hygiene bar without changing UI behavior.
   - 2026-03-08: split `ui/src/components/BrokerPanel.tsx` into broker page subcomponents for agents/connectors/members/audit, keeping the broker UI path under the same file-size hygiene bar.
   - [x] Refactor oversized daemon/edge/db + CLI host toolset files into SOLID submodules (<2000 LOC each).
   - [x] Tool-loop guard: `max_tool_call_args_chars` (daemon default + run override + core limit event).
  - [x] Broker proxy forwarding sets `X-Request-ID` and `X-Trace-ID` when missing (trace correlation).
  - [x] Broker membership management audit trail + SSE event + `/v1/trace` integration.
  - [x] WebUI broker console (agent list + membership management + audit).
  - [x] Evidence bundle capture for agentd/broker (`tools/capture_agent_evidence_bundle.sh`).
  - [x] WebUI Playwright smoke tests for broker + agentd host (UI-only).
  - [x] WebUI E2E real runs now capture Playwright artifacts (trace/video/screenshot) for headless verification.
  - [x] WebUI live event stream bounded (ring buffer) to prevent long-running job memory growth.
  - [x] WebUI scene cache + job resume metadata bounded (LRU + TTL) to avoid multi-session memory drift.
  - [x] Evidence bundle validation tool for agentd/broker (machine-checkable gate).
  - [x] Data-driven scenario pack for agentd/broker (scripted runs + evidence capture).
  - [x] Scenario pack runner for multi-scenario runs + evidence validation.
- [x] One-command devstack (agentd + broker + WebUI) with smoke checks + evidence bundle.
- 2026-03-19: AVM subprocess exec now closes inherited non-stdio file descriptors too, and the AVM smoke proves an
  ungraceful daemon death cannot leave an orphaned runner pinning the same listener port across restart.
- [x] macOS full-stack compose verification on this host (Docker Desktop running).
  - 2026-02-19: `docker info` not responding / daemon not running; host-stack verification skipped until Docker Desktop/Colima is running.
  - 2026-02-19: attempted `open -a Docker` and polled `docker info` (12x/120s); all attempts timed out.
  - 2026-02-19: `docker info` now responds; Docker Desktop is running on this host (aarch64).
  - 2026-02-19: `tools/verify_mac_local_stack.sh` succeeded with `MAC_LOCAL_SKIP_UI=1`.
  - 2026-02-19: local `postgresql@17` brew service fails; `/opt/homebrew/var/log/postgresql@17.log` shows missing `/opt/homebrew/lib/postgresql@17` (shared_preload_libraries=timescaledb). Likely needs a reinstall or config change before using local Postgres as Docker fallback.
  - 2026-02-19: fixed local brew Postgres by symlinking `/opt/homebrew/lib/postgresql@17` and `/opt/homebrew/share/postgresql@17/{timezone,timezonesets}` to the shared dirs and commenting `shared_preload_libraries=timescaledb`; `pg_isready` now reports accepting connections.
  - [x] Host-mode macOS full-stack verification script (Docker only for Postgres/Keycloak) added as fallback.
  - 2026-02-20: `tools/verify_mac_full_stack.sh` succeeded (log: `build/verify_mac_full_stack.log`).
  - 2026-02-20: mac local provider tests passed (DeepSeek + Moonshot) via `MAC_LOCAL_PROVIDER_TEST=1` using keys from `~/.env`.
  - [x] WebUI connection profiles for multi-agentd deployments (direct or broker-backed).
  - [x] Broker-mode connection profile persistence (server-side store; avoid localStorage-only URLs).
  - 2026-02-24: Broker client prefs endpoint + WebUI broker-mode profile sync (no tokens).
  - 2026-02-24: Added devstack OIDC helper (`tools/devstack_oidc_token.sh`).
  - 2026-02-24: Added broker client prefs smoke test (`tests/broker_client_prefs_smoke.sh`).
  - 2026-02-24: WebUI server-side profile sync defaults to auto when supported (auth required; tokens remain local).
  - 2026-02-25: WebUI team console includes quorum request + approvals panel (merged from Broker panel).
  - [x] WebUI profile-specific run settings per connection profile (model/provider + run limits).
  - [x] Broker multi-deployment routing (`deployment_id`, `X-Agentd-Deployment`) + WebUI deployment selector + docs.
  - [x] OTA update pipeline: agentd OTA endpoint + operator update command, WebUI multi-deployment fanout via broker bulk OTA endpoints.
  - [x] OTA task continuity: drain mode + documented restart recovery for async jobs/workflows after update.
  - [x] WebUI OTA status polling per deployment (drain + error surfacing).
  - [x] OTA drain now waits for running jobs/workflow tasks (best-effort) and status includes inflight counts.
  - [x] OTA continuity smoke script (`tools/verify_ota_continuity.sh`) to validate resume after restart.
  - [x] WebUI Memory Explorer panel (structured query + trace correlation + checkpoints).
  - [x] WebUI media_observe observers bounded (TTL + max) to prevent listener leaks.
  - [x] WebUI media_unobserve RPC to detach observers deterministically.
  - [x] Memory context index mode (progressive disclosure) with token estimates (files list + costs; index mode).
  - [x] WebUI memory context selector supports index mode (progressive disclosure).
  - [x] Memory observations + timeline retrieval tools (claude-mem style) for progressive disclosure workflows.
  - [x] Deterministic `memory_timeline` workflow task (host tool) with OpenAPI + docs + smoke test.
  - [x] Memory index bounded file reads (avoid unbounded memory spikes on large files).
  - [x] Broker memory salience fan-out + WebUI salience panel (uses recaps tuning).
  - [x] WebUI history expansion state bounded to prevent long-running UI memory drift.
  - [x] AgentdApi request-path job GC to keep embedded/relay job state bounded without a dedicated GC thread.
  - [x] Job stream event payloads capped with `data_truncated` metadata to prevent unbounded in-memory growth.
  - [x] Key discovery falls back to passwd-derived home when HOME is missing (service contexts).
  - 2026-03-15: safe `/api/v1/config` now exposes WebRTC backend availability (`builtin_available`, `bundled_available`, `external_available`, `default_runtime_kind_available`), and the main runtime smoke proves the configured-but-unavailable `default_runtime_kind=external` case before start-time failure.
3) **Next-gen contract foundation (agent-core + agentd + broker + WebUI)** — make interop explicit and replayable.
  - [x] Add a unified capability descriptor (`/api/v1/caps`) with protocol versions + limits (agentd + WebUI + broker proxy).
  - [x] WebUI: use `/api/v1/caps` to hide/disable unsupported features and cache last-known caps.
  - [x] Define a single event schema for run/workflow events and validate in CI (schema registry + tests).
 - [x] Add idempotency keys to broker proxy/orchestrate (safe retries with audit trail).
 - [x] Introduce replay bundles for deterministic runs (inputs + hashes + tool outputs) with fixture tests.
 - [x] Transport-agnostic relay interface for broker/connector (transport.Conn + WebSocket adapter).

## Weighted tasks (next up)

- [x] W=13 — Browser secret boundary hardening: keep broker/agentd auth tokens and provider API keys out of durable browser storage, then finish the move to HttpOnly/session-backed auth for brokered deployments.
  - 2026-03-07: moved WebUI broker/daemon tokens and provider API keys out of `localStorage`; they now live in browser session storage and legacy localStorage secrets are scrubbed on load.
  - 2026-03-07: connection profile persistence/server sync remains non-secret only; docs updated to reflect session-scoped secret behavior.
  - 2026-03-15: verified the broker-backed cookie/session exchange lane is complete in source: the WebUI exchanges the bearer token through `POST /v1/auth/session`, scrubs the profile/session token afterward, exposes explicit cookie-clear control, and Playwright coverage now checks both exchange and delete flows.
- [x] W=12 — Authoritative host CI: add Linux/macOS jobs that run `tools/verify.sh --ui-install --repo-guards`, gate DB/OpenAPI/docs drift, and promote compose broker smokes into a reliable required lane.
  - 2026-03-07: current local verify drift found `docs_sanity_tests` red due `docs/DB.md` schema lag; fixed header to `v33` to restore the first failing gate.
  - 2026-03-07: added GitHub Actions host verify matrix (`ubuntu-latest`, `macos-14`) that runs `tools/verify.sh --ui-install --repo-guards` with network-provider smokes disabled.
  - 2026-03-08: local repo guards now exclude generated build/output/cache bulk by default; `--strict` keeps the CI/full-workspace size gate.
  - 2026-03-08: added a dedicated `compose-broker-smoke` GitHub Actions workflow plus `tools/verify_compose_broker_smokes.sh` so CI can bootstrap one compose stack and reuse it across a compact broker smoke batch.
  - 2026-03-14: closed the stale roadmap item; the repo-side CI lanes already exist. Making the compose broker workflow branch-protection-required is an external repo setting, not a source change.
- [x] W=10 — Contract generation + UI decomposition: generate typed WebUI clients/schemas from OpenAPI, then split `useUiSettings` / `App.tsx` around connection state, run state, and team orchestration state.
  - 2026-03-07: review identified duplicated manual contracts and `any`-heavy UI state as the main velocity risk after security/CI.
  - 2026-03-07: extracted connection-profile normalization/secret merge helpers from `useUiSettings.ts` into `ui/src/hooks/uiSettingsProfiles.ts`; UI build stayed green.
  - 2026-03-07: fixed OpenAPI bundle issues blocking codegen (broken internal `$ref`s, missing exported schemas, invalid duplicate/quoted YAML entries).
  - 2026-03-07: added `tools/generate_ui_openapi_types.sh`, pinned `openapi-typescript`, and wired `npm run openapi:types:check` into `tools/verify.sh`.
  - 2026-03-07: corrected `client_prefs` OpenAPI to match real generic server behavior (connection profiles plus workflow/run-watch/team cursor prefs) and added missing diagnostics `sandbox_mount_allowlist` contract.
  - 2026-03-07: bound `ui/src/api/schemas/daemon.ts` to generated agentd types for health/caps/diagnostics/client-prefs/sandbox validation and removed several `any` fields.
  - 2026-03-07: bound the high-traffic broker list/replay schemas (`agents`, `deployments`, `members`, `membership_audit`, `connectors`, `events/replay`) to generated broker types.
  - 2026-03-07: extracted typed run-watch parsing/merge helpers from `App.tsx` into `ui/src/runWatchPrefs.ts` and updated `ui/src/jobStore.ts` to use typed run-watch entries.
  - 2026-03-07: bound the broker team/member/quorum/run/orchestrator/guidance schemas to generated OpenAPI types, keeping explicit widening only for live fields still ahead of the published component contracts (for example `member_sessions` on team run status).
  - 2026-03-15: split `ui/src/components/broker/BrokerTeamRunPanel.tsx` into focused create/runtime and control/moderator/approvals hooks, leaving the panel as a composition shell instead of an 1800-line state owner.
  - 2026-03-15: extracted broker-wide event stream/replay state from `ui/src/components/BrokerPanel.tsx` and team replay/cursor merge state from `ui/src/components/broker/BrokerTeamConsole.tsx` into focused hooks.
  - 2026-03-15: extracted `App.tsx` shell/session/layout state into `ui/src/hooks/useAppShellState.ts` and moved the scene/team hub/history main-column render into `ui/src/components/app/AppMainColumn.tsx`.
  - 2026-03-15: split `HistoryPanel.tsx` into a thin shell plus dedicated team-chat state, team section, and technical history section modules; also fixed stale team search filtering by wiring `teamSearch` into the derived timeline memo.
  - 2026-03-15: split `HistoryPanelTeamSection.tsx` into focused controls, summary, filters, and timeline modules, reducing the remaining team-history render surface to a thin shell without changing history/team behaviors.
  - 2026-03-15: split `ToolResultView.tsx` into focused parsed/plain render modules plus dedicated tool-result state/utilities, and split `ApprovalQueuePanel.tsx` into approval-queue state, filters, list, and detail modules with browser coverage for approval load/select/decision flows.
  - 2026-03-15: split `BrokerTeamConsole.tsx` into a thinner shell plus dedicated setup and control hooks, reducing inline CRUD/quick-builder/member/quorum state ownership and fixing `forcedTab="advanced"` rendering to use the effective active tab.
  - 2026-03-15: split `BrokerPanel.tsx` into a thinner shell plus dedicated broker state and deployments/memory/events section modules, and fixed broker page persistence so a stored page restores instead of always resetting to `teams`.
  - 2026-03-15: split `BrokerOrchestratorRunPanel.tsx` into a thin shell plus dedicated overview/revisions/mutations sections and a focused state hook; also fixed stale auto-heartbeat closures and cleared stale run details when switching teams.
  - 2026-03-15: split `TeamRunStatusPanel.tsx` into a state hook plus focused overview/goal/handoff sections and split `TeamRunCreatePanel.tsx` into dedicated advanced/runtime/approvals sections, reducing both panels to composition shells while keeping broker team-run Playwright coverage green.
  - 2026-03-15: split `WorkflowPanel.tsx` into a focused state hook plus dedicated workflow list, schedules, and detail/DAG sections, and added direct browser regression coverage for workflow detail rendering.
  - 2026-03-15: split `MemoryPanel.tsx` into a focused state hook plus dedicated query/correlation/checkpoints, index/salience, recaps/schedule, and retention sections, and added direct browser regression coverage for the advanced memory flow.
  - 2026-03-15: split `RunDiffPanel.tsx` into focused loader/replay/evidence sections plus dedicated diff/state utilities, and added direct browser regression coverage for replay + DB evidence comparison.
  - 2026-03-15: split `SettingsConnectionSection.tsx` into focused profile/sync, transport, session lease/operators, and execution access sections, leaving the parent as a thin composition shell while preserving broker cookie/session/operator coverage.
  - 2026-03-15: split `ConversationView.tsx` into a composition shell plus dedicated conversation event rendering and client-RPC/session-ack state modules, and added direct browser regression coverage for `entity_apply` scene updates plus client-event acknowledgements.
  - 2026-03-15: split `PromptBar.tsx` into dedicated prompt-bar state, details drawer, and composer-body modules, and added direct browser regression coverage for upload staging, drawer details, and collapsed preview state.
  - 2026-03-15: split `ConversationUiActionCard.tsx` into dedicated RPC and acknowledgement action modules plus shared conversation-ui-action types, and added browser coverage for client-state snapshot and notify acknowledgement flows.
  - 2026-03-15: split `ConversationUiActionRpcCard.tsx` into a thin card shell plus dedicated RPC state and executor modules, moved auto-run scheduling out of render, and added browser coverage for worker-backed `script_eval` DOM mutation/progress reporting.
  - 2026-03-15: split `WorkflowComposer.tsx` into dedicated composer state, toolbar, body, and status modules plus shared workflow-composer utilities/types, and promoted persisted wait-resume coverage into the canonical direct smoke lane.
  - 2026-03-15: split `SceneView.tsx` into dedicated scene state, entity-card, canvas, DOM, and utility/type modules, and added direct browser regression coverage for latest-only scene history plus expand/collapse behavior.
  - 2026-03-15: split `WorkflowGraphComposer.tsx` and `WorkflowSchedulesSection.tsx` into focused graph toolbar/canvas/inspector and schedule filters/create/list/runs modules, fixing the graph target-normalization helper path while keeping workflow editor and schedule browser coverage green.
  - 2026-03-15: split `SettingsDrawer.tsx` into a thin shell plus dedicated drawer-state hook/shared types, and split broker session operators into a focused state hook plus orchestration/shell/service/capability sections while keeping broker settings coverage green.
  - 2026-03-15: split `BrokerTeamMembersPanel.tsx` into focused members form/list sections and split `BrokerTeamGuidancePanel.tsx` into a guidance state hook plus composer/list sections, reducing both broker team surfaces to composition shells.
  - 2026-03-15: split `useUiConnectionSettings.ts` into dedicated profile-state, broker cookie-session, and server-pref sync hooks, and moved type-only consumers from `useUiSettings.ts` to `uiSettingsTypes.ts` so the shell hook no longer acts as a type barrel.
  - 2026-03-15: split `useUiRunSettings.ts` into dedicated execution/provider/limits and memory-context hooks, leaving the parent as an override/composition shell while preserving direct/broker settings coverage.
  - 2026-03-15: split `useTeamChatOrchestration.ts` into focused conversation/persistence and action/queue hooks plus shared team-orchestration types/utilities, and hardened queue hydration by normalizing malformed local-storage queue data before evaluating pending actions.
  - 2026-03-15: split `useAppDataPlane.ts` into dedicated session-query and session-mutation hooks plus shared app-data-plane types, reducing the parent to a composition shell while keeping session lease, daemon-defaults, and direct/broker data-plane coverage green.
  - 2026-03-15: split `useRuntimePlane.ts` into dedicated run-watch/job-resume and scene/client-effects hooks plus shared runtime-plane types, and removed duplicated `JobStoreWriter` declarations from adjacent run/streaming hooks.
  - 2026-03-15: closed the umbrella W=10 item after confirming generated OpenAPI UI contracts are wired through `ui/src/api/generated/*.d.ts` and `ui/src/api/schemas/*`, while `useUiSettings` and the remaining App planes are now composition shells over dedicated connection/run/client/team/data/runtime hooks.
  - 2026-03-15: split `useBrokerTeamRunCreateState.ts` into dedicated create-request and runtime-members hooks, and split `useBrokerTeamRunControlState.ts` into dedicated lookup/recent-runs, moderator, and approvals hooks while preserving the existing broker team-run panel contract and Playwright coverage.
  - 2026-03-07: extracted team event cursor parsing and replay-event normalization from `BrokerTeamConsole.tsx` into `ui/src/components/broker/teamEventPrefs.ts`, typed broker agent/deployment state in the console, and removed additional `any` payload handling from broker event rendering.
  - 2026-03-08: extracted `App.tsx` shell pieces into dedicated components (`AppHeader`, connection banner, tools sidebar, team hub card, advanced panel host), reducing `App.tsx` from 3307 to 2982 lines without changing broker/workflow UI behavior.
  - 2026-03-08: extracted broker team run/chat/guidance/query/queue state from `App.tsx` into `ui/src/hooks/useTeamChatOrchestration.ts`, reducing `App.tsx` from 2982 to 2324 lines while keeping the broker/workflow Playwright batch green.
  - 2026-03-08: extracted the session/admin/query plane into `ui/src/hooks/useAppDataPlane.ts` and tightened the remaining shell wiring, reducing `App.tsx` from 2324 to 1980 lines; UI build plus the 11-test broker/workflow Playwright batch stayed green.
  - 2026-03-08: extracted direct run execution/queue logic into `ui/src/hooks/useRunExecution.ts` and trace state into `ui/src/hooks/useTraceLookup.ts`, reducing `App.tsx` from 1979 to 1742 lines; UI build plus the 11-test broker/workflow Playwright batch stayed green.
  - 2026-03-08: extracted the runtime plane into `ui/src/hooks/useRuntimePlane.ts` so run-watch persistence, scene state/apply, job resume, client-RPC replay, and artifact acknowledgements no longer live in `App.tsx`; `App.tsx` dropped from 1742 to 1066 lines and the same 11-test broker/workflow Playwright batch stayed green.
  - 2026-03-14: extracted `useUiSettings` into `useUiConnectionSettings.ts`, `useUiRunSettings.ts`, and `useUiClientSettings.ts`, shrinking the composition shell from 1628 to 36 lines; UI build plus direct/broker Playwright coverage stayed green.
  - 2026-03-14: extracted `SettingsDrawer` moderator, diagnostics, and sessions panes into dedicated section components, reducing `SettingsDrawer.tsx` from 1905 to 941 lines; UI build plus direct/broker Playwright coverage stayed green.
  - 2026-03-14: extracted `SettingsModeratorSection.tsx` publish/events/pins concerns into dedicated subcomponents plus shared moderator utilities, reducing the section from 872 lines to a thin shell and removing duplicated moderator event-summary logic from `SettingsDrawer.tsx`.

- [x] W=14 — Orchestrator ownership + lease takeover: prevent split-brain while keeping automation always-on.
  - 2026-02-26: added `expected_owner` guard to orchestrator run update/heartbeat and orchestrator loop claim via `meta.orchestrator_owner`.
  - 2026-02-26: orchestrator loop can take over stale/missing leases when `allow_takeover=true`.
  - 2026-02-26: orchestrator console surfaces lease/owner; compose smoke asserts takeover behavior.
- [x] W=13 — Autonomous orchestrator loop: goal contract + drift guard + role handoff events + allocator, with evidence tests and SSE surface.
  - 2026-02-25: drafted `docs/spec/autonomous_orchestrator_v0.md` (goal guard + dynamic allocation model).
  - 2026-02-25: WebUI runtime member quick-add can allocate connected agents by role plan.
  - 2026-02-25: broker runtime member allocator endpoint added (role-based allocation + tests).
  - 2026-02-26: team runs support `auto_allocate_roles` + max members (broker + WebUI).
  - 2026-02-25: broker goal update endpoint added with SSE events and run payload persistence.
  - 2026-02-25: broker role handoff endpoint added with SSE event and run payload persistence.
  - 2026-02-25: WebUI team run status panel surfaces goal contract/events and handoff events with emit actions.
  - 2026-02-26: broker events are persisted + replayable; remaining work is autonomous loop + drift checkpoints.
  - 2026-02-25: added `agentd-orchestrator` loop (heartbeat + dispatch + missing-role spawn requests + drift/progress + handoff queue).
  - 2026-02-26: orchestrator spawn requests support per-role counts + per-role requirements overrides.
  - 2026-02-25: added compose smoke for orchestrator loop (`tests/broker_orchestrator_loop_compose_smoke.sh`).
  - 2026-02-25: added compose smoke for drift/progress/handoff events (`tests/broker_orchestrator_loop_events_compose_smoke.sh`).
  - 2026-02-26: added `tools/run_autonomous_devstack.sh` to run orchestrator + spawn adapter together.
  - 2026-02-26: added runtime member retire policy (orchestrator meta-driven, terminal runs).
  - 2026-02-26: orchestrator loop attempts allocator-backed runtime member allocation for missing roles (meta tracking + tests).
  - 2026-02-26: orchestrator loop dispatches handoff directives to target roles (retriable on missing sessions).
  - 2026-02-26: added unit tests for goal progress/drift event emission and meta updates.
- [x] W=12 — Durable orchestration state + event replay: persist orchestrator runs (DB + CRUD), add replayable event log for team run/goal/handoff/moderator events, and rehydrate WebUI on refresh without losing context.
  - 2026-02-26: broker persists events and exposes `/v1/events/replay`; UI rehydrates on refresh (paging still capped).
  - 2026-02-26: WebUI broker console + team console replay events on refresh; orchestrator panel also rehydrates from replay.
  - 2026-02-26: broker orchestrator runs persisted with CRUD + heartbeat endpoint.
  - 2026-02-26: WebUI team console includes orchestrator run panel (create/list/update/heartbeat).
  - 2026-02-26: orchestrator run responses include lease status derived from heartbeat.
  - 2026-02-25: added compose smoke for orchestrator runs (create/list/update/heartbeat) via `tests/broker_orchestrator_runs_compose_smoke.sh`.
  - 2026-02-26: broker console replay cursor persists via client prefs (fallback localStorage).
  - 2026-02-26: orchestrator loop events smoke now verifies replay for goal progress/drift + handoff events.
- [x] W=12 — Workflow rehydration beyond localStorage: persist workflow wait/resume state in broker/agentd and restore on WebUI reconnect.
  - 2026-02-26: workflow waits persist to client prefs (broker/agentd) with local fallback.
- [x] W=12 — Refresh-safe run streaming: persist active job stream state in client prefs (broker/agentd) and auto-resume after reload.
  - 2026-02-26: localStorage-based job resume exists; extend to server prefs + merge strategy.
  - 2026-02-25: server-backed run watch prefs merged with localStorage; throttle server updates for cursor persistence.
- [x] W=11 — Autonomous ops stack defaults: orchestrator + spawn adapter as first-class services (compose/systemd/launchd) with auto OIDC token refresh, so automation runs without the WebUI.
  - 2026-02-26: added `tools/run_autonomous_devstack.sh` wrapper for dev stacks.
  - 2026-02-26: broker supports `client-auth-allow-automation` for admin client tokens.
  - 2026-02-26: compose overlay `docker/compose.autonomous.yml` + dev client auth file added.
  - 2026-02-26: launchd install/uninstall scripts added for orchestrator + spawn adapter.
  - 2026-02-26: orchestrator/spawn adapter can read `BROKER_OIDC_TOKEN_FILE`; added `tools/oidc_token_refresh.sh`.
  - 2026-02-26: compose smoke added for OIDC refresh sidecar (`tests/broker_oidc_refresh_compose_smoke.sh`).
- [x] W=12 — User guidance/override lane: persist operator “guidance” events, deliver to orchestrator + active agents, and surface in WebUI with ack/receipt so rare human intervention re-aligns autonomy without breaking flows.
  - 2026-02-26: drafted `docs/spec/user_guidance_lane_v0.md` (guidance events + ack + replay).
  - 2026-02-26: broker storage + endpoints + SSE events for guidance lane.
  - 2026-02-26: orchestrator polls open guidance + auto-acks targeted items.
  - 2026-02-26: orchestrator forwards guidance to active members via moderator directives when targets include roles/members/agents.
  - 2026-02-26: orchestrator processes unscoped guidance (no team_run_id) by applying it to the active run.
  - 2026-02-26: WebUI team console guidance panel (list/create/ack).
  - 2026-02-26: WebUI guidance panel consumes SSE/replay events for live refresh.
  - 2026-02-26: added guidance receipts list endpoint + WebUI receipt detail view.
  - 2026-02-25: broker orchestrator loop compose smoke now validates guidance create/ack + replay events.
  - 2026-02-26: orchestrator loop compose smoke validates auto-ack for orchestrator-targeted guidance.
- [x] W=9 — Agent provisioning hooks: define an optional `agent_spawn` interface (pluggable adapters for local/remote spawn) so the orchestrator can request new runtime members when capacity is low.
  - 2026-02-25: added broker spawn request persistence + events (`/v1/teams/{team_id}/orchestrator/spawn_requests`).
  - 2026-02-26: added `agentd-spawn-adapter` CLI + `docs/spec/agent_spawn_adapter_v0.md` (local adapter contract).
  - 2026-02-26: added compose smoke for spawn adapter (`tests/broker_spawn_adapter_compose_smoke.sh`).
  - 2026-02-26: added `expected_status` claim guard to prevent double-claim races.
  - 2026-02-26: spawn adapter supports allocator mode (`SPAWN_ALLOCATOR=1`) using broker runtime member allocation.
- [x] W=11 — Team shared memory scope enforcement (read-only/read-write) wired through team runs + tool policy hooks + tests.
  - 2026-02-25: agentd run request supports `memory_scope_id` + `memory_scope_mode` with scoped memory roots.
  - 2026-02-25: host memory tools enforce read_only mode (write tools rejected, read tools allowed).
  - 2026-02-25: broker injects shared memory scope/mode into member runs; status surfaces shared memory fields.
  - 2026-02-25: WebUI team settings include shared memory mode control; OpenAPI/docs updated.
  - 2026-02-26: added broker compose smoke for shared memory read_only enforcement.
- [x] W=10 — Role handoff execution: emit `team_handoff` events and visualize role graph in WebUI.
  - 2026-02-25: WebUI team run status panel can emit and display handoff events (role graph visualization still pending).
  - 2026-02-26: WebUI role plan editor now renders a role graph preview.
  - 2026-02-26: Team run status panel now renders role graph previews from run metadata.

- [x] W=11 — Automation mode profile + moderator control plane: explicit profiles in caps, per-run override, moderator directives, and nonblocking UX.
  - 2026-02-25: added automation profiles in `/api/v1/caps`, per-run `automation_profile` override, and `effective_automation_profile` response field (agentd + docs + smoke test).
  - 2026-02-25: WebUI run settings + workflow defaults now include `automation_profile` with caps-driven options.
  - 2026-02-25: session audit records include `effective_automation_profile`; WebUI history shows applied run settings.
  - 2026-02-25: added moderator directive/task endpoints + caps feature; WebUI moderator panel publishes nonblocking events.
  - 2026-02-25: Moderator directives accept assignees/scope; WebUI exposes assignee/scope inputs and docs clarify broadcast defaults.
- [x] W=9 — Rolling memory consolidation v1: scheduled rollups (daily/weekly), cross-run correlation index, and evidence-linked recall.
  - 2026-02-24: added scheduled recap engine (daily/weekly) with config/env/caps plumbing; recap files now carry `kind` and `evidence_sources`.
  - 2026-02-25: added correlation index build endpoint + index-backed trace correlation (daily + recap evidence), rebuilt after recaps/consolidation.
- [x] W=6 — WebUI memory recap controls: schedule knobs + recap list view/filter by `kind`.
  - 2026-02-25: MemoryPanel includes schedule load/apply and recap list filtering with kind tagging.
- [x] W=8 — Real-provider automation smokes: DeepSeek + Kimi agent runs using ~/.env keys to validate full automation defaults.
  - 2026-02-25: mac-local provider tests passed (DeepSeek reasoner + Moonshot/Kimi) via `tools/verify_mac_local_stack.sh` with `MAC_LOCAL_PROVIDER_TEST=1` (log: `out/mac_local_provider_tests_2026-02-25_004102.log`).
  - 2026-02-25: mac-local provider tests re-verified (log: `out/mac_local_provider_tests_2026-02-25_021632.log`).
  - 2026-02-24: mac-local provider tests passed with DeepSeek + Kimi (log: `out/mac_local_provider_tests_2026-02-25_024211.log`).

Weight = Impact (1-5) * Urgency (1-5) / Effort (1-5). Higher is sooner.

- [x] W=10 — Orchestrator goal/role plan versioning: persist revision history in run meta and emit SSE events for replay.
  - 2026-02-26: broker stores `goal_versions` + `role_plan_versions` with version counters and emits
    `orchestrator_goal_revision` / `orchestrator_role_plan_revision` events; UI event filter updated.
- [x] W=10 — Populate OpenRouter streaming pins with a verified key, commit `ref/openrouter/streaming_pins.json`, and tighten smoke tests to prefer pins (unblocks streaming stability work).
  - 2026-03-15: key from `~/.env` now passes `tools/openrouter_auth_debug.sh` with `chat_status=200`, both `agentd_openrouter_stream_assistant_smoke` and `agentd_openrouter_stream_tool_call_smoke` pass live with `bytedance-seed/seed-1.6-flash`, and `ref/openrouter/streaming_pins.json` is now populated from that verified result.
  - 2026-02-26: OpenRouter streaming smokes now iterate pinned model lists in order (with candidate logging), and `tools/openrouter_auth_debug.sh` reports pin metadata.
  - 2026-02-19 check: key from `~/.env` returns 401 “User not found”; `OPENROUTER_HTTP_REFERER`/`OPENROUTER_X_TITLE` not set.
  - 2026-02-19 check: setting `OPENROUTER_HTTP_REFERER=http://localhost` and `OPENROUTER_X_TITLE=agentd` still returns 401 “User not found”.
  - 2026-02-19 check: `tools/probe_openrouter_stream_models.sh` fails with 401 chat auth even with headers set.
  - 2026-02-20 check: `tools/openrouter_auth_debug.sh` with key from `~/.env` still returns 401 “User not found” on `/chat/completions`.
  - 2026-02-20 check: even with `OPENROUTER_HTTP_REFERER=http://localhost` and `OPENROUTER_X_TITLE=agentd`, `/chat/completions` returns 401 “User not found”.
- [x] W=11 — First-class workflow schedules (cron + timezone) with durable persistence, agentd API, and WebUI controls.
  - 2026-03-08: closed the stale roadmap item; schedules were already implemented in DB/agentd/WebUI/docs, and now have explicit daemon smoke coverage plus a Playwright UI regression spec.
- [x] W=9 — Broker Team Console UX compaction: full-width panel layout + compact member builder to eliminate excessive vertical scroll in team setup/members views.
  - 2026-03-04: compacted team member/setup grids, added list scroll bounding, and widened broker panel layout.
- [x] W=7 — Split `ui/src/components/broker/BrokerTeamConsole.tsx` into smaller subcomponents (<2000 LOC) while preserving behavior.
  - 2026-03-04: extracted setup/members panels and shared SectionCard component.
- [x] W=8 — Nanoclaw feature leverage plan: assess channel skill registry, container-isolated tool runtime, per-group workspace/memory layout, and scheduled-task loop; map what fits agentd/broker/WebUI and capture integration steps.
  - 2026-03-04: drafted `docs/spec/nanoclaw_leverage_v0.md` and linked it from the spec index.
  - 2026-03-04: expanded leverage plan with mount allowlist, IPC auth, group queue concurrency, secrets handling, and skills-engine apply flow.
- [x] W=7 — Skill transform pipeline: add manifest schema, apply script, and docs for auditable repo changes.
  - 2026-03-04: added `tools/skills/apply_skill.py`, `tools/skills/schema.json`, and usage docs in `tools/skills/README.md` + `docs/OPERATIONS.md`.
  - 2026-03-04: added `tools/skills/preview_skill.py` to apply skills in a temp worktree and capture diffs.
  - 2026-03-04: added `tools/skills/validate_manifest.py` and a sample skill template.
  - 2026-03-04: added `tools/skills/skill_status.py` to list applied skills from state.
  - 2026-03-04: drafted `docs/spec/skills_system_v0.md` and linked it from the spec index.
  - 2026-03-04: added `tools/skills/create_skill.py` and ignored `tools/skills/local/`.
- [x] W=9 — Sandbox mount allowlist: implement external allowlist + blocked patterns for sandboxed tool mounts, with caps/diagnostics surfacing.
  - 2026-03-04: drafted `docs/spec/tool_sandbox_mount_allowlist_v0.md` and linked it from the spec index.
  - 2026-03-04: added `tools/create_mount_allowlist.py` to scaffold a default allowlist file.
  - 2026-03-04: added allowlist loader + diagnostics/config surfaces.
  - 2026-03-04: added `mount_allowlist_validate` helper + test coverage for blocked patterns, root checks, and container prefix gating.
  - 2026-03-04: added `/api/v1/sandbox/mount_validate` endpoint + OpenAPI docs for operator validation.
  - 2026-03-14: wired runtime enforcement into AVM capsule execution (`/api/v1/avm/capsule_run` + workflow `avm_capsule`) and added smoke coverage for allowed + rejected mounts.
  - 2026-02-25 check: `tools/probe_openrouter_stream_models.sh` still skips with chat 401; `tools/openrouter_auth_debug.sh` reports `chat_status=401` and `User not found` (key source `~/.env`).
  - 2026-02-19 check: `tools/openrouter_auth_debug.sh` shows `/models` ok (models_count=337) but `/chat/completions` returns 401 “User not found”.
- [x] W=9 — Drift response policy: allow `drift_action=guidance` to emit a guidance item for operator intervention, with default human target and tests.
  - 2026-02-26: orchestrator drift action emits guidance items; docs + tests updated.
- [x] W=9 — Unblock macOS full-stack compose verification (document Docker Desktop resource settings + prebuilt image path) and improve `tools/verify_mac_full_stack.sh` diagnostics for `unpigz/runc` failures.
- [x] W=8 — Finish embedded/MCU-compatible tool plugin path (ABI constraints + host/sandbox policy; Windows parity tests added).
- [x] W=7 — Split near-2000 LOC modules into SOLID units with focused tests: `broker/internal/broker/server.go` (UI `api.ts` + `daemon/src/agent_db.cpp` + `daemon/src/workflow_endpoints.cpp` completed).
- [x] W=3 — Reduce WebUI bundle size warning (>500 kB) via `manualChunks` or dynamic imports where appropriate.
- [x] W=5 — Audio streaming foundation: define WebRTC/Opus signaling + broker relay endpoints (spec + broker relay endpoints + loopback + docker-postgres smoke tests + agentd loopback tool/test added).
- [x] W=12 — Multi-agent team orchestration: define agent group model (roles, shared memory, quorum gating), add APIs + WebUI flows + smoke tests.
  - 2026-02-19: Draft spec added (`docs/spec/team_orchestration_v0.md`) describing data model + quorum semantics.
  - 2026-02-19: Added run-event payload schemas + fixtures for team handoff/quorum/member results.
  - 2026-02-19: Added broker OpenAPI shapes for team/membership/quorum endpoints (planned).
  - 2026-02-19: Added broker DB tables + CRUD handlers for teams/members/quorum (team runs still stubbed).
  - 2026-02-20: Team run execution implemented (sync fan-out + DB run records).
  - 2026-02-20: Added unit coverage for team run fan-out executor + role filtering; still need end-to-end smoke.
  - 2026-02-20: Added compose smoke for team run fan-out + role filter (`tests/broker_team_runs_compose_smoke.sh`).
  - 2026-02-20: Added quorum gating compose smoke (`tests/broker_team_runs_quorum_compose_smoke.sh`).
  - 2026-02-20: Persisted team run approvals in broker + approvals endpoints.
  - 2026-02-20: Emit `team_quorum_request` + `team_quorum_result` broker SSE events on run creation and approval updates.
  - 2026-02-20: Added broker SSE smoke for quorum events (`tests/broker_team_quorum_events_sse_compose_smoke.sh`).
  - 2026-02-19: Connector now supports `--local-agentd-token` (or `AGENTD_AUTH_TOKEN`) so team run fan-out can authenticate to local agentd; compose updated and compose smoke verified.
  - 2026-02-19: Broker now enforces `team_run` quorum rules on run creation via inline approvals (`team.approvals`) with strict failures returning `409`.
  - 2026-02-24: WebUI broker console now includes team console (teams/members/quorum) plus team runs + approvals UI.
  - [x] 2026-02-20: Add quorum enforcement smoke.
  - 2026-02-24: Added per-member backend profiles (member meta + explicit run overrides), UI inputs for backend roles, and broker allowlist enforcement + tests.
  - 2026-02-24: Added runtime members for team runs (ephemeral members per run), with broker validation, OpenAPI/docs, and WebUI support.
  - 2026-02-24: WebUI runtime member preview/removal + run status display; OpenAPI run status updated.
  - 2026-02-24: WebUI runtime member builder now supports broker agent/deployment pickers.
  - 2026-02-24: Runtime member builder auto-selects first connected deployment for chosen agent.
  - 2026-02-24: Runtime member builder can bulk-add connected agents (excluding existing team/runtime).
  - 2026-02-24: Added seed button to populate explicit member overrides from team member meta.
  - 2026-02-24: Added "Save to team" action to persist runtime members into the team registry.
  - 2026-02-24: Runtime member save preview now shows new/skipped/invalid counts.
  - 2026-02-24: Runtime member save preview now lists skipped/invalid entries with reasons.
  - 2026-02-24: Added helper to fix invalid runtime members (drop missing agent_id, fill missing role).
  - 2026-02-25: Added per-role run overrides (`team.role_overrides`) with broker allowlist enforcement + WebUI run panel support.
  - 2026-02-25: Team settings now persist `meta.role_overrides`, and broker applies it when runs omit overrides.
  - 2026-02-25: Added compose smoke for role override defaults + member precedence.
  - 2026-02-24: Runtime agent list auto-refreshes when team console is active.
  - 2026-02-24: Runtime members can be paused/resumed from the preview list.
  - 2026-02-24: Added pause/resume-all controls for runtime members.
  - 2026-02-24: Added remove-paused action for runtime members.
  - 2026-02-24: Added runtime members JSON compaction helper.
  - 2026-02-24: Added copy-to-clipboard for runtime members JSON.
  - 2026-02-24: Added import JSON file picker for runtime members.
  - 2026-02-24: Added download JSON for runtime members.
  - 2026-02-24: Added merge toggle for runtime members JSON import.
  - 2026-02-24: Added export team members to runtime JSON.
  - 2026-02-24: Added runtime vs team diff summary in runtime preview.
  - 2026-02-25: Refactored Team console run UI into `BrokerTeamRunPanel` to keep components SOLID and <2000 LOC.
  - 2026-02-25: Team members editor adds agent/deployment pickers and bulk add connected agents.
  - 2026-02-25: Team settings editor added (display name, tags, policy ref, shared memory scope, meta JSON).
  - 2026-02-25: Team member rows add pause/resume status toggles.
  - 2026-02-25: Team members list adds bulk pause/resume controls.
  - 2026-02-25: Team members list adds bulk remove-paused control.
  - 2026-02-25: Team runs now persist per-member session_id mappings; run status includes `member_sessions` and runtime member updates extend mappings.
  - 2026-02-25: Tool quorum rules now inject `policy_approval_rules` + `team_id` into member runs (distinct-role + best_effort support in agentd approvals).
  - 2026-02-25: Added broker team-run moderator broadcast endpoints (directive/task) + OpenAPI/docs updates.
  - 2026-02-25: WebUI team run panel now publishes moderator directives/tasks with target filters; session mappings visible in run status.
  - 2026-02-25: Added team-run moderator events aggregation endpoint + OpenAPI/docs.
  - 2026-02-25: WebUI team run moderator panel can load aggregated moderator events (filters + JSON view).
  - 2026-02-25: Team member inline edit panel added (role/status/weight/caps/meta + agent/deployment).
  - [x] 2026-02-25: Added runtime member updates for team runs (replace/merge) + WebUI panel + broker API/event.
  - [x] 2026-02-25: Added compose smoke for team run runtime member updates.
  - [x] 2026-02-25: Added SSE compose smoke for runtime member update events.
  - [x] 2026-02-25: Added async team runs (`team.mode=async`) with `run_async` fan-out, persisted member job metadata, and status reconciliation on lookup (Broker + WebUI + OpenAPI/docs).
  - [x] 2026-02-25: Add async team run cancellation (broker fan-out `/api/v1/job/cancel`) + aggregated member job summary (broker/WebUI/docs + unit test).
  - 2026-02-25: Added team run list endpoint + WebUI recent runs list (live SSE updates, no polling) for nonblocking monitoring.
  - 2026-02-25: Added `team_run_created` + `team_run_status` broker SSE events to drive live run list refresh.
  - 2026-02-25: Added compose smoke for team run created/status SSE events.
  - 2026-02-25: Drafted `docs/spec/orchestrator_console_v0.md` for WebUI role/back-end orchestration UX.
  - 2026-02-25: Added role instructions + prompt composition (`role_instructions`, `role_prompt_mode`) with `{{goal}}` templating in broker team runs.
  - 2026-02-25: WebUI team settings now includes a role plan editor (role instructions + role graph) and run builder supports role prompts.
  - 2026-02-25: Team run lookup now persists per team with auto-resume toggle; Team run UI split into subpanels to keep files SOLID.
- [x] W=9 — Approval queues + tool-level quorum gating: WebUI approval queue, tool-level quorum enforcement, and SSE updates for approval state.
  - 2026-02-24: Added agentd approval DB tables, approval gate, approval APIs, and approval run events.
  - 2026-02-24: Added approval event schemas + fixtures + spec tests.
  - 2026-02-24: Added WebUI approval queue panel + docs updates.
- [x] W=7 — Enforce approval role constraints once member-role identity is available in agentd approvals.
  - 2026-02-24: approvals decisions accept `member_role`, enforce role allowlists, and surface role in events/UI; approvals endpoints wired into agentd main + AgentdApi with prefix routing.
- [x] W=6 — Finalize WebUI inline run approvals in `ui/src/components/broker/BrokerTeamConsole.tsx` (tests + docs + commit).
  - 2026-02-24: added Playwright broker inline approvals smoke + WebUI doc note.
- [x] W=5 — WebUI refresh-safe workflow waits: persist active workflow wait state and resume polling after reload.
- [x] W=8 — Run comparison + evidence diff UX: side-by-side run diffs (events/artifacts/costs), evidence bundle viewer, and regression baselines.
  - 2026-02-25: Added WebUI Run diff panel using `/api/v1/run/replay` with client-side request/response/tool-record diffs, baseline shortcut, and docs/spec updates.
  - 2026-02-25: Added DB-backed event/artifact diffs via `/api/v1/db/run`, attestation viewing, and usage deltas.
- [x] W=10 — Policy hook MVP: deterministic pre/post run + tool call hooks with allow/deny + budget caps, config surface, and audit logs.
- [x] W=9 — Attestation bundles: canonical hash format + signed run certificates + verification CLI.
  - 2026-02-19: Added draft spec (`docs/spec/run_attestation_bundle_v1.md`) + host tool (`run_attestation_bundle_tool`) + smoke test.
  - 2026-02-19: Added server-side HMAC/Ed25519 signing for `/api/v1/run/attestation` + Ed25519 verification smoke.
  - 2026-02-25: Verified `tests/run_attestation_bundle_tool_smoke.sh` + `tests/agentd_run_attestation_ed25519_smoke.sh` (logs in `out/`).
- [x] W=8 — Evaluation + regression gating: canonical eval packs, deterministic scoring, and CI baselines to catch model/version drift.
  - 2026-03-15: Added canonical tracked baselines under `ref/eval_packs/`, normalized baseline writes in `tools/eval_pack.py`, default baseline gating in `tools/verify.sh`, CI baseline checks for self-contained smoke packs, and smoke coverage for baseline compare/update behavior.
  - 2026-03-15: Made scenario/eval packs devstack-aware by default via `out/devstack_state.json`, added canonical tracked baselines for `basic_agentd_smoke` and `broker_smoke`, and added a baseline refresh helper.
  - 2026-03-15: Unified canonical eval-pack set selection behind `tools/run_eval_pack_set.sh`, wired it into `tools/verify.sh`, CI, and baseline refresh, and added smoke coverage for set planning/failure semantics.
  - 2026-03-15: Fixed `tools/devstack_oidc_token.sh` issuer preservation for devstack broker auth and added a canonical live eval pack for the broker OIDC + `X-Agentd-Authorization` proxy/session boundary.
  - 2026-02-25: Added eval pack baseline compare/update flags to `tools/eval_pack.py` with spec + ops docs.
  - 2026-02-19: Added eval pack spec (`docs/spec/eval_pack_v0.md`) and runner (`tools/eval_pack.py`) with a minimal example pack.
  - 2026-02-19: Added `eval_pack_smoke` test (self-contained, no agentd required).
  - 2026-02-19: Added CI workflow to run `tools/eval_pack.py` against a self-contained eval pack.
  - 2026-02-19: Added `tools/verify.sh --eval-pack` to run eval pack smoke locally.
  - 2026-02-19: Added richer eval checks (json_number/json_len/file_sha256) + eval_pack_checks_smoke.
  - 2026-02-19: Added broker_smoke eval pack (health/ready + evidence) and templated scenario ports.
- [x] W=9 — Drift remediation actions: pause/cancel policies after drift, with audit fields and docs/tests.
  - 2026-02-26: drift_action supports guidance/pause/cancel with meta audit fields + tests.
- [x] W=8 — Drift replanning workflows: orchestrator-driven goal revision after drift, with approval hooks and evidence.
  - 2026-03-15: replan ack now patches the active team run goal contract when staying in-place, records deferred role-plan evidence when a new run is still required, and emits richer `replan_resume` evidence with previous + revised goal/contract/role-plan fields.
  - 2026-02-26: added `drift_action=replan` to pause and emit replan guidance, auto-resume after ack, and support optional new run + goal overrides.
  - 2026-02-26: added receipt-based approval thresholds for replan resume (`replan_ack_*`).
  - 2026-02-26: emit `replan_resume` goal event with receipt summary for evidence.
  - 2026-02-26: allow `replan_resume` goal events and SSE `team_goal_replan_resume`.
  - 2026-02-26: capture `replan_prev_goal`/`replan_prev_goal_contract`/`replan_prev_role_plan_snapshot` and include `prev_goal` + `goal` in replan resume events.
- [x] W=10 — Automouse defaults pack: ship a first-class "automouse" config bundle (automation_profile=full, orchestrator + spawn adapter auto-run, preflight checks) so full power is the default without manual setup.
  - 2026-02-26: added automouse compose overlay + WebUI preset config + `tools/automouse_pack.sh`, and extended compose verification with `COMPOSE_AUTOMOUSE`.
- [x] W=9 — Operator briefing payloads: emit structured "re-entry" summaries (goal, drift evidence, proposed changes) on guidance/replan so rare user engagement is low-friction.
  - 2026-02-26: drift guidance/replan payloads now include `briefing` with goal/contract/role plan, drift metrics, and proposed changes when available.
- [x] W=9 — Goal + role plan versioning: add versioned diffs for goal/role plan changes with replayable events to reduce drift and improve audits.
  - 2026-02-26: added goal/role revision history with diff keys, replayable events, SSE payloads, and WebUI revision views.
- [x] W=8 — Orchestrator modularization: split `agentd-orchestrator` loop into SOLID modules (scheduling, allocation, drift, guidance) to keep files <2000 LOC.
  - 2026-03-15: extracted drift/replan, runtime allocation/handoff/retire, run lifecycle/lease progression, bootstrap/loop control, and broker API surfaces into dedicated modules; `broker/cmd/agentd-orchestrator/main.go` is now a tiny entry shell.
- [x] W=8 — Capacity-based autoscale: spawn or retire runtime members based on backlog/latency signals (not only missing roles).
  - 2026-03-15: added opt-in `capacity_autoscale` heuristics in the orchestrator loop using `member_job_summary` / `member_jobs` pressure, allocator-first scale-out, spawn fallback, and idle duplicate-runtime-member retirement with package tests + spec updates.
- [x] W=9 — Scheduling + isolation MVP: admission control, per-run budgets, and tool execution caps with evidence logs.
  - 2026-03-15: verified submit-time admission control, durable `workflow_limits` enforcement, per-attempt cap clamping, and emitted `workflow_budget_exceeded` evidence via source + smoke coverage (`agentd_workflow_admission_control_smoke`, budget smokes, and `agentd_workflow_budget_events_smoke`).
- [x] W=8 — Data governance controls: retention policy config, export/erase endpoints, and redaction-aware evidence bundles.
  - 2026-03-15: verified retention enforcement, session erase cascade, analytics export endpoints, and redacted replay/attestation evidence with dedicated host smokes (`agentd_memory_retention_smoke`, `agentd_session_delete_governance_smoke`, `agentd_db_analytics_export_governance_smoke`, `agentd_run_replay_smoke`, and `agentd_run_attestation_ed25519_smoke`).

## New tasks (2026-02-19)

- [x] Add regression tests for `/api/v1/file` path traversal and session-root confinement.
- [x] Refactor `daemon/src/run_request.cpp` into smaller units (parsing + config + persistence + tool-loop wrapper extracted).
- [x] Add symlink containment option for `/api/v1/file` (realpath confinement with session_id).
- [x] Refactor `ui/src/components/ConversationView.tsx` into smaller components (approaching 2000 LOC).
- [x] Standardize API error envelopes across agentd + broker (`{"err":"...","code":"...","details":{...}}`) and align WebUI handling.
- [x] Refactor `ref/ds-cli/sophon/src/sophon_cli/cli.py` (5.8k LOC) into SOLID submodules or mark as vendored/read-only.
- [x] Refactor `ref/claude-mem/src/services/sqlite/SessionStore.ts` (2.3k LOC) into smaller units or mark as vendored/read-only.
- [x] Add build/log cleanup automation (e.g., tools/clean.sh + `out/` log pruning) to prevent disk bloat.
- [x] Consolidate design docs into a coherent architecture map and component summary structure.
- [x] Add repo size guard to cleanup tooling (`tools/clean.sh --max-repo-gb`) to prevent >37GB bloat.
- [x] Add repo size report tool (`tools/repo_size_report.py`) to pinpoint growth quickly.
- [x] Extend repo size report with largest-file output for rapid bloat diagnosis.
- [x] Add `--exclude-defaults` to repo size report for cleaner audits.
- [x] Add CI repo size guard workflow using `tools/repo_size_report.py`.
- [x] Remove stub CLAUDE.md placeholders from vendored claude-mem subtree.
- [x] Add `tools/clean.sh --purge-ref-git` to optionally delete nested ref/.git dirs.
- [x] Add `tools/clean.sh --report` to emit repo size summary after cleanup.
- [x] Add nested .git detection to repo size report + CI guard.
- [x] Add nested .git listing option to repo size report.
- [x] Add stub file scan tool + CI guard.
- [x] Add tracked file size guard + CI step.
- [x] Add `tools/verify_repo_guards.sh` to run all repo hygiene guards locally.
- [x] Add size override flags/env to `tools/verify_repo_guards.sh`.
- [x] Add repo guard flags to `tools/verify.sh` for one-command verification.
- [x] Add untracked file size guard + integrate into repo guards.
- [x] Allow strict mode via `REPO_GUARD_STRICT` env for repo guards.
- [x] Ignore nested `ref/**/.git` dirs to prevent accidental re-adding.
- [x] Treat `ref/claude-mem/openclaw/test-install.sh` (2.3k LOC) as vendored/read-only; documented in `docs/VENDORED.md`.
- [x] Add vendored guard to fail when `ref/` changes without an explicit override.
- [x] Add optional local git hook installer for vendored guard.
- [x] Add optional local git hook uninstaller for vendored guard.
- [x] Add local hook status helper for vendored guard.
- [x] Add JSON output mode for hooks status helper.
- [x] Add --check mode to hooks status helper.
- [x] Document hooks_status --json --check snippet for CI/local checks.
- [x] Make vendored guard check staged/unstaged changes for local hooks.
- [x] Add optional verbose mode for vendored pre-commit hook output.
- [x] Add --verbose to install_git_hooks.sh for permanent verbose hooks.
- [x] Add quiet mode for vendored guard output (env + flag).
- [x] Document quiet/verbose precedence for vendored hook output.
- [x] Add --quiet to install_git_hooks.sh for permanent quiet hooks.
- [x] Add curated handbook generator (`docs/HANDBOOK.md`) with `tools/build_handbook_bundle.py` + repo-guard sync check.
- [x] Add `docs/OPERATIONS.md` to the handbook summary/source index and keep the handbook under 250 lines (196 lines as of 2026-02-19).
- [x] Add role-based quick paths to the handbook to reduce doc-sifting.
- [x] Add policy hook deny/audit smoke test with a local stub LLM and CTest wiring.
- [x] Add Docker Desktop auto-start + wait in docker preflight with env knobs for timeouts/disablement.

## Promoted goals (explicit goals; no non-goals)

Weights updated 2026-02-19: prioritize **streaming stability** (including verified
OpenRouter pins), **tool plugin isolation** hardening, and **repo hygiene/size-guard
enforcement**; keep **interop/attestation** and **AVM** queued behind those until
streaming and plugins are stable.

- [x] CORS: add cookie-based auth support and per-route origin policies with regex/precedence rules (broker + agentd implemented; tests added).
- [x] Storage/analytics: DB query API as canonical surface, analytics layer, and binary blob storage tiers.
- [x] Tool loop: full multimodal transcript support + stable, versioned event schema with migrations.
  - [x] assistant_message events now strip multimodal prefix and emit `assistant_mm_json` + truncation metadata.
  - [x] user_message events emit multimodal payloads (run events + tools=none path).
  - [x] tools=none assistant_message events emit multimodal payloads + strip prefix (smoke test).
  - [x] DB mirror persists multimodal payloads (`mm_json`) alongside message text; session/db query endpoints surface mm fields.
  - [x] WebUI renders `assistant_mm_json` attachments (images/files) in assistant messages.
  - [x] Core tool-loop test: validates `assistant_mm_json` event payloads for prefix parsing + byte metadata.
  - [x] Run-event fixture validation enforces payload shapes for common event types.
  - [x] Payload JSON Schemas for common run event types (assistant/tool/artifact/usage/error).
  - [x] Job + run event envelopes attach payload schema identifiers for common types.
  - [x] Run-event fixtures now require schema identifiers for common payloads.
  - [x] Workflow + edge workflow event payload schemas + schema tags across event endpoints/streams.
- [ ] Streaming stability: finish verified provider pins and remaining live-provider upkeep on top of the shipped core interface.
  - [x] Core SSE parser (`agent_sse_parser_t`) is shared by CLI/daemon streaming paths.
  - [x] Core stream decoder (`agent_stream_decoder_t`) + unit tests (`tests/test_stream_decoder.c`) are shipped.
  - [x] CLI/daemon streaming is wired through the core decoder, replacing duplicated host logic.
  - [x] Compatibility matrix + core interface spec are present in `docs/STREAMING.md` and `docs/spec/streaming/core_stream_v1.md`.
  - [x] Local deterministic streaming coverage exists via `agent_local_stream_assistant_smoke`, `agent_local_stream_tool_loop_smoke`, and `agentd_local_stream_assistant_smoke`.
  - [x] Live-provider assistant/tool-call streaming smokes exist for DeepSeek, Moonshot, and OpenRouter (key-gated).
  - [x] OpenRouter probe/pin workflow exists; smoke tests already prefer `ref/openrouter/streaming_pins.json` when populated.
  - [ ] Populate OpenRouter streaming pins with a verified key; current available key still returns 401 “User not found” on chat preflight across candidates.
  - [ ] Re-verify live-provider usage/variant coverage when a working OpenRouter key is available, then commit the resulting pins file.
  - 2026-03-15: narrowed the stale umbrella item after re-verifying the shipped core decoder, local stream smokes, and provider-matrix docs.
- [x] Memory architecture: dynamic retention policy (decay/salience/recaps) informed by claude-mem research + docs.
  - [x] Memory retention policy enforcement (daily logs + checkpoints) + endpoint + background engine.
  - [x] Structured memory deprecate pass (policy-driven; bounded) with broker fan-out.
  - [x] Memory salience context + `/api/v1/memory/salience` (deterministic decay, recency + importance).
  - [x] Memory recaps: LLM summaries to `memory/recaps/` + list/generate APIs.
- [x] Memory UX alignment (claude-mem): optional context header + last summary/assistant-message hints + timeline ordering toggle.
  - [x] Index/search headers include token economics + recap hint (latest recap summary).
  - [x] Add assistant-message hint (last assistant summary or salient response) in context headers.
  - [x] Add timeline ordering toggle for memory results (newest-first vs oldest-first).
  - [x] WebUI run settings: pass `memory_context_mode=salience` through to the daemon.
- [x] Tool plugins: sandbox/isolation, Windows loader, and embedded/MCU-compatible plugin path.
  - [x] Tool plugin config JSON support (optional `*_ex` symbols + `--tool-plugin-config`) with smoke coverage.
  - [x] Windows loader for tool plugins (LoadLibrary/GetProcAddress).
  - [x] Sandbox via tool server host (`agentd_tool_plugin_host`) + smoke test.
  - [x] Plugin host resource limits (cpu/wall/as) with best-effort enforcement.
  - [x] Per-plugin policy limits (config JSON) resolved to the most restrictive values.
  - [x] In-process plugin manifest/result caps (1 MiB/4 MiB) to bound memory spikes.
  - [x] Plugin host tool-result cap (4 MiB) enforced before JSON parsing.
  - [x] Plugin host oversized payload smoke coverage (ext_big).
  - 2026-03-15: closed stale roadmap item after verifying in-process plugin loading, out-of-process isolation, embedded/MCU compile-time plugin ABI docs, and Windows loader coverage (`daemon/src/tool_plugins.cpp`, `tests/agentd_tool_plugin*_smoke.sh`, `tests/test_tool_plugin_host_limits.cpp`, `docs/PLATFORM_SUPPORT.md`, `tools/verify_windows_build.ps1`).
- [ ] Audio streaming: replace the shipped managed browser-to-agentd live media runtime with an embedded long-lived agentd-native media service.
  - [x] Broker signaling relay endpoints are implemented: `POST /v1/audio/sessions`, `GET/DELETE /v1/audio/sessions/{id}`, `POST /v1/audio/sessions/{id}/signal`, `GET /v1/audio/sessions/{id}/signal/stream`, plus list/status views for live sessions.
  - [x] Broker audio signaling loopback/durable relay coverage exists via `broker_audio_signal_loopback_smoke` and `broker_audio_signal_docker_smoke`.
  - [x] Agentd loopback audio tool + smoke coverage exist via `agentd_audio_signal_loopback` and `agentd_audio_signal_loopback_smoke`.
  - [x] WebUI open-world voice presentation harness exists via `tests/webui_observe_voice_hello_openworld.sh` and `ui/e2e/observe_voice_hello.spec.ts`.
  - [x] Implement browser-side WebRTC session negotiation/mount in the WebUI broker panel on top of the shipped broker signaling APIs.
  - [x] Close the remaining end-to-end gap with a real agentd media peer path instead of only signaling loopback and mocked-browser negotiation coverage.
  - [x] Add explicit voice session lifecycle/status controls in the WebUI rather than prompt-driven open-world presentation only.
  - [x] Define the minimal agentd API surface for voice control/stats on top of the shipped `ui_action` + `client_event` path.
  - 2026-03-15: narrowed the stale umbrella item after re-verifying broker signaling endpoints, loopback smokes, and the open-world voice observe harness.
  - 2026-03-15: broker panel now exposes explicit voice session create/list/select/send/delete controls with live signal stream inspection, backed by broker audio session status APIs and Playwright regression coverage.
  - 2026-03-15: agentd now exposes `POST /api/v1/session/voice_control` plus `GET /api/v1/session/voice_stats`, and the WebUI runtime auto-runs DB-backed `media_play`/`media_pause`/`media_snapshot` actions through the shared client RPC executor.
  - 2026-03-15: WebUI advanced tools now expose a dedicated Voice panel that drives the shipped agentd voice endpoints and renders durable voice stats, covered by `ui/e2e/voice_panel.spec.ts`.
  - 2026-03-15: WebUI broker panel now drives browser-side WebRTC offer/answer/bye automation with mounted remote audio playback and remote-candidate handling, covered by `ui/e2e/broker_audio_panel.spec.ts` against a deterministic mocked `RTCPeerConnection`.
  - 2026-03-15: `tools/agentd_audio_webrtc_peer.js` plus `tests/agentd_audio_webrtc_peer_smoke.sh` now prove a real browser-to-agentd-side WebRTC RTP path over broker signaling, including answer/candidate relay, inbound RTP stats, and `bye` teardown; the remaining gap is embedding that media peer into an agentd-native runtime surface.
  - 2026-03-15: agentd now manages that host-side media peer through `POST/GET /api/v1/session/voice_webrtc_peer`, with `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` proving start/status, live RTP, and managed stop/teardown against a real broker session.
  - 2026-03-15: extracted the voice-peer runtime manager out of `session_endpoints.cpp`, added explicit backend contract fields (`default_runtime_kind`, `bundled_available`, `builtin_available`, `peer.runtime_kind`), and promoted the voice runtime OpenAPI into reusable components so the remaining native-media gap is now a backend swap instead of an API rewrite.
  - 2026-03-15: the shipped bundled/external voice runtime now persists DB-backed runtime snapshots plus per-session stdout logs, so `GET /api/v1/session/voice_webrtc_peer` can recover running/stopped peer state across agentd restarts and avoid duplicate starts when the peer is still alive.
  - 2026-03-15: shipped a default `bundled` voice runtime tier that auto-discovers the repo-local Node/Playwright peer without `AGENTD_AUDIO_WEBRTC_PEER_TOOL`, while keeping `runtime_kind=external` as the explicit operator-configured override and `runtime_kind=builtin` reserved for the future native media backend.
  - 2026-03-15: `POST /api/v1/session/voice_webrtc_peer` can now auto-create the broker audio session from `broker_agent_id` instead of forcing callers to pre-create `broker_session_id`; runtime status records `managed_broker_session` plus broker ownership metadata, and smoke coverage now proves the caller-free bring-up path.
  - 2026-03-15: the managed bundled/external runtime now persists child-exit state eagerly and lets `action=stop` take `broker_token` so agentd can delete an owned broker audio session itself after an ungraceful peer death that never delivered `bye`; smoke coverage now proves SIGKILL + restart + cleanup fallback.
  - 2026-03-15: `DELETE /api/v1/session?session_id=...&broker_token=...` now treats session erase as a real voice-runtime ownership boundary: it stops the managed peer, clears persisted runtime/log artifacts, and deletes the owned broker audio session; smoke coverage now proves session-delete cleanup end to end.
  - 2026-03-19: if session erase has to stop a live bundled/external peer recovered from persisted running state after daemon restart, `voice_runtime_cleanup.peer` now preserves that peer's explicit terminal signal/result before the runtime record is cleared, and the main runtime smoke proves the recovered-delete path end to end.
  - 2026-03-15: `GET /api/v1/session/voice_webrtc_peer` now self-heals stale local runtime state if the session row disappears outside the normal erase path, and the runtime teardown path now suppresses the detached waiter from re-persisting a cleared runtime record after delete/cleanup.
  - 2026-03-15: managed voice/WebRTC broker defaults are now durable daemon config (`audio_webrtc.{broker_url,broker_token}` via `/api/v1/config/update` or env), safe config/runtime status expose only boolean presence, and the main WebRTC runtime smoke proves start/stop still work after restart without per-request broker credentials.
  - 2026-03-15: managed voice-runtime start now waits for bounded startup confirmation, fails closed if the child exits before ready, cleans up any owned broker audio session created for that failed start, and the WebRTC runtime smoke now proves that path using an intentionally invalid `AGENTD_AUDIO_WEBRTC_PEER_NODE_BIN` override (also fixing that env override to actually win over the built-in `"node"` default).
  - 2026-03-15: the `external` WebRTC backend seam is now durable daemon config too (`audio_webrtc.peer_tool_path`, `audio_webrtc.node_bin`, `audio_webrtc.default_runtime_kind`), daemon startup also honors `AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND`, safe `/api/v1/config` reports that seam without exposing secrets, runtime metadata now exposes default backend source/availability explicitly (`auto|env|config`), and the runtime smoke now proves env-backed default selection plus explicit and defaulted config-backed external launch paths plus config-backed fail-fast startup via an invalid persisted `node_bin`.
  - 2026-03-20: durable/defaulted WebRTC backend policy now accepts `builtin` too, so config/env can intentionally pin the future native media backend and surface `default_runtime_kind=builtin` with explicit unavailable/not-implemented metadata instead of silently self-healing it away.
  - 2026-03-20: if daemon voice-backend policy rotates under a live bundled/external peer, status and running-conflict responses now surface `backend_policy_drift.changed_fields[]` plus `current_effective_start`, and the main runtime smoke proves default-runtime drift plus external `peer_tool_path` / `node_bin` drift explicitly.
  - 2026-03-20: broker-session binding, runtime seeding, and now backend-local refresh/stop/artifact cleanup all share explicit runtime helpers, so the remaining `runtime_kind=builtin` gap is the in-process media implementation rather than duplicated child-process control glue.
  - 2026-03-20: broker audio signal send/stream handling is now shared C++ runtime infrastructure rather than a one-off loopback-tool implementation, so the future builtin backend can reuse native offer/answer/candidate streaming without starting from shell-level wiring.
  - 2026-03-20: native broker signaling now also exposes shared higher-level wait/answer/bye helpers, and the loopback smoke proves agentd-side graceful `bye` closes the broker session, so the future builtin backend has reusable C++ session-negotiation primitives rather than only low-level HTTP/SSE calls.
  - 2026-03-20: typed broker-signal protocol helpers now also exist in C++ for parsed `sender_tag`, offer/answer SDP payloads, ICE candidates, and `bye` payloads, with direct unit coverage plus loopback/runtime regression proof, so the remaining builtin gap is media/peer execution rather than re-deriving signaling JSON semantics.
  - 2026-03-20: native broker signaling now also has a shared C++ signal-session state helper for self-filtering, pending ICE queue/drain behavior, and remote `bye` tracking, with direct unit proof, so the remaining builtin gap is real peer/media execution rather than rebuilding browser-session control rules in C++.
  - 2026-03-20: the native signal client now also exposes session-aware typed ingress streaming plus typed answer/candidate/bye egress helpers, and the loopback tool uses that higher-level session stream instead of raw event parsing, so builtin backend work can consume one parsed signaling surface end to end.
  - 2026-03-20: the native session-aware wait path now also preserves queued pre-offer ICE candidates when stopping on the first remote description, and the loopback smoke proves that early trickle candidate survives through offer acceptance instead of being dropped by the bootstrap helper.
  - 2026-03-20: the native signal client now also has stateful session-ingress wait helpers for continuing past remote-description bootstrap, and the loopback smoke proves both pre-offer queued ICE and later post-answer ready ICE on the same shared C++ session state.
  - 2026-03-20: native signaling now also proves remote-side close handling on that same shared session state; the dedicated remote-bye smoke covers pre-offer ICE, post-answer ICE, and a browser-side `bye` reason through the C++ helper path.
  - 2026-03-20: the full native “answer a remote offer” sequence is now a shared C++ negotiation helper instead of loopback-tool glue, so future builtin/backend work can reuse one control surface for bootstrap, answer, post-answer ICE, remote `bye`, and optional local `bye`.
  - 2026-03-20: that shared answer-exchange helper now also has direct unit proof through injectable ops, so sequencing regressions no longer require a live broker smoke to catch.
  - 2026-03-20: reserved `runtime_kind=builtin` voice starts now still run the same request validation plus non-mutating borrowed broker-session preflight as the launchable backends, and the main runtime smoke proves builtin missing-session and mixed-mode failures happen before the final not-implemented `501`.
  - 2026-03-20: valid reserved builtin voice starts now also return a structured `builtin_start_contract` with broker-session mode/details, signaling identity, timing knobs, and explicit deferred broker mutation semantics, with direct unit proof plus runtime smoke coverage.
  - 2026-03-15: agentd now self-heals a corrupted persisted `audio_webrtc.default_runtime_kind` back to `auto` during runtime-config load, rewrites the SQLite runtime-config record, and the WebRTC runtime smoke proves bundled fallback behavior after direct DB corruption.
  - 2026-03-15: `voice_webrtc_peer` now treats `runtime_kind` as start-only, so stop requests ignore mismatched or invalid selector values; the main runtime smoke now proves both active-stop and no-runtime stop behavior with ignored selectors.
  - 2026-03-15: stop/session-delete cleanup now validates broker tokens lazily, so malformed configured defaults no longer block teardown of borrowed broker-session runtimes; the main runtime smoke now proves both stop and delete under that failure mode.
  - 2026-03-15: local stop/session-delete teardown for managed voice runtimes now also survives owned broker-session deletion failure, reporting `broker_session_deleted=false` plus `broker_session_delete_error` instead of aborting local cleanup; the main runtime smoke now proves both stop and delete under that failure mode.
  - 2026-03-15: corrupt persisted `session.voice_webrtc_peer.*` records now self-heal on status/start by clearing the bad DB record and stale local artifacts, with the main runtime smoke proving recovery instead of a hard `500`.
  - 2026-03-15: stale persisted `session.voice_webrtc_peer.*` records that still claim `running=true` after a dead daemon restart now self-heal on status/stop/start too, clearing the stale DB record plus local artifacts instead of surfacing a fake recovered peer.
  - 2026-03-20: voice-runtime refresh is now backend-aware, and the main runtime smoke proves that impossible persisted `runtime_kind=builtin` running records self-heal through the same `cleanup_on_stale_record` status/stop/start path instead of relying on child-process PID behavior.
  - 2026-03-15: `voice_webrtc_peer action=stop` now reports `reason=not_running` when the managed peer already exited, while still attempting owned broker-session cleanup instead of claiming a false-positive active stop.
  - 2026-03-19: if a still-live bundled/external peer is recovered from persisted running state after agentd restart, a later managed stop now preserves an explicit terminal signal/result in the persisted runtime snapshot instead of flattening that recovered peer into a generic stopped record.
  - 2026-03-15: running `voice_webrtc_peer` starts are now only idempotent when the effective resolved runtime config still matches the live runtime; conflicting explicit, config-driven, or now-unlaunchable starts now return `409` with the existing peer snapshot, and the main runtime smoke proves persisted-running `runtime_kind=builtin`, config-driven default-backend, missing-tool-path, invalid-`node_bin`, and in-memory external `tone_hz` conflicts.
  - 2026-03-15: WebRTC backend availability now means launchable, not merely configured; runtime/config status expose per-backend unavailable reasons, invalid `audio_webrtc.node_bin` is surfaced as preflight unavailability before any broker session is created, and the fail-fast child-exit cleanup path is now proved separately with a launchable-but-immediately-exiting runtime.
  - 2026-03-15: caller-supplied `broker_session_id` is now preflight-validated through the broker before launch, so agentd returns a clean missing-session error without spawning a managed peer against nonexistent signaling state.
  - 2026-03-15: `broker_session_id` is now mutually exclusive with `broker_agent_id` / `broker_deployment_id`, and the WebRTC runtime smoke proves that ambiguous mixed-mode start requests are rejected without creating peer/runtime state.
- [x] Add binary blob storage tiering plan (`docs/DB.md#blob-storage-tiers-design--status`).
  - [x] Add edge task/node analytics exports (CSV/JSON bundles).
  - [x] Implement blob_manifest schema + local blob store v0 (upload + read + ref-count GC).
  - [x] Add object-store tier (S3/MinIO) with signed URL reads + read-through cache.
  - [x] Add workflow analytics export bundles (CSV/JSON) for durable + edge workflows.
  - [x] Add tiering policy engine (promote/evict + size/age budgets) with metrics.
  - [x] Add blob DB query endpoints + analytics (`/api/v1/db/blobs`, `/api/v1/db/blob`, `/api/v1/db/analytics/blobs`).
  - [x] Add archive tier controls + restore gate (metadata-only; operator-managed cold storage transitions).
  - [x] Wire object-store archive/restore APIs (cold storage class transitions + restore status).
- [x] UI actions: stable public action API, autoplay unlock flow, and consented remote URL opens.
- [x] Memory privacy tags (`<private>`) to keep sensitive content out of durable storage (claude-mem inspired).
- [x] Memory progressive disclosure + citation surfacing for dynamic context assembly (claude-mem inspired).
- [x] Memory observations + timeline retrieval tools to mirror claude-mem search workflows.
- [x] Interop/attestation: finish inline certificate-chain enforcement and confidentiality beyond authenticated envelopes.
  - [x] Canonical JSON hashing (`agent_json_c14n_v1`) is wired into replay/attestation bundles and edge result hashing.
  - [x] Run replay + signed attestation bundles are implemented and verified via `run_attestation_bundle_tool_smoke` and `agentd_run_attestation_ed25519_smoke`.
  - [x] UM-BMP envelope authenticity is implemented for JSON/CBOR wire with HMAC + Ed25519, operator enforcement knobs, and MCU bring-up vectors.
  - [x] Agentd enforceable edge task attestation policy (`edge_attest_required` + `edge_attest_require_sig`) is documented and verified.
  - [x] 2026-03-15: added first-class trust-root rotation endpoints (`GET /api/v1/edge/auth/trust_roots`, `POST /api/v1/edge/auth/trust_roots/rotate`) with durable epoch metadata, signed bundle export, and live rotation proof in `tests/agentd_edge_auth_trust_roots_rotate_smoke.sh`.
  - [x] 2026-03-15: added per-node trust-root provisioning + binding inspection (`POST /api/v1/edge/auth/provision_node`, `GET /api/v1/edge/auth/node_binding`) with active `kid_policy` enforcement and proof in `tests/agentd_edge_auth_provision_node_smoke.sh`.
  - [x] 2026-03-15: added signed revocation bundle/control (`GET /api/v1/edge/auth/revocations`, `POST /api/v1/edge/auth/revocations/update`) with live revoked-`kid` / revoked-node enforcement proof in `tests/agentd_edge_auth_revocations_smoke.sh`.
  - [x] 2026-03-15: added `GET /api/v1/edge/node/manifest_bundle`, a platform-signed node/tool manifest bundle export backed by durable `edge_nodes` state and existing attestation keys; proof: `tests/agentd_edge_manifest_bundle_smoke.sh`.
  - [x] 2026-03-15: added node-pollable signed manifest distribution (`POST /api/v1/edge/node/manifest_bundle/send`) that enqueues `PLATFORM_MANIFEST_BUNDLE` over the existing UM-BMP outbox path; proof: `tests/agentd_edge_manifest_bundle_send_smoke.sh`.
  - [x] 2026-03-15: added node-pollable signed trust-root and revocation distribution (`POST /api/v1/edge/auth/trust_roots/send`, `POST /api/v1/edge/auth/revocations/send`) that enqueue `PLATFORM_TRUST_ROOTS_BUNDLE` and `PLATFORM_REVOCATIONS_BUNDLE`; proof: `tests/agentd_edge_auth_bundle_send_smoke.sh`.
  - [x] 2026-03-15: added durable PEM certificate-root bundle control and node-pollable signed delivery (`GET/POST /api/v1/edge/auth/cert_roots...`) so certificate-root material now has the same signed control-plane and outbox distribution path as manifests/trust-roots/revocations; proof: `tests/agentd_edge_auth_cert_roots_smoke.sh`.
  - [x] 2026-03-15: added operator-side cert-root bundle inspection and OpenSSL chain verification tooling (`tools/edge_cert_roots_tool.py`) backed by Ed25519 bundle verification support in `agent_ed25519_tool`; proof: `tests/agentd_edge_auth_cert_chain_verify_smoke.sh`.
  - [x] 2026-03-15: added server-side chain verification over the stored cert-root set (`POST /api/v1/edge/auth/cert_roots/verify_chain`) so agentd now exposes structured X.509 verification results over the live root bundle, including a fail-closed negative path; proof: `tests/agentd_edge_auth_cert_roots_verify_chain_smoke.sh`.
  - [x] 2026-03-15: added optional fail-closed manifest identity certificate-chain enforcement on `NODE_CAPS_RSP` (`edge_auth_require_manifest_cert_chain`) plus best-effort `identity_cert_verify` surfacing on node/manifest reads; proof: `tests/agentd_edge_manifest_identity_cert_enforce_smoke.sh`.
  - [x] 2026-03-15: added AES-GCM encrypted UM-BMP body support (`body_enc`) with fail-closed ingress enforcement (`edge_confidentiality_required`), configured key slots (`edge_confidentiality_keys`), a host bring-up helper (`tools/edge_confidentiality_tool.cpp`), and encrypted outbox delivery via `confidential_kid` on manifest/trust-root/revocation/cert-root send endpoints; proof: `tests/agentd_edge_confidential_body_smoke.sh`, `tests/agentd_edge_auth_bundle_send_smoke.sh`, and `tests/agentd_edge_manifest_bundle_send_smoke.sh`.
  - 2026-03-15: narrowed the stale umbrella item after re-verifying `umbmp_auth_vectors_tests`, `agentd_edge_auth_hmac_cbor_wire_smoke`, `agentd_edge_auth_ed25519_cbor_wire_smoke`, `agentd_edge_task_attest_required_smoke`, and `agentd_run_attestation_ed25519_smoke`.
- [x] AVM: persist governance bundles/record-replay artifacts, add explicit host-effects policy, and carry quorum/attestation identity through deterministic VM results.
  - [x] Non-exec AVM governance endpoints: `/api/v1/avm/job_scan`, `/policy_scan`, `/inspect`, `/verify_strict`, `/trace_hash`.
  - [x] Exec-gated capsule runner: `/api/v1/avm/capsule_run` with operator gates (`AGENTD_AVM_BIN`, `AGENTD_AVM_EXEC`, `yolo`) and deterministic hash extraction (`RESULT_HASH`, `TRACE_HASH`, `STATE_HASH`).
  - [x] Durable workflow VM task: `kind:"avm_capsule"` executes out-of-process under the same guarded runner.
  - [x] Deterministic quorum joins over VM hashes via workflow aggregate pointers (`/avm/result_hash`, `/avm/trace_hash`).
  - [x] Mount allowlist enforcement for direct capsule runs and workflow `avm_capsule` tasks.
  - [x] Structured output evidence is surfaced on direct and workflow results via `output.{raw_text,json_text,residual_text,hashes.*}`, and direct capsule runs preserve the parsed run fragment as `run_json_raw`.
  - [x] Persist a reusable governance bundle object (scan/inspect/verify/run/log) keyed by program/job hash.
  - [x] Expose AVM record/replay artifacts as durable evidence, not only transient subprocess stdout.
  - [x] Add explicit host-effects policy surfaces beyond mounts (FS/PROC/NET capability gating).
  - [x] Carry node identity / attestation material through quorum votes for multi-node correctness.
  - 2026-03-15: narrowed the stale umbrella item after re-verifying the shipped runner/scan/quorum surfaces in `agentd_avm_job_scan_smoke`, `agentd_workflow_avm_capsule_smoke`, and `agentd_workflow_aggregate_quorum_smoke`.
  - 2026-03-15: added structured AVM output evidence to the shipped runner path so callers no longer need to scrape raw stdout for the JSON fragment and hash tokens when using direct `capsule_run` or workflow `avm_capsule`.
  - 2026-03-15: added explicit `host_effects.{fs,proc,net}` request policy with fail-closed operator gates (`AGENTD_AVM_ALLOW_{FS,PROC,NET}`), runner env propagation, and direct/workflow smoke coverage.
  - 2026-03-15: workflow `avm_capsule` tasks now persist session-scoped AVM governance bundles plus durable output-log artifacts, keyed by program/job hash and verified through `GET /api/v1/session/artifacts` in `agentd_workflow_avm_capsule_smoke`.
  - 2026-03-15: workflow `avm_capsule` evidence runs now emit `run_attestation_bundle_v1` with stable `node_id`, persist `attestation_bundle.json`, and `quorum_hashes` automatically defaults `node_pointer` to `/avm/attest/node_id` for AVM hash joins while surfacing `attestations_by_task_id`.
- [ ] Node consensus: replace the current host-managed helper with embedded / firmware-native adoption and add dynamic membership / recovery policy above the shipped frame + relay + managed-runtime layer.
  - [x] Centralized platform-owned coordination is already the implemented design stance for UM-EAIS and broker team orchestration.
  - [x] Deterministic quorum/join surfaces are already shipped for workflows and broker team runs.
  - [x] Define a peer protocol for node-native elections/conflict resolution without relying on the platform coordinator.
  - [x] Carry node identity / trust-root material through decentralized votes and partition recovery.
  - [x] Add deterministic multi-node simulation tests for split-brain, duplicate delivery, trust-epoch mismatch, and quorum recovery.
  - [x] Integrate consensus frames into live edge outbox/inbox delivery and node polling flows.
  - [x] Persist and expose leader term / recovery state through edge node observability surfaces.
  - [x] Connect the relayed frame protocol to autonomous node-side execution loops rather than platform-only relay.
  - 2026-03-15: narrowed the stale umbrella item after re-verifying shipped centralized quorum coverage (`agentd_workflow_aggregate_quorum*`, `agentd_workflow_*quorum_hashes*`, `broker_team_runs_quorum_compose_smoke`, `broker_team_quorum_events_sse_compose_smoke`).
  - 2026-03-15: added a deterministic node-native consensus core (`edge_node_consensus_frame_v1`) with carried identity/trust witnesses and host-side duplicate/partition/recovery simulation proof in `edge_node_consensus_tests`.
  - 2026-03-15: added live `CONSENSUS_FRAME` relay over `/api/v1/edge/message` plus sender-node consensus summaries surfaced via `/api/v1/edge/node` and `/api/v1/edge/nodes`; proof: `tests/agentd_edge_consensus_transport_smoke.sh`.
  - 2026-03-15: added `agentd_edge_consensus_node`, a host-side autonomous node loop that polls `/api/v1/edge/outbox`, consumes relayed `CONSENSUS_FRAME`s through `EdgeConsensusReplica`, and posts generated vote/commit frames back through `/api/v1/edge/message`; proof: `tests/agentd_edge_consensus_autonomous_smoke.sh`.
  - 2026-03-15: agentd now manages that same autonomous loop through `POST/GET /api/v1/edge/node/consensus_runtime`, with `consensus_runtime` surfaced on `/api/v1/edge/node` and `/api/v1/edge/nodes`; proof: `tests/agentd_edge_consensus_runtime_smoke.sh`.
  - 2026-03-15: extracted the autonomous election scheduler / frame-routing behavior into reusable core code (`EdgeConsensusNodeLoop`) with dedicated unit proof in `tests/test_edge_node_consensus_loop.cpp`, so embedded adoption no longer has to start from the host CLI helper as the only implementation.
  - 2026-03-15: added bounded retry-capable campaign timers (`campaign_retry_ms`, `campaign_retry_max_ms`, `campaign_retry_backoff_factor`) to the reusable loop, host helper, durable cluster policy, and managed runtime, with late-peer convergence proved in `tests/agentd_edge_consensus_runtime_smoke.sh`.
  - 2026-03-15: added explicit membership versioning/member sets (`membership_epoch`, `member_node_ids`) to the shared consensus core, host helper, and managed runtime, with unit/runtime proof for stale/non-member rejection.
  - 2026-03-15: added signed durable cluster policy bundles (`edge_consensus_membership_v1`) with export/rotate/send APIs plus managed-runtime defaulting from stored member/retry policy; proof: `tests/agentd_edge_consensus_membership_bundle_smoke.sh`.
  - 2026-03-15: moved the normal managed runtime path in-process by adding a shared HTTP runtime core plus builtin `POST/GET /api/v1/edge/node/consensus_runtime` execution, while keeping `runtime_kind=external` for helper parity/debug; proof: `tests/agentd_edge_consensus_runtime_smoke.sh` and `tests/agentd_edge_consensus_membership_bundle_smoke.sh`.
  - 2026-03-15: added explicit leader heartbeat/lease policy (`leader_heartbeat_ms`, `leader_lease_ms`) to the reusable loop, durable cluster policy, managed runtime, and live runtime/membership proof so stale leaders can be expired deterministically before re-campaigning.
  - 2026-03-15: the optional `runtime_kind=external` consensus helper seam is now durable daemon config too (`edge_consensus.node_tool_path`), `/api/v1/config` reports whether that helper is configured and launchable, and the runtime smoke now proves config-backed external start/stop plus fail-closed unavailability after clearing the persisted helper path.
  - 2026-03-15: managed `runtime_kind=external` consensus starts now also use bounded startup confirmation, so executable-but-immediately-exiting helpers fail closed with `startup_confirmed=false` instead of reporting a false-positive started runtime.
  - 2026-03-15: managed consensus backend selection is now durable daemon policy too (`edge_consensus.default_runtime_kind`), startup honors `AGENTD_EDGE_CONSENSUS_DEFAULT_RUNTIME_KIND`, metadata exposes `default_runtime_kind_source=auto|env|config`, and invalid persisted defaults self-heal back to builtin auto during runtime-config load.
  - 2026-03-15: builtin managed consensus starts now use the same bounded startup confirmation as external helpers, so immediate in-process transport failures fail closed with `startup_confirmed=false` and leave no stale runtime record behind, while fast successful commits still return success.
  - 2026-03-15: managed consensus stop now returns `reason=not_running` when a runtime already finished and preserves the final runtime snapshot/result instead of reporting a false-positive active stop.
  - 2026-03-15: managed consensus start now returns `409` when a different effective runtime config tries to reuse an already-running `node_id`, while repeated identical starts remain idempotent with `already_running=true`.
  - 2026-03-15: managed consensus runtime snapshots now persist in DB meta too, so `GET /api/v1/edge/node/consensus_runtime` can recover finished/stopped runtime state after restart with `status_source=persisted`, can also recover a still-live external helper from a persisted running snapshot, and now self-heals stale builtin/corrupt persisted runtime records by clearing the record plus dead local artifacts.
  - 2026-03-19: if that recovered live external helper is later stopped through agentd, the persisted final runtime snapshot now keeps the stop signal/result instead of degrading into a signal-less stopped record.
  - 2026-03-19: if durable cluster membership/retry policy rotates while a managed consensus runtime is still running, runtime reads now surface `runtime.cluster_policy_drift` with changed fields plus the current policy, and the main runtime smoke proves restart-based adoption of the rotated policy.
  - 2026-03-20: if callers omit `trust_roots_epoch` / `revocations_epoch` / `cert_roots_epoch`, managed consensus start now defaults those from the daemon's current `edge_auth_*_epoch` policy, runtime reads surface `runtime.trust_epoch_drift` when that trust policy rotates underneath a live runtime, and the main runtime smoke proves restart-based adoption while the membership-bundle smoke now also checks explicit trust-epoch carriage.
  - 2026-03-20: active managed consensus runtimes now expose best-effort `runtime.live_status` snapshots while still running, including external helper parity while stdout remains attached to agentd, and the main runtime smoke proves operator-visible live member/trust identity plus campaign state before terminal result.
  - 2026-03-20: the builtin managed consensus backend now uses daemon-local transport instead of self-HTTP, normalizes runtime `daemon_url` to `@local`, and ignores builtin-only `daemon_url` / `auth_token` request drift when deciding whether a running runtime is reusable; proof: `tests/agentd_edge_consensus_runtime_smoke.sh`.
  - 2026-03-20: `GET /api/v1/edge/node?node_id=...` now falls back to a synthetic runtime-backed node record when no hello/caps row exists but managed consensus runtime state does, and the main runtime smoke proves builtin running, builtin persisted, and external recovered-runtime lookup through the main node-read API.
  - 2026-03-20: `GET /api/v1/edge/nodes` now applies the same runtime-backed fallback to listing, so runtime-only managed consensus nodes still appear even when the `edge_nodes` row is missing, with builtin-persisted and recovered-external proof in `tests/agentd_edge_consensus_runtime_smoke.sh`.
  - 2026-03-20: durable consensus policy now also carries `lease_expiry_recampaign_delay_ms`, the shared loop exposes lease-expiry recovery counters/deadlines in live/final status, and both membership/runtime smokes plus `test_edge_node_consensus_loop.cpp` prove the new bounded post-expiry cooldown path.
  - 2026-03-20: extracted the voice peer broker-session/launch/startup/cleanup orchestration into a shared launch-flow module with direct unit proof, refactored the bundled/external child backend onto it, and extended `builtin_start_contract` with the same staged `startup_sequence` so future native work targets a real shared seam instead of child-only logic.

## Deferred (after macOS stability)

- Validate Windows build (tool plugins + tool servers remain disabled).
  - Script available: `tools/verify_windows_build.ps1` (supports optional `VCPKG_ROOT`, runs `agent_core_tests` unless `-SkipTests`).
  - CI workflow added: `.github/workflows/windows-build.yml` (checks core build/tests on windows-latest).
  - Status helper: `tools/check_ci_windows_build.sh` (fetches latest run from GitHub API).
  - Trigger helper: `tools/trigger_ci_windows_build.sh [ref]` (dispatches CI run).

## Recently shipped (proof in CI)

- Tool plugins (`--tool-plugin`) so tools are composable without rebuilding.
  - Proof: `ctest` includes `agentd_tool_plugin_smoke`.
- Tool servers (out-of-process) so big integrations stay isolated and “bring-up fast”.
  - Daemon flag: `--tool-server-cmd "<cmd>"` (repeatable; stdio JSON-lines protocol)
  - Reliability knobs: `--tool-server-timeout-ms`, `--tool-server-max-line-bytes`, plus restart-with-backoff on death (fail-closed; no auto-retry)
  - 2026-03-19: tool-server child exec now closes inherited non-stdio fds too, and the restart smoke proves an
    ungraceful daemon death cannot leave an orphaned helper pinning the same listener port across restart.
  - Optional health checks: `--tool-server-ping-interval-ms <n>` (best-effort idle `op:"ping"`; auto-disables if server replies `unknown op`)
  - Docs: `docs/TOOLS.md`
  - Proof: `ctest` includes `agentd_tool_server_smoke`, `agentd_tool_server_ping_smoke`, and `agentd_tool_server_restart_smoke`.
- Broker/agent interop primitive for durable workflows (deterministic; gated):
  - New deterministic workflow task: `kind:"http_json"` with `http_json` (outbound HTTP JSON; no LLM required).
  - Safety: disabled by default (SSRF risk). Enable explicitly with `--workflow-enable-http-tasks` (or env `AGENTD_WORKFLOW_ENABLE_HTTP_TASKS=1`).
  - Optional hardening: restrict outbound targets with `--workflow-http-allow-host <host[:port]>` (repeatable),
    or env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`.
  - Further hardening: CIDR allowlist + deny-private mode:
    - `--workflow-http-allow-cidr <cidr>` (repeatable), or env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
    - `--workflow-http-deny-private`, or env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
  - Defense-in-depth: optional CIDR denylist + DNS pinning:
    - `--workflow-http-deny-cidr <cidr>` (repeatable), or env `AGENTD_WORKFLOW_HTTP_DENY_CIDRS=...`
    - `--workflow-http-dns-pin`, or env `AGENTD_WORKFLOW_HTTP_DNS_PIN=1`
    - Evidence: when DNS pinning is enabled, workflow results include best-effort `http.resolved_addrs` (IP list) for audit/debug.
    - Shipped hardening: DNS pinning now pins the **same resolution used by allow/deny policy** into the actual request (libcurl resolve list),
      closing the TOCTOU window where a hostname could rebind between “policy check” and “connect”.
  - Secrets hygiene: submit rejects `http_json.headers.Authorization`; use `http_json.bearer_env` (env var name only is persisted).
  - Proof: `ctest` includes `agentd_workflow_http_json_smoke`.
  - Correctness surface: when response JSON parsing succeeds, `http_json` emits a best-effort deterministic hash token over the JSON
    under `http.response_sha256` (algorithm `agent_json_c14n_v1`) for quorum/correlation.
- Agent-to-agent collaboration primitive (deterministic; gated):
  - New deterministic workflow task: `kind:"agentd_call"` with `agentd_call` (submits a remote workflow, polls until terminal, returns remote final JSON).
  - Spec: `docs/spec/agentd-agentd/agentd_agent_interop_v0_1.md`
  - Broker/connector compatibility: `agentd_call.base_url` may also be a **broker proxy prefix**
    (e.g. `https://<broker>/v1/agents/<agent_id>/proxy`) so durable workflows can collaborate with agents behind NAT.
  - Shipped (ergonomics + interop): `agentd_call.broker_proxy:{broker_base_url,agent_id}` as an alternative to `base_url`
    (server computes/persists the proxy prefix) so MCU/edge systems can target agents by identity without hardcoding URL paths.
    - Also supported by submit-time macro `kind:"agentd_parallel"` for each `targets[]` entry.
  - Optional hardening: restrict outbound targets with `--workflow-http-allow-host <host[:port]>` (repeatable),
    or env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`.
  - Further hardening: CIDR allowlist + deny-private mode:
    - `--workflow-http-allow-cidr <cidr>` (repeatable), or env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
    - `--workflow-http-deny-private`, or env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
  - Proof: `ctest` includes `agentd_workflow_agentd_call_smoke` and `agentd_workflow_agentd_parallel_quorum_hashes_smoke`.
  - Correctness surface: `agentd_call` now emits best-effort deterministic hash tokens for quorum/correlation:
    - `agentd.result_sha256`: hash of a **stable projection** of terminal remote `final.result` (preferred stable surface)
      - `agentd.result_sha256_schema=agentd_call_result_stable_v1` (ephemeral fields pruned before hashing)
    - `agentd.final_sha256`: hash of the full terminal remote workflow JSON (audit/debug; includes workflow_id/timestamps)
    (algorithm `agent_json_c14n_v1`).
  - Distinct-target quorum: `kind:"agentd_parallel"` defaults `aggregate.node_pointer="/agentd/base_url"` for `mode:"quorum_hashes"`
    so `require_distinct_nodes:true` counts distinct remote agent targets correctly.
  - Quorum ergonomics: for `kind:"agentd_parallel"` + `mode:"quorum_hashes"`, the server defaults `aggregate.pointers=["/agentd/result_sha256"]`
    so users don’t accidentally inherit the generic aggregate defaults (`/avm/result_hash`, `/avm/trace_hash`).
- Embedded bring-up helper: `agent_core` now includes UM‑BMP/UM‑EAIS interop helpers (`agent/edge_interop.h`)
  for id-safe validation/sanitization + message type constants (reduces node/platform drift).
  - Proof: `ctest` includes `agent_core_tests`.
- “Docs as truth” guardrails (DB schema doc + OpenAPI sanity checks).
  - Proof: `ctest` includes `docs_sanity_tests` + `openapi_sanity_tests`.
  - Shipped: OpenAPI now includes `/api/v1/config` and `/api/v1/config/update` so operator tooling can discover the effective
    workflow outbound HTTP policy (`workflow_http_allow_*`, `deny_*`, `dns_pin`) and scheduling knobs.
  - Shipped: workflow outbound HTTP policy knobs (`allow_hosts/allow_cidrs/deny_cidrs/deny_private/dns_pin`) are now runtime-mutable
    and persisted via `/api/v1/config/update` (no restart needed to tighten policy).
  - [x] Split `docs/openapi/agentd.yaml` into modular `$ref` files and update `openapi_sanity_tests` to accept multi-file specs.
  - Shipped: global HTTP request body limit for agentd (`AGENTD_HTTP_MAX_BODY_BYTES`) with a smoke test.
  - Shipped: per-file session upload cap (`AGENTD_UPLOAD_MAX_BYTES`, `--upload-max-bytes`) with structured upload errors.
- UM‑EAIS contract is now executable (best-effort) via machine-readable artifacts:
  - JSON Schemas: `docs/spec/um-eais/schema/`
  - Golden transcript fixtures: `docs/spec/um-eais/fixtures/`
  - Proof: `ctest` includes `um_eais_spec_sanity_tests`, `agentd_edge_interop_transcript_replay_smoke`,
    and `agentd_edge_interop_task_loop_replay_smoke`.
- UM‑BMP crash-window correctness: node-initiated handoff messages are replayable even if `msg_id` is deduped (at-least-once),
  preventing permanent drops when a crash happens after persisting `edge_inbox_messages` but before applying side effects.
  - Proof: `ctest` includes `agentd_edge_message_dedupe_replay_handoff_smoke`.
- UM‑BMP crash-window correctness (generalized): `edge_inbox_messages` now tracks a `processed` marker so **all** message types can be safely
  retried with the same `msg_id` after a crash window (covers `TASK_DONE`, `TASK_EVENT`, etc; not only handoffs).
  - Proof: `ctest` includes `agentd_edge_message_dedupe_replay_task_done_smoke`.
- `trace_id` correlation end-to-end, plus a merged trace timeline across broker ⇄ agentd.
  - Proof: `ctest` includes `agentd_trace_id_smoke`.
  - New convenience: `GET /api/v1/trace?trace_id=...&include_memory=1` attaches `memory_correlate` so traces can correlate
    durable execution with rolling structured memory checkpoints (uses `trace:<trace_id>` evidence).
- Trace correlation is now useful across edge interop:
  - `POST /api/v1/edge/task/assign` forwards an optional `trace` object into the `TASK_ASSIGN` envelope.
  - `edge_tasks` persists `trace_id` (durable correlation even if a node omits echoing trace on `TASK_*` messages).
  - Best-effort: if the platform did not set a trace_id, it backfills `edge_tasks.trace_id` from inbound edge messages that include `trace.trace_id`.
  - `GET /api/v1/trace?trace_id=...` also surfaces best-effort edge task metadata, task events, and inbound UM‑BMP envelopes.
  - Proof: `ctest` includes `agentd_trace_edge_interop_smoke` and `agentd_edge_task_attest_smoke`.
- Trace correlation now joins durable orchestration and edge workflows:
  - `GET /api/v1/trace?trace_id=...` also surfaces durable workflow events (`source:"workflow_event"`) via indexed `workflows.trace_id`.
  - `GET /api/v1/trace?trace_id=...` also surfaces edge workflow events (`source:"edge_workflow_event"`) correlated via `edge_tasks.trace_id`.
  - Proof: `ctest` includes `agentd_trace_workflow_events_smoke`.
- Oren AVM governance endpoints (scan-before-execute; out-of-process).
  - Endpoints:
    - `POST /api/v1/avm/job_scan` (`avm --print-job-json`)
    - `POST /api/v1/avm/policy_scan` (`avm --print-policy-json`)
    - `POST /api/v1/avm/inspect` (`avm --inspect-json`)
    - `POST /api/v1/avm/verify_strict` (`avm --verify-strict`)
    - `POST /api/v1/avm/trace_hash` (`avm --print-trace-hash`)
    - `POST /api/v1/avm/capsule_run` (exec gated; `avm --capsule --print-run-json ...`)
  - Bring-up helper: `tools/oren_avm_bringup.sh` builds/locates `../oren-lang/avm` and prints its absolute path for `AGENTD_AVM_BIN`.
  - Capsule task helper: `tools/oren_capsule_task.sh` compiles `.oren` → `.obc` and emits a ready `kind:"avm_capsule"` task JSON.
  - Proof: `ctest` includes `agentd_avm_job_scan_smoke` (stubbed AVM runner for determinism/CI).
- Durable workflow engine v1 (DAG + retries + resumable after restart + deterministic expectations).
  - API:
    - `POST /api/v1/workflow/submit`
    - `GET /api/v1/workflow`
    - `GET /api/v1/workflows`
    - `POST /api/v1/workflow/cancel`
  - Engine semantics:
    - dependency scheduling (`depends_on`)
    - retries (`max_attempts`) + backoff (`ready_unix_ms`)
  - correctness assertions (`expect`)
  - **restart continuity**: running tasks are recovered back to queued (at-least-once)
  - prompt templating (simple but powerful): `${task.<id>.assistant_text}` and `${task.<id>.json:/json_pointer}`
  - v2 template expansion: templates are expanded across the full task request JSON (not only `prompt`), enabling deterministic dataflow into `edge_invoke.args` and other structured fields.
  - v2 JSON-native embedding: `{"$ref":"task.<id>.json:/ptr"}` replaces the entire value with embedded JSON (not a string), enabling structured payload/args wiring.
  - v2.1 workflow inputs: tasks may carry `inputs` and reference them via `${input.<name>...}` and `{"$ref":"input.<name>..."}` for cleaner dataflow.
  - Proof: `ctest` includes `agentd_workflow_inputs_smoke`.
  - Optional workflow submit dependency inference: `infer_depends_on: true` scans for `${task.<id>...}` / `$ref:"task.<id>..."` and merges into `depends_on`.
  - Proof: `ctest` includes `agentd_workflow_infer_deps_smoke`.
  - Proof: `ctest` includes `agentd_workflow_smoke` (validates DAG ordering + templating + restart recovery).
- Workflow engine maintainability refactor:
  - JSON Pointer helper promoted to `daemon/src/json_util.*` (`json_pointer_get`).
  - Workflow template expander extracted into `daemon/src/workflow_templates.*` (keeps `workflow_engine.cpp` under ~2000 lines).
  - Workflow aggregation/join logic extracted into `daemon/src/workflow_aggregate.*` (keeps `workflow_engine.cpp` lean and SOLID).
  - Workflow engine common helpers + fair-queue picker extracted into:
    - `daemon/src/workflow_engine_common.*`
    - `daemon/src/workflow_engine_pick.cpp`
    (keeps `daemon/src/workflow_engine.cpp` under 2000 LOC even as scheduling evolves).
- Workflow endpoints maintainability refactor:
  - Split endpoint implementations into smaller translation units:
    - `daemon/src/workflow_query_endpoints.cpp` (GET `/api/v1/workflow`, GET `/api/v1/workflows`)
    - `daemon/src/workflow_admin_endpoints.cpp` (POST `/api/v1/workflow/cancel`, GET `/api/v1/workflow/events`, GET `/api/v1/workflow/stats`)
  - Workflow submit macro expander extracted into `daemon/src/workflow_submit_macros.*` to keep submit logic SOLID and cheap to evolve.
  - `daemon/src/workflow_endpoints.cpp` now focuses on submit-only and stays <2000 LOC.
  - Proof: existing workflow + stats + docs/openapi smokes still pass (`agentd_workflow_smoke`, `agentd_workflow_http_json_smoke`,
    `agentd_workflow_stats_smoke`, `docs_sanity_tests`, `openapi_sanity_tests`).
- Durable workflows can now run deterministic AVM capsule tasks (no LLM required):
  - Task kind: `kind: "avm_capsule"`
  - Task payload: `capsule: { obc_base64, timeout_ms, gas, mem_bytes, ... }` (same schema as `POST /api/v1/avm/capsule_run`)
  - Result shape: `result.results_by_task[task_id].avm.{run,result_hash,trace_hash,state_hash,...}`
  - Proof: `ctest` includes `agentd_workflow_avm_capsule_smoke` (runs AVM capsule, then templates its result into an LLM stub task).
- Durable workflows now support deterministic aggregation/join tasks (no LLM required):
  - Task kind: `kind: "aggregate"` (modes: `quorum_hashes`, `first_ok`, `best_of_n`, `collect`)
  - Use-case: compare deterministic hash surfaces (e.g. AVM `result_hash` / `trace_hash`) across N runs/nodes and require quorum.
  - Ergonomics: for `mode:"quorum_hashes"`, if a pointer resolves to a non-string JSON value, the platform hashes canonical JSON bytes and votes on the `sha256:<64hex>` token.
  - Proof: `ctest` includes `agentd_workflow_aggregate_quorum_smoke`, `agentd_workflow_aggregate_quorum_hashes_object_smoke`, `agentd_workflow_aggregate_first_ok_smoke`, and `agentd_workflow_aggregate_best_of_n_smoke`.
- Durable workflows now support UM‑EAIS edge collaboration tasks (no LLM required):
  - Task kind: `kind: "edge_invoke"` (dispatches `TASK_ASSIGN mode:"invoke"` and waits for `TASK_DONE`)
  - Also supports `mode:"agent"` (dispatches `TASK_ASSIGN mode:"agent"` with a prompt/payload for embedded `agent_core`)
  - Use-case: mix deterministic compute + LLM reasoning + real-world actuation in one durable DAG.
  - Correctness surface: workflow results now include `edge_result_sha256` (platform-computed sha256 of platform-canonicalized edge result JSON bytes; `agent_json_c14n_v1` best-effort) and `edge_attest` (best-effort node attest blob).
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_smoke`, `agentd_workflow_edge_invoke_template_args_smoke`, `agentd_workflow_edge_agent_smoke`, and `agentd_workflow_edge_agent_ref_payload_smoke`.
- Capability routing hardening (distinct-node fan-out helper):
  - `edge.match_any.exclude_node_ids` is now supported by both workflow `kind:"edge_invoke"` and `POST /api/v1/edge/task/assign`.
  - Use-case: parallel fan-out to *distinct* nodes without hardcoding node_id (avoid accidental same-node selection).
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_match_any_exclude_nodes_smoke`.
- UM‑EAIS platform extensions (node → platform workflow handoff) are now proven end-to-end:
  - `WORKFLOW_SUBMIT` and `WORKFLOW_CANCEL` can be ingested via `POST /api/v1/edge/message` and drive the edge workflow runner.
  - Best-effort node ACKs: outbox `WORKFLOW_ACK` is enqueued for both submit and cancel (non-HTTP transports).
  - Proof: `ctest` includes `agentd_edge_workflow_submit_message_smoke` and `agentd_edge_workflow_cancel_message_smoke`.
- Durable workflows can now be handed off via the same UM‑BMP ingress (MCU-friendly, transport-agnostic):
  - `DURABLE_WORKFLOW_SUBMIT` forwards to `POST /api/v1/workflow/submit` (platform durable orchestration)
  - `DURABLE_WORKFLOW_CANCEL` forwards to `POST /api/v1/workflow/cancel`
  - Best-effort node ACK: outbox `DURABLE_WORKFLOW_ACK {op, workflow_id, ok}`
  - Proof: `ctest` includes `agentd_edge_durable_workflow_submit_message_smoke`.
  - Correctness: default `idempotency_key` prefers `edge_wf:<workflow_id>` when `workflow_id` is caller-provided (stable retries).
- UM‑EAIS node capability cache correctness: when a node reports a new `caps_sha256`, the platform invalidates cached
  `manifest_json/tools_json/tags_json` and re-requests a full manifest (prevents stale routing).
  - Proof: `ctest` includes `agentd_edge_interop_smoke`.
- UM‑EAIS `caps_sha256` correctness hardening:
  - `caps_sha256` is now treated as a real sha256 token (`(sha256:)?[0-9a-fA-F]{64}`) across schemas + platform ingestion.
  - Embedded `agent_core` exposes the same validator (`agent_umbmp_sha256_token_is_safe`) so MCU firmware can self-check.
  - Proof: `ctest` includes `agentd_edge_caps_sha256_validation_smoke`, `agentd_edge_interop_transcript_replay_smoke`,
    and `agent_core_tests` (edge interop module tests).
- UM‑EAIS tool correctness guardrail: for `TASK_ASSIGN mode:"invoke"`, the platform validates `payload.args` against the
  target node tool schema from the stored manifest (`parameters_schema` preferred, fallback `parameters`) before enqueueing.
  - Proof: `ctest` includes `agentd_edge_task_assign_schema_validation_smoke`.
- UM‑EAIS tool correctness guardrail: for `TASK_DONE` of `mode:"invoke"`, if the tool definition includes a `result_schema`,
  the platform validates `result.data` (best-effort subset, fail-closed) before marking the task succeeded.
  - Proof: `ctest` includes `agentd_edge_task_done_result_schema_validation_smoke`.
- UM‑EAIS portable correctness surface (v0.3 partial):
  - `agent_core` now exposes `agent_json_c14n_v1` canonical JSON hashing (`agent/json_c14n.h`) so heterogeneous nodes can compute the same `result_sha256`.
  - The platform canonicalizes SUCCEEDED edge `result_json` and computes `edge_tasks.result_sha256` over canonical bytes (fallback to raw bytes on failure; evidence is recorded in task events).
  - Spec: `docs/spec/um-eais/um-eais-v0.3.md` + schema v0.3 files under `docs/spec/um-eais/schema/`.
  - Proof: `ctest` includes `agent_core_tests` (json c14n module) and edge attest replay smokes.
- UM‑EAIS scheduling guardrail (actuator safety): if a tool definition includes `resource_lock`, the platform blocks parallel
  dispatch of another invoke task using the same lock while an existing task is `QUEUED`/`RUNNING` (HTTP 429, retryable).
  - Proof: `ctest` includes `agentd_edge_resource_lock_smoke`.
- UM‑EAIS correctness surface (attestation precursor): edge tasks now persist a deterministic hash surface on completion:
  - `edge_tasks.result_sha256` is computed on `TASK_DONE` (sha256 of stored `result_json` bytes) and surfaced via `GET /api/v1/edge/task`.
  - Optional node attestation blob `result.attest` is persisted (best-effort) and surfaced as `task.attest`.
  - Proof: `ctest` includes `agentd_edge_task_attest_smoke`.
- Workflow engine now supports **soft-fail** tasks via `allow_error: true`:
  - If a task ends `status="error"` and `allow_error=true`, the workflow can still complete `done` (so long as no “hard” errors remain).
  - Dependencies treat `(status=error + allow_error=true)` as satisfied; aggregation tasks can also depend on any terminal dependency to compute joins over errors.
  - Proof: `ctest` includes `agentd_workflow_aggregate_first_ok_smoke` (uses an `allow_error` failing branch + `mode:"first_ok"` join).
- Workflow engine supports expanded deterministic expectations (`expect`) for correctness:
  - `json_pointer_exists`, `json_pointer_regex`, `json_pointer_number_between` (in addition to `ok`, `assistant_text_contains`, `json_pointer_equals`).
  - Proof: `ctest` includes `agentd_workflow_expect_extended_smoke` (includes a passing task and an allow_error failing task).
- Workflow engine supports task-controlled rescheduling for polling/async patterns:
  - Tasks may return `retryable=true` and `retry_in_ms` to control the requeue delay (instead of fixed polling/backoff).
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_smoke` (wait/poll loop uses task-controlled retry delay).
- Resumable async jobs: `run_async` requests persist enough state to resume after daemon restart (at-least-once semantics).
  - Proof: `ctest` includes `agentd_job_restart_durability_smoke` (restart mid-job → finishes done).
- Memory v2 (retrieval): `memory_search` now prefers a ranked on-disk index (SQLite FTS5) when available, with automatic fallback to bounded substring scan.
  - Also: structured memory updates (`memory_put(entries)`) produce rolling JSON checkpoints under `memory/checkpoints/` for time-correlation.
  - Proof: `ctest` includes `test_host_toolset` (memory tools) plus existing daemon smokes that exercise host tools.
- Memory v2.3 (structured query primitive): deterministic `memory_structured_query` host tool + workflow task kind for fetching structured records by key/prefix/kind/source evidence,
  with query-plan knobs for time windows + ordering (rolling consolidation and “recent facts” retrieval without substring scan).
  (more correct + explainable than substring search when keys are stable).
  - Proof: `ctest` includes `agentd_workflow_memory_structured_query_smoke`.
- Memory v2.1 (rolling consolidation, deterministic): `agentd` can promote explicit `@mem ...` markers from daily memory into structured memory via `POST /api/v1/memory/consolidate`, and can run it periodically with `--memory-consolidate-interval-ms`.
  - Proof: `ctest` includes `agentd_memory_consolidate_smoke` (idempotent; no checkpoint churn on second run).
- Durable workflows can now trigger deterministic rolling consolidation as a task:
  - Task kind: `kind: "memory_consolidate"` (scans `memory/YYYY-MM-DD.md` markers into `memory/STRUCTURED.md`)
  - Proof: `ctest` includes `agentd_workflow_memory_consolidate_smoke`.
- Memory v2.2 (versioned facts + evidence): structured memory entries now keep bounded `sources[]` (evidence) and `versions[]` (superseded history) under schema `agent_memory_v2`.
  - Proof: `ctest` includes `host_toolset_tests` assertions that validate schema upgrade + history retention.
- Durable workflows now support deterministic memory updates (correctness-gated + correlated):
  - Task kind: `kind: "memory_put"` (structured upsert via host tool `memory_put(entries=[...])`)
  - Engine injects workflow correlation evidence into `entries[].source` when missing (`workflow:<id> task:<id> trace:<id> ...`).
  - Proof: `ctest` includes `agentd_workflow_memory_put_smoke`.
- Workflow event log + streaming (durable): `agentd` persists workflow events (`workflow_events`) and exposes:
  - `GET /api/v1/workflow/events` (paged)
  - `GET /api/v1/workflow/stream` (SSE; ends with `workflow_done`)
  - Proof: `ctest` includes `agentd_workflow_stream_smoke`.
- Scheduler knobs (efficiency precursor): `agentd` supports configuring background engine concurrency/polling:
  - `--job-concurrency`, `--job-poll-ms`, `--workflow-concurrency`, `--workflow-poll-ms` (also reflected in `/api/v1/config`)
- Workflow scheduler fairness/budget caps (load resilience precursor):
  - `--workflow-max-inflight-per-workflow` (prevents one fan-out workflow monopolizing all workers)
  - `--workflow-max-inflight-per-session` (optional multi-tenant cap; default disabled)
  - Proof: `ctest` includes `agentd_workflow_inflight_cap_smoke` (asserts non-overlap under cap=1).
- Per-session fairness cap is now proven (multi-tenant guard):
  - Proof: `ctest` includes `agentd_workflow_inflight_session_cap_smoke`.
- Workflow scheduler scan fairness v2.0 (beyond caps; prevents LIMIT-starvation):
  - Oversampled workflow scan (bounded by DB clamp) so older workflows can’t be permanently excluded by `LIMIT`.
  - Session-aware scan order (round-robin across session buckets) to reduce multi-tenant starvation risk under heavy load.
  - Proof: `ctest` includes `agentd_workflow_scan_fairness_smoke`.
- Workflow admission control / submit-time backpressure (v1.5):
  - Caps total inflight workflow tasks (`queued|running`) at submit time (returns HTTP `429` with `retry_after_ms`).
  - Knobs:
    - `--workflow-admit-max-inflight-tasks-per-session` (env `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_PER_SESSION`)
    - `--workflow-admit-max-inflight-tasks-total` (env `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_TOTAL`)
  - Proof: `ctest` includes `agentd_workflow_admission_control_smoke`.
- Deterministic workflow-only delay task (`kind:"delay"`) for scheduling tests and wait gates (no LLM required).
  - Proof: `ctest` includes `agentd_workflow_inflight_cap_smoke`.
- Workflow correctness: tool-call constraints in `expect` (enforce “must call / must not call” deterministically).
  - Proof: `ctest` includes `agentd_workflow_expect_tool_calls_smoke` (uses ext_echo tool plugin).
- Workflow scheduler stats endpoint (queue pressure metrics):
  - `GET /api/v1/workflow/stats`
  - Proof: `ctest` includes `agentd_workflow_stats_smoke`.
  - Optional per-session inflight stats: `GET /api/v1/workflow/stats?include_sessions=1`
  - Proof: `ctest` includes `agentd_workflow_stats_sessions_smoke`.
- Workflow spec introspection:
  - `GET /api/v1/workflow?workflow_id=...&include_spec=1` returns redacted `spec_json` (+ parsed `spec` when valid).
  - Proof: `ctest` includes `agentd_workflow_get_spec_smoke`.
- Workflow deadline (scheduler-level):
  - Submit `deadline_unix_ms` to cancel queued tasks after a wall-clock cutoff; running tasks are cooperatively cancelled at safe boundaries (best-effort).
  - Proof: `ctest` includes `agentd_workflow_deadline_smoke`.
- Workflow submit idempotency + DB-backed policy columns:
  - Submit `idempotency_key` to dedupe workflow submits (safe retries / at-least-once upstream delivery).
  - Workflows now persist `deadline_unix_ms` and `idempotency_key` as dedicated DB columns (schema v17), so the scheduler does not depend on parsing `spec_json`.
  - Proof: `ctest` includes `agentd_workflow_idempotency_smoke` and `agent_db_tests` asserts the new columns exist.
- Workflow cooperative cancellation for running tasks (v1.4):
  - `POST /api/v1/workflow/cancel` now cancels running tasks at safe boundaries (tool loop + long-running host tools).
  - Deadline cancellation (`deadline_unix_ms`) also cancels running tasks best-effort.
  - Proof: `ctest` includes `agentd_workflow_cancel_running_smoke`.
- Workflow agent collaboration primitive (v1.6):
  - Task kind: `kind:"delegate"` runs a sequence of candidate sub-requests (fallback) with optional per-attempt `expect`,
    returning `delegate.attempts[]` + `delegate.chosen_id` and surfacing chosen `assistant_text`.
  - Proof: `ctest` includes `agentd_workflow_delegate_smoke`.
- Scheduler-visible parallel collaboration macro (v1.6.1):
  - Submit-time task macro: `kind:"delegate_parallel"` expands into N attempt tasks + a deterministic `kind:"aggregate"` join (mode `first_ok`).
  - Attempt tasks default to `allow_error=true` so one failed attempt doesn’t fail the workflow; the join fails only if all attempts fail.
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_macro_smoke`.
- Scheduler-visible agent collaboration macro (v1.10):
  - Submit-time task macro: `kind:"agentd_parallel"` expands into N `kind:"agentd_call"` tasks + a deterministic `kind:"aggregate"` join (default `mode:"first_ok"`).
  - Targets can be normal agentd base URLs or broker proxy prefixes (`.../v1/agents/<id>/proxy`).
  - Proof: `ctest` includes `agentd_workflow_agentd_parallel_macro_smoke`.
- Parallel collaboration macro join customization (v1.7.0):
  - `delegate_parallel` can now pass `delegate.aggregate` to pick a deterministic join strategy (e.g. `mode:"best_of_n"`).
  - Server overwrites `aggregate.task_ids` with the derived attempt task IDs (`<task_id>:<attempt_id>`).
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_best_of_n_smoke`.
- Quorum ergonomics for delegate_parallel (v1.7.x):
  - For `delegate.aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.pointers=["/assistant_text"]` when omitted,
    so the join does not accidentally inherit the generic aggregate defaults (`/avm/result_hash`, `/avm/trace_hash`).
  - For `delegate.aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.node_pointer="/effective_base_url"` when omitted,
    enabling `require_distinct_nodes:true` to do distinct-provider quorum votes over the run attempts.
    (Run attempts now surface `effective_base_url` / `effective_model` in their results for audit/quorum correlation.)
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_quorum_hashes_default_pointers_smoke` and `agentd_workflow_delegate_parallel_distinct_nodes_smoke`.
- Parallel collaboration macro upgrade (v1.7.1):
  - New deterministic join strategy: `mode:"quorum_ok"` (succeed only if >= quorum attempts are ok; deterministically chooses the first ok attempt).
  - New merge primitive: `delegate.attempt_defaults` (missing-key-only defaults applied to each attempt.request with higher priority than workflow defaults).
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_quorum_ok_smoke` and `agentd_workflow_aggregate_quorum_ok_smoke`.
- Parallel collaboration macro hardening (v1.7.2):
  - Deterministic strict join: `mode:"strict_all_ok"` (fail if any attempt is missing/not ok; deterministically chooses the first task as the chosen result).
  - Macro semantics: `kind:"delegate_parallel"` now preserves task-level fields on the join node (`allow_error`, `inputs`, `ready_unix_ms`) so it behaves like a normal task.
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_strict_all_ok_allow_error_smoke` and `agentd_workflow_aggregate_strict_all_ok_smoke`.
- Multi-node quorum evidence surface (v1.7.3):
  - `mode:"quorum_hashes"` now optionally surfaces stable node identity evidence via `aggregate.node_pointer` (default `/edge/node_id`),
    emitting `nodes_by_task_id` in the aggregate result (does not change quorum semantics).
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_quorum_hashes_node_ids_smoke`.
- Delegate-parallel macro correctness (v1.7.4):
  - `kind:"delegate_parallel"` now propagates the macro task’s `inputs` and `ready_unix_ms` to derived attempt tasks,
    so `${input.*}` templates work inside `attempt.request` without duplicating inputs on every attempt.
  - Proof: `ctest` includes `agentd_workflow_delegate_parallel_inputs_propagate_smoke`.
- Enforceable per-attempt budgets (v1.7.5):
  - New `delegate.attempt_caps` and `delegate_parallel.delegate.attempt_caps` clamp attempt run knobs (hard max; cannot be exceeded by attempt.request).
  - Delegate attempt result rows now surface `effective_*` fields for debugging/attestation (`effective_timeout_ms`, etc).
  - Proof: `ctest` includes `agentd_workflow_delegate_attempt_caps_smoke` and `agentd_workflow_delegate_parallel_attempt_caps_smoke`.
- Multi-node quorum hardening (v1.7.6):
  - For `mode:"quorum_hashes"`, new `require_distinct_nodes:true` counts votes across distinct `/edge/node_id` values instead of per-task votes.
  - Each distinct node contributes at most one vote per pointer (deterministically chosen from the first task_id with a non-empty value).
  - Proof: `ctest` includes `agentd_workflow_edge_invoke_quorum_hashes_distinct_nodes_smoke`.
- Distinct-node edge fan-out macro (v1.7.7):
  - New workflow task macro: `kind:"edge_parallel"` with `edge_parallel` (submit-time expansion).
  - Selects **distinct** nodes via `edge.match_any` (node registry), expands into per-node `kind:"edge_invoke"` tasks,
    then replaces the macro task with a deterministic aggregate join.
  - Default join behavior: `mode:"strict_all_ok"`; override via `edge_parallel.aggregate`.
  - Quorum ergonomics: for `edge_parallel.aggregate.mode:"quorum_hashes"`, the server defaults:
    - `aggregate.pointers=["/edge_result_sha256"]`
    - `aggregate.node_pointer="/edge/node_id"`
  - Proof: `ctest` includes `agentd_workflow_edge_parallel_macro_smoke` and `agentd_workflow_edge_parallel_quorum_hashes_default_pointers_smoke`.
- Event-triggered durable orchestration (v1.7.8):
  - Automation rules for `SENSOR_EVENT` now support `action.type:"durable_workflow_submit"` (in addition to `task_assign`).
  - The platform injects `inputs.sensor_event` (best-effort) so workflow tasks can template against `${input.sensor_event...}`.
  - Proof: `ctest` includes `agentd_edge_rules_durable_workflow_smoke`.
- Durable workflow edge wait gate (v1.7.9):
  - New deterministic workflow task: `kind:"edge_wait_sensor"` with `edge_wait_sensor` (polls `edge_sensor_events` and is `retryable` until a matching event arrives).
  - Default `since_utc_ms` is workflow creation time (prevents old sensor events satisfying a new workflow by accident).
  - Proof: `ctest` includes `agentd_workflow_edge_wait_sensor_smoke`.
- Durable workflow memory retrieval (v1.7.10):
  - New deterministic workflow task: `kind:"memory_search"` with `memory_search` (host tool; no LLM required).
  - Enables workflows to retrieve relevant memory snippets deterministically and feed them into later tasks via templates/expectations.
  - Proof: `ctest` includes `agentd_workflow_memory_search_smoke`.

## P0 (next: maximize autonomous continuity + correctness)

### Reweighted next 5 (highest compound impact)

Priority order (reweighted after event-triggered durable orchestration shipped; memory leak resolved):

1) **Autonomous continuity** — durable orchestrator state + refresh-safe UI resume + guidance lane alignment.
2) **Streaming stability** — OpenRouter pins + provider matrix upkeep + provider key verification (Moonshot/Kimi, OpenRouter).
3) **Tool plugin isolation** — sandbox/host policy hardening + limits enforcement.
4) **Agent collaboration v2.2** — unify fan-out/join patterns across edge + LLM tasks (edge_parallel + delegate_parallel).
5) **Interop v0.4** — enforceable edge attestation + trust roots and stronger multi-node identity binding.

Details (stable order for diff readability; numbering below is not priority):

1) **Agent collaboration v2.2** — unify fan-out/join patterns across edge + LLM tasks (edge_parallel + delegate_parallel),
   plus event-triggered durable workflows as a first-class collaboration primitive.
   - Foundation shipped: deterministic outbound HTTP `kind:"http_json"` (gated) plus `kind:"agentd_call"` for agent-to-agent workflow handoff.
   - Shipped hardening: outbound HTTP policy for `http_json`/`agentd_call`:
     - `--workflow-http-allow-host` (repeatable)
     - `--workflow-http-allow-cidr` (repeatable)
     - `--workflow-http-deny-private` (defense-in-depth)
   - Shipped hardening: explicit CIDR denylist + optional DNS pinning (defense-in-depth against DNS rebinding).
2) **Interop v0.4** — enforceable edge attestation + trust roots and stronger multi-node identity binding.
   - Why now: collaboration and workflows amplify power, but MCU ecosystems need authenticity to prevent spoofed nodes/messages
     from turning “automation” into “unsafe actuation”.
   - Shipped (v0.4 partial): envelope-level authenticity via HMAC keyring + enforcement knob (`edge_auth_required`),
     plus optional replay-window hardening (`edge_auth_require_ts`, `edge_auth_max_skew_ms`).
   - Shipped (v0.4 partial): CBOR-native auth input option for MCU stacks:
     - `auth.alg="hmac-sha256-cbor"` verifies HMAC over deterministic CBOR bytes of the envelope with `auth` removed
       (definite lengths; text-string map keys ordered by UTF‑8 byte length then bytes; float64 only; matches `daemon/src/cbor_encode.*`).
     - Embedded bring-up: `agent_core` deterministic CBOR writer (`agent/cbor_det.h`) can generate these bytes without pulling in a full CBOR library.
     - Embedded bring-up: `agent_core` helpers `agent/umbmp_auth.h` build `env_no_sig` CBOR bytes and compute `auth.sig` base64 for HMAC/Ed25519.
   - Shipped (v0.4 partial): public-key envelope auth (no shared-secret blast radius):
     - `auth.alg="ed25519"` verifies Ed25519 signatures over canonical JSON bytes (`agent_json_c14n_v1`).
     - `auth.alg="ed25519-cbor"` verifies Ed25519 signatures over deterministic CBOR bytes (same profile as above).
   - Shipped (v0.4 partial): authoritative canonicalization/signature test vectors for MCU bring-up:
     - Fixture: `docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json`
     - Proof: `ctest` includes `umbmp_auth_vectors_tests`
   - Shipped (v0.4 partial): best-effort verification of `TASK_DONE.result.attest.sig` (Ed25519/HMAC) when key material is provisioned:
     - Verifies a small versioned signing string `UM_EAIS_RESULT_ATTEST_v0_1` over `{task_id,step_id,idempotency_key,result_sha256,ts_utc_ms}`.
     - Evidence is emitted under edge task events (trace-queryable).
     - Embedded bring-up: `agent_core` helper `agent_um_eais_result_attest_signing_input_v0_1(...)` generates the exact signing bytes.
   - Next: complete trust roots + identity binding:
     - replay window guidance + nonce/ts enforcement for lossy bridges (configurable; deterministic)
   - Shipped (MCU ergonomics): small-footprint CBOR **decoder** helper in `agent_core` for the same definite-length wire profile:
     - Core API: `agent/cbor_read.h` (no allocations; returns views into input bytes; depth/item limits for safety).
     - Proof: `ctest` includes `agent_core_tests` (decodes the canonical `node_hello_minimal` auth vector).
   - Shipped (MCU ergonomics): UM‑BMP envelope CBOR decode helper in `agent_core`:
     - Core API: `agent/umbmp_envelope_read.h` (extracts `{msg_id,ts_utc_ms,type,from,to,trace_id,auth}` and returns `body` as an opaque CBOR slice).
     - Proof: `ctest` includes `agent_core_tests` (decodes canonical vector + a trace+sig variant).
   - Shipped (MCU ergonomics): UM‑EAIS `TASK_ASSIGN` body CBOR decode helper in `agent_core`:
     - Core API: `agent/um_eais_task_assign_read.h` (extracts `{task_id,step_id,idempotency_key,mode,deadline_utc_ms,attempt}` and returns `payload` as an opaque CBOR slice).
     - Proof: `ctest` includes `agent_core_tests` (builds a CBOR `TASK_ASSIGN` envelope and decodes it without JSONCPP).
     - Note: supports optional `body.node_id` (v0.3 schema); the platform now includes it in TASK_ASSIGN envelopes for defense-in-depth.
   - Shipped (MCU ergonomics): UM‑EAIS task lifecycle CBOR body encoders in `agent_core`:
     - Core API: `agent/um_eais_task_lifecycle_write.h` encodes `TASK_ACK`, `TASK_EVENT`, `TASK_FAILED`, and `TASK_DONE` bodies using deterministic CBOR map key ordering.
     - Proof: `ctest` includes `agent_core_tests` (encodes bodies and asserts canonical key ordering via the embedded CBOR reader).
   - Shipped (MCU ergonomics): UM‑EAIS node lifecycle CBOR body encoders in `agent_core`:
     - Core API: `agent/um_eais_node_write.h` encodes `NODE_HELLO`, `NODE_HEARTBEAT`, and `SENSOR_EVENT` bodies with deterministic key ordering.
     - Proof: `ctest` includes `agent_core_tests` (encodes bodies and asserts canonical key ordering via the embedded CBOR reader).
   - Shipped (MCU ergonomics): UM‑EAIS `NODE_CAPS_RSP` CBOR body encoder in `agent_core`:
     - Core API: `agent/um_eais_node_caps_rsp_write.h` wraps caller-provided manifest CBOR bytes and validates the manifest is a definite-length map with text keys.
     - Optional strict mode enforces deterministic key ordering and no duplicates (recommended for `auth.alg="*-cbor"` signature stability).
     - Proof: `ctest` includes `agent_core_tests`.
   - Not shipped (yet): a built-in deterministic CBOR encoder for `NODE_CAPS_RSP.body.manifest` (schema-driven; likely generated or app-specific).
   - Shipped (MCU ergonomics): UM‑BMP envelope CBOR encode helper in `agent_core` (wire send path):
     - Core API: `agent/umbmp_auth.h` `agent_umbmp_envelope_cbor_v0_4(...)` can emit deterministic CBOR wire envelopes with optional `auth.sig` (base64 text).
     - Proof: `ctest` includes `agent_core_tests` (encodes a full envelope with `auth.sig` and decodes it back via the CBOR reader).
3) **Scheduling policy v2.4+** — DRR is shipped; telemetry-driven cost is now shipped (`telemetry_v1`), next is budget-pressure-aware charging and resilient fairness under mixed workloads.
4) **Memory v2.3** — query-plan primitives (bounded windows + key-prefix filters) and automatic consolidation triggers as time advances,
   so long-running systems keep context tight and correct.
   - Shipped (v2.3 partial): search-based memory context injection for runs (`memory_context_mode="search"`) with ranked snippets,
     citations, and bounded caps (claude-mem style progressive disclosure).
   - Shipped (v2.3 partial): `memory_search` clamps results/snippet/context to avoid unbounded memory growth under hostile inputs.
5) **Budgets v0.7** — complete streaming usage accounting and host-tool charging; surface budget pressure so schedulers can act cheaply.

Maintainability note (always-on):
- Keep endpoint implementations SOLID and <2000 LOC per file; split large translation units (e.g. workflow endpoints) so new collaboration primitives remain cheap to add.
- Shipped: submit-time macro expansion is now isolated (`daemon/src/workflow_submit_macros.*`).
- Shipped: structured checkpoint scan/read logic is centralized (`daemon/src/memory_checkpoints.*`) so memory endpoints and deterministic workflow tasks stay consistent.
- Shipped: `/api/v1/run` endpoint refactor to keep `daemon/src/run_endpoints.cpp` <2000 LOC by extracting cohesive helpers:
  - `daemon/src/run_endpoints_internal.*` (job request redaction, host prompt pinning, file safety, tool extension mux helpers)
  - `daemon/src/run_multimodal.*` (multimodal prefix handling and content-part construction)
  - `daemon/src/run_memory_context.*` (durable memory context injection helpers)
  - `daemon/src/run_client_acks.*` (best-effort client acknowledgement verification helpers)
- Shipped: unified `trace_id` validation + generation in `daemon/src/trace_id_util.*` (used by run/trace/edge).
- Shipped: workflow engine refactor to keep `daemon/src/workflow_engine.cpp` <2000 LOC by extracting memory maintenance task handlers
  into `daemon/src/workflow_memory_ops.*` (keeps memory evidence/checkpoint correlation consistent across tasks).
- Follow-up: extract remaining submit-time validators/redaction helpers if `daemon/src/workflow_endpoints.cpp` grows again,
  so the durable workflow surface can evolve without accumulating another “mega endpoint” file.
- DB backend decision: **stay on SQLite** (WAL + bounded writes + migrations) until a concrete requirement demands replication/multi-writer semantics at the DB layer.
  - If that day comes, prefer a *SQLite-compatible* path (e.g. libSQL) before rewriting the storage layer around KV engines.

1) **Durable budget enforcement at scheduler level** (correctness + cost predictability)
   - Shipped (v0): workflow-level tool-call budget `workflow_limits.max_tool_calls_total` enforced by the workflow engine:
     - clamps LLM/tool tasks’ `max_tool_calls_total` to the remaining workflow budget
     - cancels LLM/tool tasks when the remaining workflow budget is 0 (durable + restart-safe)
     - emits `workflow_budget_exceeded` events and surfaces run telemetry (`tool_calls_total`, `steps_executed`, `elapsed_ms`) for auditing
     - Proof: `ctest` includes `agentd_workflow_budget_tool_calls_smoke`.
   - Shipped (v0.1): workflow-level step/time budgets:
     - `workflow_limits.max_steps_total` (sum of `steps_executed`)
     - `workflow_limits.max_elapsed_ms_total` (sum of `elapsed_ms`, best-effort)
     - Proof: `ctest` includes `agentd_workflow_budget_steps_smoke`.
   - Shipped (v0.2): retry-safe budget charging via DB cumulative counters:
     - `workflow_tasks.tool_calls_total_cum`, `steps_executed_cum`, `elapsed_ms_cum`
     - Fixes undercounting across retries (budget enforcement now charges every attempt)
     - Proof: `ctest` includes `agentd_workflow_budget_retry_charges_smoke` and `agent_db_tests` asserts schema v19.
   - Shipped (v0.3): bulk cancellation of queued tasks when budget exceeded:
     - prevents scheduler thrash by cancelling all `status=queued` tasks in one pass (unclaimed tasks keep `attempt==0`)
     - Proof: `ctest` includes `agentd_workflow_budget_bulk_cancel_smoke`.
	   - Shipped (v0.4): workflow-level token budgets (best-effort; provider-reported):
	     - Budget knob: `workflow_limits.max_total_tokens` (sum of `total_tokens`, provider response `usage.total_tokens`).
	     - Durable counters: `workflow_tasks.prompt_tokens_cum`, `completion_tokens_cum`, `total_tokens_cum` (retry-safe charging).
	     - Run telemetry surfaced per task: `prompt_tokens`, `completion_tokens`, `total_tokens` (aggregated across all LLM calls in the run).
	     - Proof: `ctest` includes `agentd_workflow_budget_tokens_smoke` and `agent_db_tests` asserts schema v21.
	     - Budget telemetry is now visible at the workflow level:
	       - `GET /api/v1/workflow?...` surfaces `workflow_limits`, `workflow_usage`, and `workflow_remaining` (best-effort).
	       - Proof: `ctest` includes `agentd_workflow_budget_tokens_smoke`.
	     - Limitations:
	       - only enforced when providers return `usage` in responses
	       - streaming tool-loop calls require provider support for `stream_options.include_usage`; otherwise usage may be missing
	   - Shipped (v0.6 partial): streaming token usage accounting (best-effort; OpenAI-compatible Chat Completions):
	     - When `stream_assistant:true` is used, the tool provider requests `stream_options.include_usage=true` and emits `llm_usage` events
	       when the final chunk includes usage.
	     - Compatibility fallback: if a provider rejects `stream_options`, the client retries once without it (streaming still works, but usage may be missing).
	     - Proof: `ctest` includes `agentd_workflow_budget_tokens_stream_smoke`.
	   - Next: host-tool budgets, enforce token budgets for streaming paths, and surface budget pressure in `/api/v1/workflow/stats`.
	   - Shipped (v0.5 partial): deterministic host-tool tasks now charge and obey workflow budgets:
	     - `kind:"memory_put"` and `kind:"memory_consolidate"` count as `tool_calls_total=1` and `steps_executed=1` per attempt.
	     - Budget enforcement: if remaining workflow budgets are 0, these tasks cancel the workflow with `workflow budget exceeded: ...`.
	     - Proof: `ctest` includes `agentd_workflow_budget_host_tool_memory_put_smoke`.
	   - Shipped (v0.5 partial): budget pressure surface for cheap polling:
	     - `GET /api/v1/workflow/stats?include_budget_pressure=1` returns best-effort aggregate pressure under `budget_pressure`
	       across queued|running workflows that define `workflow_limits`.
	     - Optional `include_budget_workflows=1` includes a small sampled workflow list with `remaining` budgets.
	     - Proof: `ctest` includes `agentd_workflow_stats_budget_pressure_smoke`.

2) **Scheduling policy v2 (beyond caps)** (predictable progress under load)
   - Shipped (v2.0): oversampled scan + session-aware round-robin scan order (prevents `LIMIT` starvation under typical load).
   - Proof: `ctest` includes `agentd_workflow_scan_fairness_smoke`.
   - Shipped (v2.1): scheduler DB scan is now oldest-first (priority DESC, created_unix_ms ASC) with an index, so fairness holds even
     when queued workflows exceed the DB scan clamp (512).
   - Proof: `ctest` includes `agentd_workflow_scan_fairness_smoke` (submits >512 workflows; older session still completes early).
   - Shipped (v2.2): explicit fair-queue policy surface (weighted round-robin, session-scoped):
     - Daemon config/flags: `--workflow-fair-queue-policy wrr` (default), weight clamps.
     - Workflow submit knob (requires `allow_sessions=true`): `session_weight` (>=1).
   - Proof: `ctest` includes `agentd_workflow_wrr_session_weight_smoke` (ensures session B is not starved behind session A; session A still dominates early prefix when weight=2).
   - Next (v2.3): graduate from WRR to deficit round-robin (DRR) with cost-aware quanta (e.g. budget pressure, token cost) and durable per-session tokens.
   - Shipped (v2.3 partial): deficit round-robin (DRR) policy option (cost=1 per admitted task):
     - Daemon flag: `--workflow-fair-queue-policy drr` (also `AGENTD_WORKFLOW_FAIR_QUEUE_POLICY=drr`)
     - Weight source remains `session_weight` from workflow submit spec (clamped).
     - Proof: `ctest` includes `agentd_workflow_drr_session_weight_smoke`.
   - Shipped (v2.3.1): DRR deficits are now persisted (best-effort) so fairness survives daemon restart:
     - DB table: `workflow_fairq_sessions` (schema v25; see `docs/DB.md`).
     - Proof: `ctest` includes `agentd_workflow_drr_durable_deficit_smoke`.
   - Shipped (v2.3.2): optional cost-aware DRR charging (simple_v1; default remains unit-cost):
     - New daemon knob: `--workflow-drr-cost-model simple_v1` (env `AGENTD_WORKFLOW_DRR_COST_MODEL=simple_v1`)
     - Scheduler charges DRR deficit by best-effort estimated task cost (bounded + deterministic by request JSON).
     - Proof: `ctest` includes `workflow_fairq_cost_tests`.
   - Shipped (v2.3.3 partial): cost model tightened for LLM-heavy queues (still deterministic):
     - LLM-like runs (presence of `model` + `prompt`) are charged higher than deterministic tasks.
     - Prompt length and `stream_assistant:true` add small bounded bumps (better fairness under mixed workloads).
     - Proof: `ctest` includes `workflow_fairq_cost_tests`.
   - Shipped (v2.4.0): telemetry-driven DRR charging option (telemetry_v1; mixed deterministic + polling workloads):
     - New daemon knob: `--workflow-drr-cost-model telemetry_v1` (env `AGENTD_WORKFLOW_DRR_COST_MODEL=telemetry_v1`)
     - Scheduler prefers charging cost from the **last attempt’s telemetry** stored in `workflow_tasks.result_json` when available:
       - uses `elapsed_ms`, `total_tokens`, `tool_calls_total`, `steps_executed`
       - uses `retryable` + `retry_in_ms` as a poll-loop hint
     - Rationale: first attempt can be “heavier” (enqueue/submit), but steady-state polls are cheap; telemetry_v1 makes that distinction.
     - Proof: `ctest` includes `workflow_fairq_cost_tests` (covers telemetry_v1 estimator).
   - Next (v2.3+): tighten the cost model:
     - incorporate budget pressure, token usage counters, and edge polling characteristics into the estimator.

3) **Interop spec hardening for MCU/edge handoff** (ecosystem leverage)
   - Shipped: `DURABLE_WORKFLOW_SUBMIT` / `DURABLE_WORKFLOW_CANCEL` over `POST /api/v1/edge/message` (durable orchestration handoff).
     - Proof: `ctest` includes `agentd_edge_durable_workflow_submit_message_smoke`.
   - Shipped: `agent_core` UM‑BMP interop helpers (id-safe + sanitizer + message type constants).
     - Proof: `ctest` includes `agent_core_tests`.
   - Shipped: portable crypto primitive for embedded nodes + daemon parity:
     - `agent_core` now includes `agent_hmac_sha256` (`agent/hmac_sha256.h`) built on the same minimal SHA-256 implementation.
   - Shipped: tiny base64 helpers for embedded nodes:
     - `agent_core` includes `agent/base64.h` (RFC 4648 standard alphabet) so MCU firmware can emit `auth.sig` and pubkey base64 strings without ad-hoc libs.
   - Shipped: optional CBOR wire encoding for MCU/gateway efficiency:
     - `POST /api/v1/edge/message` accepts `Content-Type: application/cbor` with a CBOR map shaped like the JSON envelope.
     - `GET /api/v1/edge/outbox` accepts `Accept: application/cbor` and returns `Content-Type: application/cbor` (binary response).
     - Constraint: definite-length items only; map keys must be text strings (string-key CBOR profile).
     - Proof: `ctest` includes `agentd_edge_message_cbor_smoke`, `agentd_edge_outbox_cbor_smoke`,
       and `agentd_edge_task_loop_cbor_wire_smoke` (TASK_ACK/EVENT/DONE over `application/cbor`).
     - Proof (auth + CBOR): `ctest` includes `agentd_edge_task_loop_auth_hmac_cbor_wire_smoke` and
       `agentd_edge_task_loop_auth_ed25519_cbor_wire_smoke` to prove envelope auth gates apply to `TASK_*` lifecycle too
       (not only `NODE_HELLO`).
     - Shipped (drift guard): `agentd_edge_message_cbor_smoke` now generates the CBOR bytes using a core-linked encoder tool
       (`tools/agent_core_umbmp_cbor_encode.cpp`) instead of a handwritten Python CBOR snippet, reducing profile drift.
     - Shipped (bring-up ergonomics): the same encoder tool now supports emitting deterministic CBOR envelopes for:
       `NODE_HELLO`, `NODE_HEARTBEAT`, `NODE_CAPS_RSP`, `SENSOR_EVENT`, `TASK_ACK`, `TASK_EVENT`, `TASK_DONE`, `TASK_FAILED`.
     - Shipped (bring-up ergonomics): the encoder tool can also generate a **minimal but invoke-capable** deterministic CBOR
       UM‑ACDS manifest (`--manifest-minimal-ws2812`) containing tool `ui.led.ws2812.control` with:
       - `parameters_schema` (`additionalProperties:false`, `required:["action"]`)
       - `result_schema` (validates `result.data.text`)
     - Proof: `ctest` includes `agentd_edge_invoke_task_loop_auth_hmac_cbor_wire_smoke` (mode=invoke, schema validation + HMAC auth).
     - Proof: `ctest` includes `agentd_edge_invoke_task_loop_auth_ed25519_cbor_wire_smoke` (mode=invoke + Ed25519 auth).
     - Proof: `ctest` includes `agentd_edge_rules_sensor_event_auth_hmac_cbor_wire_smoke`
       (SENSOR_EVENT triggers automation rule which enqueues TASK_ASSIGN; node completes over the same CBOR+auth transport).
     - Proof: `ctest` includes `agentd_edge_rules_sensor_event_auth_ed25519_cbor_wire_smoke`
       (same rule loop, but `auth.alg="ed25519-cbor"` for public-key node identities).
     - Proof: `ctest` includes `agentd_edge_heartbeat_auth_hmac_cbor_wire_smoke`
       (NODE_HEARTBEAT over CBOR+auth persists `edge_nodes.health_json` evidence for operator visibility).
     - Shipped (MCU docs): TinyCBOR / `cobr` style implementation note mapping external CBOR libs onto this repo’s deterministic profile:
       - `docs/spec/um-eais/mcu_cbor_encoder_notes.md`
     - Shipped (correctness): preserve numeric types for CBOR auth signing inputs:
       - CBOR `float64` must remain `float64` in deterministic signing bytes even if numerically integral (e.g. `87.0`).
       - Proof: `ctest` includes `cbor_det_roundtrip_tests` and `agentd_edge_heartbeat_auth_hmac_cbor_wire_smoke` (with telemetry floats).
     - Next (optional): define fixed-point telemetry fields for MCU nodes (schema-level ergonomics):
       - e.g. `battery_bp` (0..10000), `rssi_dbm` (int), `temp_c_x100` (int)
       - keep float fields supported, but recommend fixed-point for validation simplicity.
   - Shipped (v0.4 partial): optional envelope authenticity (HMAC) for trust roots + spoofing resistance:
     - Envelope may include `auth:{alg,kid,seq?,sig}` where:
       - `alg:"hmac-sha256"` signs canonical JSON bytes (`agent_json_c14n_v1`)
       - `alg:"hmac-sha256-cbor"` signs deterministic CBOR bytes (definite lengths; key order by UTF‑8 length then bytes; float64 only)
       - `alg:"ed25519"` signs canonical JSON bytes (`agent_json_c14n_v1`)
       - `alg:"ed25519-cbor"` signs deterministic CBOR bytes (same profile as above)
       - `sig` is base64 of 32 bytes (HMAC) or 64 bytes (Ed25519)
     - Operator control-plane:
       - `GET /api/v1/config` surfaces `edge_auth.required` + `edge_auth.hmac_keys_set`
       - `POST /api/v1/config/update` supports:
         - `edge_auth_required` and `edge_auth_hmac_keys`
         - `edge_auth_ed25519_pubkeys` (kid -> base64(pubkey32))
         - optional replay-window hardening: `edge_auth_require_ts` and `edge_auth_max_skew_ms`
         - optional per-node trust root policy: `edge_auth_kid_policy` ("any"|"match_node"|"node_prefix")
         - optional anti-replay: `edge_auth_require_seq` (strict monotonic `auth.seq` per node; best-effort)
     - Enforcement behavior:
       - if required: missing/invalid auth is rejected with HTTP 401 (fail-closed; no inbox persistence)
       - optional: if `edge_auth_require_ts=true`, authenticated envelopes require `ts_utc_ms > 0`
       - optional: if `edge_auth_max_skew_ms > 0`, authenticated envelopes are rejected when `abs(now-ts_utc_ms)` exceeds the window
       - if optional: unsigned accepted, but if `auth` is present it must verify
     - Works for both JSON and CBOR wire encodings (auth is verified over platform canonical JSON after decoding).
     - Proof (CBOR wire): `ctest` includes `agentd_edge_auth_hmac_cbor_wire_smoke` (posts `Content-Type: application/cbor` envelopes signed with `hmac-sha256-cbor`).
   - Shipped (v0.4 partial): optional envelope authenticity (Ed25519) for per-node identities (no shared-secret blast radius):
     - Proof (CBOR wire): `ctest` includes `agentd_edge_auth_ed25519_cbor_wire_smoke` (posts `Content-Type: application/cbor` envelopes signed with `ed25519-cbor`).
   - Shipped (gateway ergonomics): `agent_core` can decode the CBOR outbox response without JSONCPP:
     - Core API: `agent/um_eais_outbox_read.h` extracts `messages[].msg` as `agent_umbmp_envelope_view_t` views.
     - Proof: `ctest` includes `agent_core_tests` (outbox decode unit) and `agentd_edge_outbox_cbor_smoke` now validates
       the platform response using a core-linked helper (`agent_core_outbox_cbor_check`).
   - Shipped (node ergonomics): UM‑EAIS `PLATFORM_CAPS_REQ` body CBOR decode helper in `agent_core`:
     - Core API: `agent/um_eais_platform_caps_req_read.h` (extracts `{node_id,want}` with strict want enum).
     - Proof: `agent_core_outbox_cbor_check` validates `want:"full"` for the platform-generated outbox message.
     - Spec note: `docs/spec/um-eais/um-bmp-envelope-auth-hmac-v0.4.md`
     - Proof: `ctest` includes `agentd_edge_auth_hmac_smoke` and `agentd_edge_auth_ed25519_smoke`.
   - Next: consolidate UM‑EAIS + durable workflow handoff into a single **versioned interop contract** with explicit:
     - idempotency rules (`msg_id` vs `idempotency_key`) and replay guidance for lossy transports (MQTT/LoRa bridges)
     - correlation rules (`workflow_id` / `trace_id` / task trace suffixing)
     - safety defaults (inline API keys forbidden; gateway-auth required)
   - Deliverables (shipped): JSON Schemas (envelope + core + platform extensions) + golden transcript fixtures (replay-ready JSONL).
     - Proof: `ctest` includes `um_eais_spec_sanity_tests`, `agentd_edge_interop_transcript_replay_smoke`,
       and `agentd_edge_interop_task_loop_replay_smoke`.
   - Shipped: normative reliability rules + strict task-loop correlation:
     - Spec: `docs/spec/um-eais/um-eais-v0.1.md` §5.3 (reliability + idempotency)
     - Platform enforcement: `TASK_ACK/TASK_EVENT/TASK_DONE/TASK_FAILED` require `idempotency_key` (reject missing/invalid)
   - Shipped (v0.2 draft): attestation + trace correlation are now versioned and executable:
     - Spec addendum: `docs/spec/um-eais/um-eais-v0.2.md`
     - JSON Schemas: `docs/spec/um-eais/schema/*v0.2*.schema.json`
     - Fixture transcript: `docs/spec/um-eais/fixtures/umbmp_task_loop_v0.2_trace_attest.jsonl`
     - Proof: `ctest` includes `agentd_edge_interop_task_loop_trace_attest_v0_2_replay_smoke`.
   - Shipped (v0.3 partial): portable canonical hashing surface for `result_sha256`:
     - Canonical algorithm: `agent_json_c14n_v1` (core API: `agent/json_c14n.h`).
     - Platform implementation: edge `TASK_DONE` results are canonicalized before hashing/storing; evidence is emitted via
       `_platform_result_sha256_alg` and `_platform_result_c14n_error`.
     - Spec: `docs/spec/um-eais/um-eais-v0.3.md` + v0.3 schemas.
   - Next (v0.3+): extend payload conventions for deterministic compute attestations:
     - allow nodes to attach deterministic compute hashes (e.g. AVM `result_hash` / `trace_hash`) under `result.attest.*` for quorum joins.

4) **Agent collaboration v2 (budgeted parallel fan-out + join macros)** (power-unleashed)
   - Status: submit-time parallel macro shipped as `kind:"delegate_parallel"` (v1.6.1).
   - Shipped: deterministic join strategies for parallel fan-out now include `first_ok`, `best_of_n`, `quorum_ok`.
   - Shipped: per-attempt wiring defaults via `delegate.attempt_defaults` (enables per-attempt budget knobs without repetition).
   - Remaining: enforceable per-attempt budgets + richer joins (`strict_all_ok`, node-identity-aware quorum votes).

5) **Memory ↔ workflow time correlation (next after memory_put)** (time-advancing correctness)
   - Shipped: deterministic workflow `kind:"memory_put"` and deterministic workflow `kind:"memory_consolidate"`.
   - Shipped: evidence hashing for replay/correlation:
     - `memory_put` structured checkpoints now emit `checkpoint_sha256` (sha256 of checkpoint JSON bytes).
     - workflow engine emits `workflow_events` `type="memory_checkpoint"` with `{checkpoint:{path,sha256,ts_utc,...}}` when memory changes.
     - Proof: `ctest` includes `agentd_workflow_memory_put_smoke` and `agentd_workflow_memory_consolidate_smoke`.
   - Shipped: bounded correlation queries (time windows + trace_id evidence needle):
     - `GET /api/v1/memory/checkpoints`
     - `GET /api/v1/memory/correlate?trace_id=...`
     - Proof: `ctest` includes `agentd_memory_correlate_smoke`.
   - Shipped: “memory query plan” primitives for large fleets:
     - `GET /api/v1/memory/checkpoints?structured_path=...` (filter checkpoints by structured file)
     - `GET /api/v1/memory/correlate?...&structured_path=...&key_prefix=...` (bounded + prefix-filtered correlation)
     - `GET /api/v1/memory/query?...&key_prefix=...` (bounded query over the current view of structured memory)
   - Shipped: deterministic workflow memory correlation query (no LLM; scans structured checkpoints on disk):
     - Task kind: `kind:"memory_correlate"` with `memory_correlate:{trace_id?,since_utc_ms?,until_utc_ms?,max_entries?,timeline?}`
     - Proof: `ctest` includes `agentd_workflow_memory_correlate_smoke`.
   - Shipped: deterministic workflow structured memory query (no LLM; reads newest structured checkpoint on disk):
     - Task kind: `kind:"memory_query"` with `memory_query:{since_utc_ms?,until_utc_ms?,structured_path?,key_prefix?,limit?}`
     - Proof: `ctest` includes `agentd_workflow_memory_query_smoke`.
   - Shipped: cross-layer event correlation by `trace_id`:
     - `GET /api/v1/trace?trace_id=...` now joins durable `workflow_events` and `edge_workflow_events` (best-effort) into the same trace surface.
     - Proof: `ctest` includes `agentd_trace_workflow_events_smoke`.
   - Next: extend correlation beyond trace lookup:
     - add an API to query structured memory “current view” by key-prefix/time window (avoid downloading/parsing whole checkpoints).

### 1) AVM capsule execution v0 (next: integrate + attest)

Goal:
- Make correctness and replayability a first-class primitive: “code as data capsule” runnable under explicit budgets,
  with deterministic hashing surfaces so results can be validated across time/nodes.

Deliverables:
- Persisted “governance bundle” object (job/policy/inspect/verify/run) so platform code can do scan → run → attest
  with one stable stored object keyed by `job_hash_sha256` / `program_hash_sha256`.
- Workflow + join:
  - (shipped) durable workflows can dispatch a capsule run as a task kind (no LLM required)
  - (shipped) deterministic aggregation/join nodes (`kind:"aggregate"`) can compare `RESULT_HASH` / `TRACE_HASH` across runs/nodes (k-of-n correctness)
  - (next) extend aggregation strategies (`first_ok`, `quorum_ok`, `strict_all_ok`, `collect`, `best_of_n`) and attach node identity to votes for multi-node correctness.
- Edge interop integration:
  - extend UM‑EAIS payload conventions so a node can execute a capsule and report back hashes as “task done” attestation.

Proof:
- Deterministic smoke test with a stub AVM binary + (optional) integration smoke when `../oren-lang` is present.

### 2) Budgets + scheduling policy (fairness, concurrency, backpressure)

Goal:
- Make the framework “strong by default” under load: predictable progress and fairness across jobs/workflows.

Deliverables:
- Fair scheduling:
  - per-session fairness (avoid one client starving others even with high `priority`)
- Budget + backpressure:
  - token/step/tool-call budgets enforced at scheduler level (not just per-run)
  - queue pressure metrics + admission control
- Cancellation semantics:
  - deterministic cancellation propagation (queued → cancelled; running → cooperative cancel)
 - (shipped) per-workflow in-flight cap + round-robin workflow scan start to reduce starvation.

Proof:
- Stress test (deterministic stub provider): submit N workflows/jobs and assert bounded completion time + fairness.

### 3) Workflow engine v2: dataflow + aggregation nodes

Problem:
- DAG ordering is useful, but real workflows need explicit **dataflow** and **aggregation strategies**.

Deliverables:
- Dataflow model:
  - (shipped) `${task.<id>.assistant_text}` and `${task.<id>.json:/ptr}` template expansion across full task request JSON (prompt + structured fields)
  - (shipped) explicit `inputs` map per task (shared variables; supports `${input...}` and `{"$ref":"input..."}`; enables schema validation later)
- Aggregation nodes (no LLM required by default):
  - `first_ok`, `quorum_ok`, `best_of_n`, `strict_all_ok`, `collect`
- Optional LLM aggregator (tools=none by default).

Proof:
- Deterministic stub-server test executes a DAG with dataflow and asserts outputs are wired correctly.

### 4) Workflow streaming + UI surface (without making UI the power source)

Status:
- Shipped v1.5: durable workflow event log + SSE stream + smoke test.
- Shipped: WebUI workflow DAG viewer (read-only) for listing + inspecting tasks.
- Shipped: WebUI workflow composer (JSON templates + submit).

Next:
- UI view for workflow timeline (reuse trace UI patterns), plus filters (by task_id, by event type).
- [x] Drag-and-drop workflow composer (graph editor) with JSON import/export + submit.

### 5) Memory v2: semantic retrieval + rolling consolidation

Problem:
- Stateless LLM calls and bounded context require a memory system that evolves over time:
  - consolidates outdated facts
  - correlates new information with old (conflict resolution)
  - retrieves relevant memory efficiently

Deliverables:
- Retrieval (shipped v2.0):
  - `memory_search` prefers SQLite FTS5-ranked retrieval when available (`use_index=true`), scoped to the same file set as legacy scanning (`daily_days`, core/session/structured).
  - Automatic fallback: bounded substring scan when SQLite/FTS5 is unavailable at runtime.
- Rolling consolidation + correlation (shipped v2.1, deterministic core):
  - explicit `@mem` marker promotion (daily → structured) + `POST /api/v1/memory/consolidate`
  - optional periodic scheduler (`--memory-consolidate-interval-ms`) with a conservative default (disabled)
- Rolling consolidation + correlation (next):
  - time/size based consolidation across **all** layers (core/daily/session/structured), not just daily markers
  - versioned facts: `supersedes`, `observed_utc`/`valid_from`, and multi-source evidence arrays
  - correlation graph: link memory items to `trace_id`/workflow/job ids and source excerpts

Proof:
- `ctest` covers memory tools end-to-end (`test_host_toolset`), ensuring `memory_write`→`memory_search`→`memory_get` works.
- Next: add a deterministic ranking test (index mode) + deterministic conflict-resolution tests for structured mode.

### 6) Correctness v2: validators + replayability

Deliverables:
- Expand `expect`:
  - JSON pointer assertions (already v1)
  - regex, numeric bounds, schema checks
  - tool-call constraints (e.g., forbid certain tools, require a tool call)
- “Replay mode”:
  - re-run a workflow from persisted inputs using a stub provider
  - deterministic outputs validated by expectations

Proof:
- `ctest` includes replay tests that produce identical results under stub providers.

## P1 (big wins after P0)

### 6) Tool servers (subprocess / stdio) + remote device tool bridges

Status:
- Shipped: `--tool-server-cmd` loads out-of-process tools via a strict stdio JSON-lines protocol.
- Shipped: reliability hardening (fail-closed):
  - per-server timeouts: `--tool-server-timeout-ms` (must follow `--tool-server-cmd`)
  - per-server response byte cap: `--tool-server-max-line-bytes` (must follow `--tool-server-cmd`)
  - optional idle health checks: `--tool-server-ping-interval-ms` (best-effort `op:"ping"`; auto-disables if unsupported)
  - restart-with-backoff if the server dies (does **not** auto-retry the same tool call)
  - Proof: `ctest` includes `agentd_tool_server_smoke`, `agentd_tool_server_ping_smoke`, and `agentd_tool_server_restart_smoke`.
- Shipped: protocol violations mark tool call errors with `protocol_violation=true` (invalid JSON / oversized responses).
- Shipped: WebUI tool result view surfaces protocol violations with a dedicated warning panel.

Remaining:
- Remote device bridges:
  - reference tool server for ESP32 serial/MQTT bridges that speaks the same protocol and advertises UM‑ACDS tool schemas

### 7) Multi-agent workflows (broker-aware)

Status:
- Foundation shipped: deterministic workflow `kind:"http_json"` (gated) plus `kind:"agentd_call"` for remote durable workflow handoff without an LLM.
- Shipped (high leverage): submit-time collaboration fan-out macro `kind:"agentd_parallel"` (expands into parallel `agentd_call` tasks + a deterministic `aggregate` join).
  - Why this compounds: it turns `agentd_call` into a first-class **redundancy/correctness** primitive (first_ok, quorum, collect, best_of_n),
    and makes multi-agent patterns scheduler-visible (fairness/budgets apply to each branch).
  - Shipped: identity-based broker proxy addressing (`agentd_call.broker_proxy` and `agentd_parallel.targets[].broker_proxy`).
  - Remaining after macro: broker-routed target discovery/routing policy + identity-scoped memory (see below).

Deliverables:
- Allow workflow tasks to target:
  - local agentd
  - broker-routed agents
- Add explicit routing policy and identity-scoped memory.

Proof:
- Integration test exercises broker fan-out with workflow DAG dependencies.
- voice builtin start contract now exposes shared planned runtime artifact layout
- voice builtin start contract now also exposes a runtime-schema-shaped planned runtime preview
- valid builtin `501` starts now also surface that planned runtime preview through the normal top-level `peer` snapshot path
