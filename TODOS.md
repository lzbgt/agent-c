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
  - [ ] macOS full-stack compose verification on this host blocked by Docker `unpigz/runc` resource errors; needs Docker Desktop resources increased or prebuilt images.
  - [x] Host-mode macOS full-stack verification script (Docker only for Postgres/Keycloak) added as fallback.
  - [x] WebUI connection profiles for multi-agentd deployments (direct or broker-backed).
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
3) **Next-gen contract foundation (agent-core + agentd + broker + WebUI)** — make interop explicit and replayable.
  - [x] Add a unified capability descriptor (`/api/v1/caps`) with protocol versions + limits (agentd + WebUI + broker proxy).
  - [x] WebUI: use `/api/v1/caps` to hide/disable unsupported features and cache last-known caps.
  - [x] Define a single event schema for run/workflow events and validate in CI (schema registry + tests).
  - [x] Add idempotency keys to broker proxy/orchestrate (safe retries with audit trail).
  - [x] Introduce replay bundles for deterministic runs (inputs + hashes + tool outputs) with fixture tests.
  - [x] Transport-agnostic relay interface for broker/connector (transport.Conn + WebSocket adapter).

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

## Promoted goals (explicit goals; no non-goals)

Weights updated 2026-02-19: prioritize **streaming stability** (including verified
OpenRouter pins) and **tool plugin isolation** hardening; keep **interop/attestation**
and **AVM** queued behind those until streaming and plugins are stable.

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
- [ ] Streaming: core-layer streaming interface + provider compatibility matrix with full variant coverage.
  - [x] Core SSE parser (`agent_sse_parser_t`) shared by CLI/daemon streaming paths.
  - [x] Core stream decoder (`agent_stream_decoder_t`) + unit tests (`tests/test_stream_decoder.c`).
  - [x] Compatibility matrix for streaming variants + provider coverage in `docs/STREAMING.md`.
  - [x] OpenRouter + Moonshot streaming assistant smoke tests (key-gated).
  - [x] OpenRouter + Moonshot streaming tool-call smoke tests (key-gated).
  - [x] DeepSeek streaming tool-call smoke test (key-gated).
  - [x] OpenRouter streaming probe script to discover stable models.
  - [x] OpenRouter streaming probe can emit `ref/openrouter/streaming_pins.json`; smoke tests prefer the pin file.
  - [ ] Populate OpenRouter streaming pins with a verified key (current key returns auth errors on chat completions across candidates).
  - [x] Draft core streaming interface spec (`docs/spec/streaming/core_stream_v1.md`).
  - [x] Wire CLI/daemon streaming through core stream decoder (replace duplicated host logic).
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
- [ ] Tool plugins: sandbox/isolation, Windows loader, and embedded/MCU-compatible plugin path.
  - [x] Tool plugin config JSON support (optional `*_ex` symbols + `--tool-plugin-config`) with smoke coverage.
  - [x] Windows loader for tool plugins (LoadLibrary/GetProcAddress).
  - [x] Sandbox via tool server host (`agentd_tool_plugin_host`) + smoke test.
  - [x] Plugin host resource limits (cpu/wall/as) with best-effort enforcement.
  - [x] Per-plugin policy limits (config JSON) resolved to the most restrictive values.
  - [x] In-process plugin manifest/result caps (1 MiB/4 MiB) to bound memory spikes.
  - [x] Plugin host tool-result cap (4 MiB) enforced before JSON parsing.
  - [x] Plugin host oversized payload smoke coverage (ext_big).
- [ ] Audio streaming: Opus/WebRTC voice pipeline + broker relay + UI voice session controls.
  - [x] Add workflow DB query endpoints (`/api/v1/db/workflows`, `/api/v1/db/workflow`, `/api/v1/db/workflow_tasks`, `/api/v1/db/workflow_events`)
    with docs + smoke tests.
  - [x] Add edge workflow DB query endpoints + workflow analytics aggregates (counts, latency, error rates).
  - [x] Add edge task/edge node analytics aggregates (counts, latency, error rates).
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
- [ ] Interop/attestation: PKI provisioning + signed manifests/attestations + canonical JSON hashing + envelope confidentiality.
  - [x] Agentd enforceable attestation policy (`edge_attest_required` + `edge_attest_require_sig`) with docs + tests.
- [ ] AVM: scoped flag passthrough, host-effects policy, record/replay plumbing, and quorum/attestation.
- [ ] Node consensus: decentralized coordination protocol with conflict resolution + deterministic simulation tests.

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

1) **Streaming stability** — OpenRouter pins + provider matrix upkeep + provider key verification (Moonshot/Kimi, OpenRouter).
2) **Tool plugin isolation** — sandbox/host policy hardening + limits enforcement.
3) **Audio streaming** — Opus/WebRTC pipeline + broker relay + UI voice controls.
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
     - per-node key provisioning workflow (bootstrap + rotation)
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

Next:
- UI view for workflow timeline (reuse trace UI patterns), plus filters (by task_id, by event type).

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
