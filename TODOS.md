# TODOs

This file tracks the next highest-leverage tasks to reach the goal described in `project.md`, using `DESIGN.md` as the spec (which may evolve).

## Near-term (core correctness + portability)

- [x] Define a stable **LLM provider interface** in the core (no HTTP/JSON assumptions).
- [x] Define a stable **tool registry + executor interface** in the core (host-pluggable tools: GPIO, sensors, etc.).
- [ ] Move the **tool-call loop** into core so daemon/embedded can reuse it (host provides JSON + transport adapters).
- [x] Implement a core **runner** that:
  - compacts session before calls (char budget)
  - calls provider
  - appends assistant text to session
- [ ] Add a persistence interface (optional) so host apps can supply file/SQLite/NVS implementations.
- [ ] Expand unit tests for compaction edge cases (overlap prefix/suffix; empty; all system messages).
- [x] Add a small unit test for tool-loop compaction/rotation logic (JSON message arrays) to prevent regressions.
- [x] Add a unit test to ensure tool result truncation stays JSON-shaped and capped.

## Near-term (CLI ergonomics)

- [x] Add `agent chat` REPL mode (session-backed, incremental turns).
- [x] Improve error messages (HTTP status + provider error extraction).
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
- [x] Add more network smokes for host tools beyond `shell_exec` (e.g. `fs_read` paging) to reduce regressions.
- [x] Add a host-tool network smoke for `text_search` (DeepSeek + daemon) to reduce regressions.
- [x] Improve host filesystem tools defaults to avoid noise:
  - [x] exclude `node_modules`/`build`/`dist` by default in `fs_list` (configurable)
  - [ ] consider optional `respect_gitignore`/`exclude_globs` (future)

## Mid-term (daemon + broker)

- [x] Add a minimal local daemon (`agentd`) with HTTP+JSON endpoints for a Web UI client.
- [x] Add a structured event log (`events`) for UIs (LLM + tool timeline).
- [x] Expand daemon RPC surface:
  - list sessions, get session, delete session
  - fetch per-run audit entries
- [ ] Add streaming responses (SSE/WebSocket) for UI responsiveness.
  - [x] stream job events via SSE (`GET /api/v1/job/stream`) for responsive UIs
  - [~] stream assistant token deltas (provider-dependent)
    - [x] `tools=none`: daemon can emit `assistant_delta` events when `stream_assistant=true`
    - [x] `tools=none`: retry/rotate context on “too long” errors (reduces perceived hangs)
    - [ ] `tools=basic/host`: stream while still supporting tool calls (harder)
  - make event log fully structured (typed payloads; truncation metadata; stable schema)
- [x] Add async runs (`/api/v1/run_async` + `/api/v1/job`) so UIs can poll long-running jobs without blocking.
- [x] Add job cancellation (`POST /api/v1/job/cancel`) and UI Cancel button (cooperative; kills host subprocess tools).
- [x] Add live job progress via event tailing:
  - `GET /api/v1/job?job_id=...&include_events=1&cursor=...` (cursor-based incremental event polling).
- [x] Emit lightweight `heartbeat` events for async jobs during long tool execution / slow providers (avoids "hang" perception).
- [ ] Add a daemon-level “sandbox policy” model:
  - YOLO vs host-scoped tools-root
  - future: per-tool allow/deny and command restrictions for `proc_exec` / `shell_exec`
  - [x] add `/api/v1/tools` for clients to query active tool schemas

## Near-term (UI polish)

- [x] Render assistant output as Markdown (GFM).
- [x] Render code blocks with syntax highlighting in the UI (Markdown + highlight.js).
- [x] Render tool outputs with a JSON-aware view (diff + stdout/stderr previews).
- [x] Render runs as a conversation (message cards) derived from `events` (user → assistant → tool calls/results).
- [x] Persist UI settings in `localStorage` (refresh-safe) and allow toggling the settings panel.
- [x] Keep the last completed run visible during async runs and transient fetch/SSE errors (avoid “history wiped” perception).
- [x] Add a daemon file endpoint for UI previews (`/api/v1/file`) for images/audio/video artifacts.
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

## Hygiene

- [x] Add `.gitignore` for `build/`, logs, and local session artifacts.
- [x] Add a local Web UI scaffold (`ui/`) to exercise daemon RPC day 1.
