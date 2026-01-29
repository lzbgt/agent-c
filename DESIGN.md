# Agent Project – Architecture & Design (Draft)

Date: 2026-01-29

## Goals

- **Fast startup + small footprint** compared with heavy Python stacks.
- **OpenAI-compatible backends** (OpenAI, OpenRouter, DeepSeek, Gemini via routing, etc.) by using the OpenAI Chat Completions API shape (or compatible).
- **Seamless compaction** so long-running sessions remain usable within context limits.
- **Multi-runtime portability**
  - Desktop CLI / daemon: full features (env/config, persistence, broker connectivity).
  - Embedded (ESP32-class) and VM targets (Oren AVM): minimal assumptions (no env, limited storage).

## Non-Goals (for the first milestone)

- Full tool ecosystem (shell, filesystem, browser automation).
- Full streaming / incremental tool call protocol coverage.
- Guaranteed token-accurate budgeting for every model (tokenizers are model-specific and often heavy).

## Key Decisions (Facts / Constraints)

1) **Core library must not depend on environment variables**
   - Many runtimes (embedded MCU, VM) may not expose `getenv()` meaningfully or at all.
   - Therefore: env parsing is an adapter/host concern (CLI/daemon), not a core concern.

2) **Session storage is optional in core, mandatory in host apps**
   - CLI/daemon benefit from persisted sessions; embedded often cannot afford large histories.
   - Therefore: core owns the in-memory *session model*, while persistence is provided by the host.

3) **Transport is an injected interface**
   - Embedded environments may not support libcurl/POSIX sockets.
   - Therefore: core defines a transport “port” (function pointers / vtable).
   - The CLI provides a libcurl-backed implementation; embedded provides its own.

   Milestone 1 scope: the core stays strictly network-agnostic; the desktop CLI owns the HTTP client.
   The transport “port” is a planned addition once the request/streaming surface is stabilized.

4) **Compaction is policy-driven and supports multiple budgeting strategies**
   - Token counting is model-specific and may require heavy tokenizer libraries.
   - Therefore: core supports a portable **character-budget** strategy by default, and can accept a
   host-provided token counter in the future.

5) **Provider APIs are usually stateless; “session id” is a client concern**
   - OpenAI-compatible Chat Completions APIs typically do not store conversation state on the server.
   - DeepSeek explicitly documents `/chat/completions` as “stateless”: clients must concatenate and resend prior messages.
   - Therefore our `session_id` is a **local agent/session store concept**, not a provider feature.
   - Some providers offer **context caching** (prefix-cache / KV-cache) which reduces cost/latency when request prefixes repeat,
     but this is not the same as a server-side session handle.

## Layering Overview

### 1) `agent_core` (portable core, C API)

Responsibilities:
- Session model: messages, roles, timestamps (optional), metadata.
- Compaction policy: decide when to compact and what to keep (portable char budget).
- Deterministic prompt assembly decisions (ordering, pinned messages).
- Tool abstractions: portable tool registry + host-provided tool executor callbacks.
- No assumptions about env, filesystem, argv, network availability.

Prohibitions:
- No `getenv`, no reading `.env` / `~/.config`, no CLI flag parsing.
- No mandatory libcurl / sockets dependency.
- No mandatory persistence dependency (SQLite/files).

Interfaces exported:
- `agent_session_*`: create/destroy session, append messages, compact, iterate messages.
- Optional hooks for custom allocators (useful for embedded).

### 2) `agent_cli` (desktop host adapter)

Responsibilities:
- Parse flags + environment variables + config files (policy owned by CLI).
- Provide transport implementation (libcurl).
- Provide persistence implementation (file-based session store initially; can evolve to SQLite).
- Call core APIs to manage session and compaction.
- Provide host toolsets (filesystem operations, process execution) for tool-calling models.

### 3) Future host adapters

- `agent_daemon`: long-running broker-connected service (MQTT or similar), remote auth, remote clients.
- `agent_embed`: bindings for ESP32 / Oren AVM with minimal storage and transport.

## Day-1 Product Direction (Decision)

We prioritize a **daemon-first** architecture with multiple clients:

- `agentd` (daemon) is the **source of truth** for:
  - session persistence and compaction policy
  - tool loop execution and tool plugins
  - transcript/audit logging (LLM requests, responses, tool calls, tool outputs)
  - optional broker connectivity (future)
- Clients are replaceable front-ends:
  - `agent` CLI is a **thin client** (plus a useful debug surface).
  - Local Web UI is the **primary “beautiful UX”** surface (rich interactions, diff views, filtering, etc.).

Rationale:
- Terminal TUIs are expensive to make “feature-rich” (diff viewers, tool panes, multimodal previews, search/filter).
- A browser UI makes rich inspection and editing workflows much easier.
- A daemon aligns naturally with the future goal of outbound broker connectivity behind NAT.

## Data Model

### Message

- `role`: one of `system`, `user`, `assistant`, `tool` (extensible).
- `content`: UTF-8 text (first milestone).
- `name` (optional): for tool/function naming in compatible formats.

### Session

- Ordered list of messages.
- “Pinned” system messages supported by convention (first messages).

## Compaction (First Milestone)

### Trigger

- When `estimated_chars(messages) > max_chars`, compact.

### Action (default)

- Preserve:
  - All leading `system` messages (pinned) OR at minimum the first system message.
  - The last `K` messages (configurable).
- Drop older non-pinned messages until the budget is satisfied.

### Tool-loop compaction (host-side, day-1)

The tool-call loop uses the same char-budget idea, but performs compaction at the **request assembly** layer:

- Keep pinned leading `system` messages.
- Keep the last `K` messages.
- Drop the middle window when over budget.
- Optionally insert a deterministic `system` “compaction summary” message describing what was dropped (no extra LLM call).

This mirrors the approach in `ref/ds-cli` (Sophon) where compaction summaries are lightweight and do not require a second model call.

### Per-request context hygiene (host-side)

Compaction only triggers when the history exceeds a threshold. In practice, **tool outputs** can bloat the context quickly even
before compaction triggers (e.g. repeated `rg`/`cat` output, large diffs, verbose build logs).

Therefore the host tool loop applies additional per-request guardrails:
- Tool results are **capped** before being appended into the next LLM request context (still preserving JSON envelope shape when possible).
- Full tool outputs remain available in the daemon/UI event log and audit trail, but the model only sees a bounded excerpt.

### “Session rotation” when context runs out

Because most providers are stateless, “spawning a new provider session” is implemented as **session rotation**:

- When the assembled request exceeds the configured context budget (or the provider rejects it as too large),
  the agent compacts more aggressively and retries.
- This produces a new “epoch” of context: pinned system prefix + one compaction-summary + last `K` messages.
- The tool-loop emits `compaction` events with an `epoch` counter so UIs can display when rotations happen.

Host implementation notes (day-1):
- The daemon and CLI treat “context too long” as a best-effort heuristic (`openai_is_context_too_long_error(...)`)
  based on HTTP status (e.g. `413`) and error message substrings (“maximum context”, “too many tokens”, etc.).
- For `tools="none"` runs (both non-streaming and `stream_assistant=true`), the host applies the same idea:
  on a context-too-long rejection it retries up to 2 times, each time reducing `max_chars` (≈ 3/4) and compacting again.
  This is functionally equivalent to “spawning a new provider session” on stateless APIs.

Note on provider caching:
- DeepSeek supports automatic “context caching” for repeated request **prefixes** (KV-cache on disk).
- Our compaction policy preserves pinned `system` prefixes and (after a compaction event) a stable summary prefix,
  so subsequent turns can still benefit from prefix cache hits.

### Event payload size limits (UI stability)

Some event fields (especially `llm_request.request_json` and `llm_response.response_body`) can become extremely large
when verbose tracing is enabled. To keep the Web UI responsive:
- Event payloads are **bounded** (truncated with a `*_truncated` flag) for UI transport.
- Full fidelity request/response bodies remain available in the per-run `trace_text` transcript.

### Optional summary insertion (future-friendly)

- The host may generate a summary (possibly via an LLM call) and insert it as a `system` message:
  - `system`: "Session summary: …"
- Core supports “insert summary then prune more aggressively”.

## API Shape (C-friendly, stable ABI)

- Core exposes a C header (`core/include/agent/agent.h`) usable from C/C++.
- Core avoids C++ types across the boundary.

## Build / Packaging

- CMake-based project:
  - `agent_core` as a static library.
  - `agent` as a CLI executable.
- CLI links libcurl; JSON parsing in the CLI only (core remains JSON-agnostic).

## Milestone 1 Deliverables

- Minimal `agent` CLI:
  - One-shot prompt (`agent run "..."`)
  - Optional session (`--session <id>`) with persistence
  - Optional compaction (`--max-chars`, `--keep-last`)
  - OpenAI-compatible chat completion call (`/v1/chat/completions`)
- `agent_core` session + compaction APIs with unit/smoke tests.

## Tools (Host vs Embedded)

### Core tool contracts

- Core stores tool definitions in a portable registry (`name`, `description`, `parameters_json`).
- Tool execution is a host callback: `execute(ctx, tool_name, arguments_json) -> result_string`.
- Core does not assume tools correspond to shell commands; tools can represent:
  - CLI/daemon: filesystem, subprocess execution, network requests, code search, etc.
  - Embedded: GPIO output, I2C/SPI transactions, camera capture, audio playback, BLE scan, etc.

### Host toolset (CLI/daemon)

The CLI/daemon toolset is designed around **OS-native tooling** plus **bounded filesystem inspection**:
- For **inspection** (read/list/stat), prefer bounded filesystem tools:
  - `fs_stat`: metadata (exists/type/size/mtime/binary hint).
  - `fs_list`: bounded directory listing (supports recursion with depth/entry caps).
  - `fs_read`: bounded text reads with line-based pagination (`start_line`, `max_lines`, optional `end_line`).
  These exist primarily to control token usage and avoid “cat the world” context blow-ups.
- For everything else, use `proc_exec` / `shell_exec` to run system-installed binaries for project inspection and file operations
  (`rg`, `find`, `git`, language toolchains, etc.).
- Use a dedicated diff-based editing tool (`file_apply_patch`) so file edits are auditable in the transcript
  (the tool result includes the unified diff that was applied).

Host default system hint (policy):
- When using the host toolset, host apps inject a one-time `system` message into an empty session to encourage
  **incremental inspection** (e.g. `rg/grep`, `head`, `tail`, `awk`, `sed -n`, and `fs_read` paging) instead of reading entire files.
- This is host policy only (not a core concept) and can be disabled/overridden by the host or client.

`--tools-root` (when set) is primarily used to control the working directory for `file_apply_patch`
(via `git -C <root> apply ...`). It is not intended as a strong security boundary (since `proc_exec`
can still invoke arbitrary commands in a YOLO host configuration).

Tool transcript persistence (day-1 pragmatic choice):
- The OpenAI tool-calling message schema uses fields like `tool_call_id` that are not yet part of the portable core session model.
- Therefore, CLI/daemon persist tool usage into the session as **assistant text markers**:
  - `[tool_call] name=... id=...` + args JSON
  - `[tool_result] name=... id=...` + tool output string
- This keeps sessions provider-compatible (no malformed `tool` role messages) while still preserving useful context for later turns.

### Tool success semantics (important)

Tool success is not solely a return/exit code:
- For host tools like `shell_exec`, a non-zero exit code can still produce useful output (or partial progress),
  while exit code 0 may still contain warnings/errors that matter (e.g. `pip install` logs).
- For embedded tools like GPIO/I2C, “success” may depend on returned sensor readings or device state.

Therefore tools should return **structured results** (recommended JSON envelope):
- `ok` (boolean) is a hint only
- `error` (string, optional) describes execution-level failure
- `data` (object) contains the raw tool output (stdout/stderr text, state fields, measurements, etc.)

The LLM (and higher-level agent policy) should judge success based on the returned `data`.

## Local RPC / API (Daemon ↔ UI)

Day-1 RPC goals:
- Simple, debuggable, OpenAPI-friendly.
- Works for local Web UI and future remote/broker bridging.

Decision:
- Use **HTTP + JSON** locally (bind to `127.0.0.1` by default).
- Version endpoints under `/api/v1/...`.

Initial endpoints (implemented in `agentd`):
- `GET /api/v1/health` → `{ ok, service, version }`
- `GET /api/v1/tools` → returns the active tool registry (`name`, `description`, `parameters_json`) for the requested toolset.
- `GET /api/v1/openrouter/models` → fetches + filters OpenRouter’s model catalog (for picking a cheap verification model).
- `POST /api/v1/run` → runs one user prompt against an LLM backend with optional tool loop.
- `POST /api/v1/run_async` → starts a background run and returns `{ ok, job_id }` (UI polls job status).
- `GET /api/v1/job?job_id=...` → returns `{ ok, status, result? }` for async runs.
  - Progress polling for UIs:
    - `include_events=1` to include tool/LLM events captured so far (best-effort).
    - `cursor=<n>&max_events=<m>` to tail events incrementally (cursor-based pagination).
- `GET /api/v1/job/stream?job_id=...&cursor=...` → streams events as Server-Sent Events (SSE) for responsive UIs.
- `GET /api/v1/file?path=...&yolo=0|1` → returns a file for UI preview (images/audio/video/text; size capped).
- `GET /api/v1/sessions` → list session ids.
- `GET /api/v1/session?session_id=...` → get session messages.
- `DELETE /api/v1/session?session_id=...` → delete a session (messages + audit log).
- `GET /api/v1/session/audit?session_id=...` → fetch recent per-run audit entries (JSONL parsed to array).

`POST /api/v1/run` request (JSON):
- `prompt` (string, required)
- `session_id` (string, default `"default"`)
- `no_session` (bool, default `false`)
- `model`, `base_url`, `api_key` (optional overrides; if omitted, daemon uses env/config)
- `timeout_ms` (number; optional; provider HTTP timeout for this run)
- `stream_assistant` (bool; optional; when `tools="none"`, request SSE streaming and emit `assistant_delta` events)
- `tools` (`"host"|"basic"|"none"`, default `"host"`)
- `tools_root` (string; used for diff-based edits in host tools)
  - `""` or `@cwd` means unrestricted (current working directory)
  - `@host` means “daemon host scope” (configured by daemon, typically project/workspace root)
- `max_steps` (number; `0` means unlimited)
- `trace` (bool; include transcript text)
- `yolo` (bool; if true, disables tool scoping and forces unrestricted mode)
- `verbose` (bool; if true, captures full tool outputs and raw request/response bodies into `events`)

Response (JSON):
- `ok` (bool)
- `assistant_text` (string)
- `trace_text` (string; full transcript between daemon↔LLM↔tools)
- `http_status`, `http_body` (best-effort diagnostics)
- `error` (best-effort message)
- `effective_yolo` (bool) and `effective_tools_root` (string) so clients can display what actually applied.
- `effective_timeout_ms` (number) so clients can display the provider timeout that actually applied.
- `effective_stream_assistant` (bool) so clients can display whether assistant streaming was requested.
- `events` (array; structured event log for UIs)

Note on “thinking process”:
- The UI can display **tool calls/results and request/response transcripts**.
- We do **not** expose hidden chain-of-thought. If a model includes reasoning in the visible assistant message, it will be displayed as part of the assistant content.

Event log sizing:
- Per-event large fields are truncated (best-effort).
- The overall event log has a maximum event count and capture budget; if exceeded the final `end` event includes `truncated=true`.

Assistant streaming (provider-dependent):
- When `tools="none"`, clients may request `stream_assistant=true` to have the daemon use OpenAI-compatible SSE streaming
  (`stream: true`) and emit incremental `assistant_delta` events while the request is in-flight.
- Tool-calling loops (`tools="basic"`/`"host"`) still use non-streaming calls in milestone 1; streaming tool calls requires
  reconstructing tool-call JSON incrementally and is deferred.

Security notes (future):
- Binding to `127.0.0.1` avoids LAN exposure by default.
- Once broker/remote control is added, the daemon must implement an auth story (device provisioning, rotating tokens).
