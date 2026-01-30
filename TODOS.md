# TODOs

This file tracks the next highest-leverage tasks to reach the goal described in `project.md`, using `DESIGN.md` as the spec (which may evolve).

## Near-term (core correctness + portability)

- [x] Define a stable **LLM provider interface** in the core (no HTTP/JSON assumptions).
- [x] Define a stable **tool registry + executor interface** in the core (host-pluggable tools: GPIO, sensors, etc.).
- [x] Move the **tool-call loop** into core so daemon/embedded can reuse it (host provides JSON + transport adapters).
- [x] Add a portable, JSON-free **session persistence codec** (`.sess`) and make the host session store load/save both `.sess` and `.json`.
- [x] Decouple `agent_core` from host-only dependencies in CMake (`-DAGENT_BUILD_HOST=OFF`).
- [x] Implement a core **runner** that:
  - compacts session before calls (char budget)
  - calls provider
  - appends assistant text to session
- [x] Add a persistence interface (optional) so host apps can supply file/SQLite/NVS implementations.
- [x] Expand unit tests for compaction edge cases (overlap prefix/suffix; empty; all system messages).
- [x] Add a small unit test for tool-loop compaction/rotation logic (JSON message arrays) to prevent regressions.
- [x] Add a unit test to ensure tool result truncation stays JSON-shaped and capped.

## Near-term (CLI ergonomics)

- [x] Add `agent chat` REPL mode (session-backed, incremental turns).
- [x] Improve error messages (HTTP status + provider error extraction).
- [x] Deduplicate `tools=none` OpenAI provider glue between CLI + daemon (shared adapter in `agent_host`).
- [x] Add `--summary-model` option (host-generated compaction summary insertion for `--tools none`).
- [x] Implement extensible host toolsets (subprocess exec + diff-based file edits) for CLI/daemon (`--tools host`).
- [x] Add bounded filesystem inspection tools (`fs_stat`, `fs_list`, `fs_read`) for token-efficient inspection (paged reads; entry caps).
- [x] Add bounded file discovery tool (`fs_find`) so models avoid unbounded `find`/`tree`.
- [x] Add an OpenRouter **tool-call** smoke test (host toolset) using a cheap model that supports `tools`.
- [x] Add DeepSeek/OpenRouter `fs_find` network smokes to keep bounded discovery working.
- [x] Persist **tool-call transcripts** in a host-only audit log (`~/.agent/sessions/<id>.events.jsonl`) so session messages stay clean (token-safe).
- [x] Make tool-loop `--max-steps` default unlimited (0 means unlimited).
- [x] Add “session rotation” retries for `tools="none"` when providers reject an over-long context (CLI + daemon).
- [x] Support explicit proxy override (`--proxy` / request `proxy`) to avoid network hangs when env proxy is required.
- [x] Add `--stream-assistant` for tool loops (CLI) to emit incremental `assistant_delta` output (provider-dependent).
- [x] Support `--stream-assistant` for `--tools none` (CLI) to stream assistant output to stdout (provider-dependent).
- [x] Add more network smokes for host tools beyond `shell_exec` (e.g. `fs_read` paging) to reduce regressions.
- [x] Add a host-tool network smoke for `text_search` (DeepSeek + daemon) to reduce regressions.
- [x] Improve host filesystem tools defaults to avoid noise:
  - [x] exclude `node_modules`/`build`/`dist` by default in `fs_list` (configurable)
  - [x] support optional `respect_gitignore`/`exclude_globs`

## Mid-term (daemon + broker)

- [x] Add a minimal local daemon (`agentd`) with HTTP+JSON endpoints for a Web UI client.
- [x] Add a structured event log (`events`) for UIs (LLM + tool timeline).
- [x] Add daemon job garbage collection (TTL + max jobs) so long-running `agentd` stays memory-bounded.
- [x] Add optional daemon auth token (`--auth-token` / `AGENTD_AUTH_TOKEN`) for safe non-loopback deployments.
- [x] Refuse non-loopback binding without auth by default (`--allow-unauth` override).
- [x] Expand daemon RPC surface:
  - list sessions, get session, delete session
  - fetch per-run audit entries
- [ ] Add streaming responses (SSE/WebSocket) for UI responsiveness.
  - [x] stream job events via SSE (`GET /api/v1/job/stream`) for responsive UIs
  - [~] stream assistant token deltas (provider-dependent)
    - [x] `tools=none`: daemon can emit `assistant_delta` events when `stream_assistant=true`
    - [x] `tools=none`: retry/rotate context on “too long” errors (reduces perceived hangs)
    - [~] `tools=basic/host`: stream while still supporting tool calls (provider-dependent; best-effort)
      - [x] OpenAI-style `delta.tool_calls` reconstruction + `assistant_delta` events via SSE streaming (`stream: true`)
  - make event log fully structured (typed payloads; truncation metadata; stable schema)
- [x] Add async runs (`/api/v1/run_async` + `/api/v1/job`) so UIs can poll long-running jobs without blocking.
- [x] Add job cancellation (`POST /api/v1/job/cancel`) and UI Cancel button (cooperative; kills host subprocess tools).
- [x] Add live job progress via event tailing:
  - `GET /api/v1/job?job_id=...&include_events=1&cursor=...` (cursor-based incremental event polling).
- [x] Emit lightweight `heartbeat` events for async jobs during long tool execution / slow providers (avoids "hang" perception).
- [x] Add a tool-loop runaway guard (`max_repeated_tool_calls`) and expose it end-to-end (daemon request + UI setting + CLI flag).
- [x] Fix tool loop limit semantics: hitting `max_steps` is an error (`AGENT_ERR_LIMIT`) and the daemon default is configurable (`0` means unlimited).
- [x] Add additional tool-loop runaway caps beyond step-count: `max_tool_calls_total` and `max_tool_calls_per_tool` (daemon defaults are configurable; UI/CLI can override per-run).
- [ ] Add a daemon-level “sandbox policy” model:
  - YOLO vs host-scoped tools-root
  - future: per-tool allow/deny and command restrictions for `proc_exec` / `shell_exec`
  - [x] add `--host-policy full|readonly` (readonly omits exec + patch tools)
  - [x] support per-request `host_policy` and `/api/v1/tools?host_policy=...` (can only tighten vs daemon default)
  - [x] Tighten `yolo` + `tools_root` the same way (requests can only reduce scope), including `/api/v1/file`
  - [x] add `/api/v1/tools` for clients to query active tool schemas

## Near-term (UI polish)

- [x] Render assistant output as Markdown (GFM).
- [x] Render code blocks with syntax highlighting in the UI (Markdown + highlight.js).
- [x] Render tool outputs with a JSON-aware view (diff + stdout/stderr previews).
- [x] Render runs as a conversation (message cards) derived from `events` (user → assistant → tool calls/results).
- [x] Persist UI settings in `localStorage` (refresh-safe) and allow toggling the settings panel.
- [x] Surface daemon config snapshot (`GET /api/v1/config`) in the Settings panel for debugging.
- [x] Keep the last completed run visible during async runs and transient fetch/SSE errors (avoid “history wiped” perception).
- [x] Add a daemon file endpoint for UI previews (`/api/v1/file`) for images/audio/video artifacts.
- [x] Add multi-client-safe session creation (`POST /api/v1/session/new`) and a UI “New session” action.
- [x] Add explicit artifact events (`artifact`) so UI can render media without regex guessing.
- [x] Add an “Artifacts” panel that indexes artifacts from session audit (cross-run browsing).
- [x] Add an operator-friendly daemon `--state-dir` / `--sessions-root` so multiple agentd instances never share the same state by accident.
- [x] Add an explicit `ui_action` event type (agent → UI) with an allowlist and user consent (e.g. play audio for an artifact, focus an artifact).
- [x] Add a DB-backed troubleshooting view for `ui_actions` (list recent actions, click through to run detail).
- [x] Add a UI → daemon “client events” channel (`/api/v1/session/ui_event`) and mirror it into the troubleshooting DB.
- [x] Add a cooperative `ui_wait_event` host tool so agents can wait for UI acknowledgements *within a single run* (reduces retry loops without relying on hard limits).
- [x] Make `GET /api/v1/tools` accept `session_id` so UIs can see session-scoped tools (e.g. `ui_wait_event`) during tool introspection.
- [x] Keep `<session>.client_events.jsonl` bounded via best-effort rotation + backups; expose `include_rotated` tail reading in `GET /api/v1/session/client_events`.
- [x] Extend `ui_wait_event` matching to support nested `data_match` objects (safe, bounded recursion) and cover it with a smoke test.
- [x] Add join-wait host tools (`ui_wait_any`, `ui_wait_all`) so agents can deterministically wait for one-of or all-of multiple UI acknowledgements.
- [x] Add UI auto-ack client events (`ui_action_shown`, `artifact_rendered`) so “present to UI” tasks have an observable DoD without relying on tool-call caps.
- [x] Generalize UI events into a client collaboration protocol: `/session/client_event`, `client` identity, and client-agnostic wait/probe tools (`client_wait_*`, `client_peek`).
- [x] Add a client state snapshot protocol (`client_state`) + client-side media telemetry (video play events) so agents can proactively reason about client environment state.
- [x] Add a universal client RPC surface (`ui_action type=client_rpc` + `client_rpc_result`/`client_rpc_progress`) so agents can request bounded client introspection and (optionally) side-effecting actions deterministically.
- [x] Add a scriptable client RPC kind (`rpc.kind=script_eval`) so the agent can send task-specific “probe code” (killable worker + API bridge).
- [x] Add `rpc.kind=dom_apply` to support DOM create/edit/delete/dispatch as a first-class client-side side-effect surface.
- [x] Add `rpc.kind=entity_apply/entity_query` to support client-agnostic scene entities (create/update/delete/action/query), with a WebUI implementation (`SceneView` + `canvas2d`).
- [x] Add an unsafe `page_eval` client RPC engine (explicit opt-in; reload as kill switch).
- [x] Add client-kind system prompt extensions (“client profiles”) so user prompts stay clean while the agent still knows default presentation/DoD semantics for the active collaboration surface.
- [ ] Add a higher-level browser automation layer (navigation flows + wait/assert primitives; possibly via Playwright/CDP) built on top of client RPC.
- [x] Add a real browser E2E harness (Playwright) for validating client RPC + artifact flows against a running agentd/UI.
- [ ] Add a daemon skeleton with:
  - local control socket / HTTP
  - broker client abstraction (MQTT)
  - session routing per client
- [ ] Define an auth story (device provisioning; tokens; rotating secrets).
- [ ] Add a “Provider catalog” UX:
  - list OpenRouter models via daemon endpoint (done)
  - improve search/filter UI (context length, modalities, provider, price range)

## Mid-term (multimodal + edge)

- [ ] Extend message model to support **content parts** (text/image/audio/video URL or bytes).
- [ ] Provide streaming I/O hooks (audio in/out) at the host layer.
- [ ] Add compile-time feature flags for MCU builds (disable persistence, disable large buffers).
- [x] Add a dedicated `camera_capture` host tool (single-shot) that returns an `artifact` + optional `ui_action play_audio/notify` to reduce capture loops.

## Hygiene

- [x] Add `.gitignore` for `build/`, logs, and local session artifacts.
- [x] Add a local Web UI scaffold (`ui/`) to exercise daemon RPC day 1.
- [x] Refactor oversized `daemon/src/main.cpp` into smaller modules (job manager, JSON/HTTP helpers, etc.).
- [x] Deduplicate agentd smoke scripts via shared bash lib (`tests/lib/agentd_smoke_lib.sh`).
- [x] Add `tools/verify.sh`/`tools/publish.sh` for consistent build+test(+push) workflow with logs.
- [x] Add key-free local daemon smokes for critical UI paths (async jobs, SSE, streamed assistant deltas, session audit) using stub OpenAI servers.
- [x] Add an optional SQLite troubleshooting store for agentd (mirror sessions/runs/events/tool records) (`docs/DB.md`).
- [x] Mirror explicit `artifact` events into the SQLite store (schema v2) for debugging “runaway capture” loops.
- [x] Add daemon endpoints to query SQLite directly (runs/events/artifacts) for UI-native troubleshooting views.
- [x] Parse DB event/artifact JSON into structured payloads in `/api/v1/db/run` (still includes raw `*_json` fields).
- [x] Parse `tool_records.arguments_json` and `tool_records.result_text` into structured payloads in `/api/v1/db/run`.
- [x] Support `include_ui_actions=1` in `/api/v1/db/run` so a single run view can show artifacts + UI actions together.
- [x] Configure a git remote so `git push` works in this workspace (origin set; `git push` succeeds).
