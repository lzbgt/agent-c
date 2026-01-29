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

## Near-term (CLI ergonomics)

- [x] Add `agent chat` REPL mode (session-backed, incremental turns).
- [x] Improve error messages (HTTP status + provider error extraction).
- [ ] Add `--summary-model` option (host-generated summary insertion).
- [x] Implement extensible host toolsets (subprocess exec + diff-based file edits) for CLI/daemon (`--tools host`).
- [ ] Decide how to persist **tool-call transcripts** (tool_call_id + tool messages) without breaking OpenAI-compatible request formats.
- [x] Make tool-loop `--max-steps` default unlimited (0 means unlimited).

## Mid-term (daemon + broker)

- [x] Add a minimal local daemon (`agentd`) with HTTP+JSON endpoints for a Web UI client.
- [x] Add a structured event log (`events`) for UIs (LLM + tool timeline).
- [ ] Expand daemon RPC surface:
  - list sessions, get session, delete session
  - streaming responses (SSE/WebSocket)
  - make event log fully structured (typed payloads; truncation metadata; stable schema)
- [ ] Add a daemon-level “sandbox policy” model:
  - YOLO vs host-scoped tools-root
  - future: per-tool allow/deny and command restrictions for `proc_exec` / `shell_exec`

## Near-term (UI polish)

- [x] Render assistant output as Markdown (GFM).
- [x] Render tool outputs with a JSON-aware view (diff + stdout/stderr previews).
- [x] Add a daemon file endpoint for UI previews (`/api/v1/file`) for images/audio/video artifacts.
- [ ] Add a daemon skeleton with:
  - local control socket / HTTP
  - broker client abstraction (MQTT)
  - session routing per client
- [ ] Define an auth story (device provisioning; tokens; rotating secrets).

## Mid-term (multimodal + edge)

- [ ] Extend message model to support **content parts** (text/image/audio/video URL or bytes).
- [ ] Provide streaming I/O hooks (audio in/out) at the host layer.
- [ ] Add compile-time feature flags for MCU builds (disable persistence, disable large buffers).

## Hygiene

- [x] Add `.gitignore` for `build/`, logs, and local session artifacts.
- [x] Add a local Web UI scaffold (`ui/`) to exercise daemon RPC day 1.
