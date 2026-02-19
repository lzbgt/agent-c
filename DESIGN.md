# Agent Project – Architecture & Design

Last updated: 2026-02-19

## Goals

- **Fast startup + small footprint** compared with heavy Python stacks.
- **OpenAI-compatible backends** (OpenAI, OpenRouter, DeepSeek, Gemini via routing, etc.) by using the OpenAI Chat Completions API shape (or compatible).
- **Seamless compaction** so long-running sessions remain usable within context limits.
- **Multi-runtime portability**
  - Desktop CLI / daemon: full features (env/config, persistence, broker connectivity).
  - Embedded (ESP32-class) and VM targets (Oren AVM): minimal assumptions (no env, limited storage).
- Optional deterministic “policy VM” layer (future): a small VM/runtime can execute routing/retry/aggregation logic without LLM calls.
  - Integration contract (draft): `docs/spec/agent_vm_port_v0.md`
  - Ecosystem leverage notes: `docs/OREN_LANG_ECOSYSTEM.md`

## Additional goals (for the first milestone)

- Full tool ecosystem (shell, filesystem, browser automation).
- Full streaming / incremental tool call protocol coverage.
- Guaranteed token-accurate budgeting for every model (tokenizers are model-specific and often heavy).

## Promoted goals (formerly non-goals)

There are no “non-goals” in this project. Anything that was previously deferred is now an explicit goal tracked in the
Roadmap section below and `TODOS.md`, including:

- Staging/rollback-friendly OTA updates with explicit auditability.
- Multi-deployment fanout controls surfaced in broker + WebUI.
- Dynamic memory: observation capture + progressive disclosure (search → timeline → get).

## Design Doc Map

- `DESIGN.md` (this doc): system goals, boundaries, layering, and cross-cutting policies.
- `docs/BROKER.md`: broker relay design, trust model, and HTTP/WSS API.
- `docs/CLIENT.md`: client collaboration model (events, UI actions, DoD, client RPC).
- `docs/AGENTD_LIB.md`: embedding `agentd` in-process and tool-extension interfaces.
- `docs/PROTOCOL.md`: run/artifact protocol envelopes and semantics.
- `docs/WORKFLOWS.md`: workflow engine model and task semantics.
- `docs/STREAMING.md`: streaming compatibility matrix and behavior notes.
- `docs/MEMORY.md`: memory retention, salience, and recap architecture.
- `docs/DIAGNOSTICS.md`: diagnostics endpoints and provider health checks.
- `docs/DB.md`: daemon SQLite store, blob storage tiers, and DB query API.
- `docs/DOD_ACK.md`: UI-visible “definition of done” handshake semantics.
- `docs/spec/README.md`: index of versioned protocol/spec deep dives.

## Key Decisions (Facts / Constraints)

1) **Core library must not depend on environment variables**
   - Many runtimes (embedded MCU, VM) may not expose `getenv()` meaningfully or at all.
   - Therefore: env parsing is an adapter/host concern (CLI/daemon), not a core concern.

2) **Session storage is optional in core, mandatory in host apps**
   - CLI/daemon benefit from persisted sessions; embedded often cannot afford large histories.
   - Therefore: core owns the in-memory *session model*, while persistence is provided by the host.

   Practical compromise (Milestone 1.5):
   - Core provides a **JSON-free, line-based session codec** (`agent/session_codec.h`) so hosts can persist/restore sessions
     without a JSON library (embedded-friendly).
   - Hosts may additionally write an OpenAI-ish JSON view (`.json`) when JSONCPP is available, but the portable `.sess` format
     is treated as the primary persisted session file.

   Planned portability port (Milestone 2.1):
   - Core defines an optional **persistence interface** (`agent/persist.h`) so hosts/embedded targets can supply:
     - filesystem persistence (`.sess` on desktop)
     - SQLite (desktop/daemon)
     - NVS/flash key-value storage (MCU)
   - The core itself remains storage-agnostic and does not assume POSIX paths or env vars.

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

The stack is layered so the portable core stays env-agnostic while host services and clients evolve independently.

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

### 2) `agentd` (daemon host service)

Responsibilities:
- HTTP/SSE API and run orchestration (`/api/v1/run`, `/api/v1/job`, client events).
- Session persistence, audit logs, and artifact handling.
- Tool loop execution + tool plugins / tool servers.
- Runtime config store, safety limits, and optional broker connector.

See: `docs/CLIENT.md`, `docs/PROTOCOL.md`, `docs/TOOLS.md`.

### 3) `agent_cli` (standalone desktop client)

Responsibilities:
- Parse flags + environment variables + config files (policy owned by CLI).
- Provide transport implementation (libcurl).
- Provide persistence implementation (file-based session store; optional SQLite).
- Call core APIs to manage session and compaction.
- Provide host toolsets (filesystem operations, process execution) for tool-calling models.

### 4) Broker (optional relay / control plane)

Responsibilities:
- Agent registry, membership authZ, and audit.
- mTLS agent connectivity + OIDC/JWT client auth.
- Relay HTTP/SSE between clients and agentd deployments behind NAT.

See: `docs/BROKER.md`.

### 5) Clients (WebUI + integrations)

- WebUI: primary UX for runs, artifacts, diagnostics, and ops.
- Other clients: mobile, Slack, backend services, or thin CLIs.

See: `docs/CLIENT.md`.

### 6) Embedded / AVM adapters (future)

- `agent_embed` targets ESP32 / Oren AVM with minimal storage and transport.
- Optional policy VM to run deterministic routing/retry without LLM calls.

## System Flows (Summary)

This section consolidates the most important cross-cutting flows so the system
can be understood without hopping across multiple design docs.

### Run flow (client → agentd → UI)

- Clients submit runs to `agentd` (`/api/v1/run` or `/api/v1/run_async`).
- `agentd` executes the tool loop, persists events/audit records, and emits
  structured events (SSE) defined in `docs/spec/run-events/run_events_v1.md`.
- UI-visible effects (artifacts, UI actions) use the DoD handshake so the agent
  can deterministically stop once the UI acknowledges delivery (see `docs/DOD_ACK.md`).

### Broker relay flow (client → broker → agentd)

- Agents connect **outbound** to the broker via mTLS (`/v1/agent/connect`).
  The broker verifies the client cert against `--tls-client-ca` and matches
  the cert CN to `agent_id` (see `docs/BROKER.md`).
- Clients authenticate with OIDC/JWT bearer tokens (optional cookie auth is
  supported for browser environments).
- The broker proxies requests via:
  - `/v1/agents/{agent_id}/proxy/...` for HTTP
  - `/v1/agents/{agent_id}/proxy_sse/...` for SSE streams
- Optional idempotency keys (`Idempotency-Key` / `X-Idempotency-Key`) provide
  safe retries for proxied requests.

### Persistence & observability (agentd)

- The daemon persists sessions and run/audit records to the state root.
- When `--db-path` is enabled, it mirrors runs/events/tools/artifacts into
  SQLite and exposes read-only query endpoints (see `docs/DB.md#db-query-api-troubleshooting`).
- Diagnostics endpoints (`/api/v1/diagnostics*`) provide fast health snapshots
  and provider key presence checks (see `docs/DIAGNOSTICS.md`).

## Day-1 Product Direction (Decision)

We prioritize a **daemon-first** architecture with multiple clients:

- `agentd` (daemon) is the **source of truth** for:
  - session persistence and compaction policy
  - tool loop execution and tool plugins
  - transcript/audit logging (LLM requests, responses, tool calls, tool outputs)
  - optional broker connectivity (relay)
- Clients are replaceable front-ends:
  - Local Web UI is the **primary “beautiful UX”** surface for daemon-backed workflows (rich interactions, diff views, filtering, etc.).
  - `agent` CLI remains a **standalone host adapter** for local runs and a useful debug surface.

Rationale:
- Terminal TUIs are expensive to make “feature-rich” (diff viewers, tool panes, multimodal previews, search/filter).
- A browser UI makes rich inspection and editing workflows much easier.
- A daemon aligns naturally with the future goal of outbound broker connectivity behind NAT.

## Host Tool Sandbox Policy (Daemon/CLI)

When enabling host tools (filesystem inspection, process execution, patch application), we need an explicit safety knob that
works both for local CLI runs and for daemon deployments that may be reachable over a network.

Policy modes (first milestone):

- `full`: enable all host tools (`shell_exec`, `proc_exec`, `file_apply_patch`, `fs_*`, `text_search`).
- `readonly`: disable process execution + patch application (`shell_exec`, `proc_exec`, `file_apply_patch`), keeping only
  bounded inspection tools (`fs_*`, `text_search`).

Additional sandbox knob:

- `yolo` (daemon request/query): when disabled, the daemon **disables process execution tools**
  (`shell_exec` / `proc_exec`) even if `host_policy=full`. This prevents “readonly-ish” UI runs from still having arbitrary
  host command execution.
- `agentd` no longer enforces a runtime filesystem sandbox (`tools_root`) for host tools. For multi-tenant isolation,
  run separate daemon instances (e.g. separate containers).

Implementation notes:

- The policy should be applied at the **tool registry** layer (omit disabled tool schemas) to prevent models from discovering
  tools they are not allowed to call.
- The executor should still reject disabled tool names (defense in depth) in case a provider returns tool calls that are not
  present in the tool schema set.

## HTTP CORS Policy (Daemon)

Problem:
- The daemon serves a local Web UI and is often accessed cross-origin (e.g. UI dev server `http://localhost:5173` → daemon `http://127.0.0.1:8123`).
- CORS was previously hard-coded to `Access-Control-Allow-Origin: *` and a limited header allowlist, which is:
  - too permissive by default when binding to non-loopback; and
  - too restrictive for the UI’s `X-OpenRouter-Key` header.

Goals:
- Safe defaults that work for the local UI on loopback.
- No implicit “public API” behavior when binding to non-loopback.
- Consistent behavior across JSON endpoints and SSE endpoints, including `OPTIONS` preflight.
- Easy to reason about and test (centralized policy, minimal per-handler duplication).

Additional goals (for this milestone):
- Add cookie-based auth / `Access-Control-Allow-Credentials` support (explicitly opt-in).
- Support per-route origin policies and regex matching with a clear precedence model.

Policy:
- Default:
  - If daemon binds to loopback (`127.0.0.1`, `localhost`): enable permissive CORS (`*`) for developer ergonomics.
  - If daemon binds to a non-loopback host: CORS is disabled unless explicitly configured.
- Configuration:
  - `--cors-origin <origin|*>` (repeatable):
    - `*` enables `Access-Control-Allow-Origin: *`.
    - Otherwise, only the configured exact origins are allowed; when a request has an `Origin` header matching the allowlist,
      the daemon reflects that origin and adds `Vary: Origin`.
  - `--cors-allow-headers <csv>` defaults to at least: `Content-Type, Authorization, X-OpenRouter-Key`.
  - `--cors-allow-methods <csv>` defaults to at least: `GET, POST, DELETE, OPTIONS`.

Implementation shape:
- Introduce a `CorsConfig` and a single function that applies CORS headers given `HttpRequest` + config.
- Teach the HTTP server to dispatch `OPTIONS` requests to a configurable preflight handler so preflight responses can include
  policy-derived headers (including dynamic origin reflection).

## Daemon Endpoint Structure (Hygiene)

As `agentd` grows, keep `daemon/src/main.cpp` focused on:
- process/flag initialization
- wiring configs (CORS/sandbox/provider defaults)
- HTTP route registration

Endpoint handler logic should live in dedicated modules under `daemon/src/` to keep responsibilities small and testable:
- `*_endpoint.{h,cpp}` for JSON endpoints (`/api/v1/config`, `/api/v1/tools`, `/api/v1/file`, etc.)
- `job_stream_endpoint.{h,cpp}` for SSE endpoints
- shared helpers (auth, sandbox policy) in single-purpose headers

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
- The OpenAI-compatible host provider adapter treats “context too long” as a best-effort heuristic
  (`openai_is_context_too_long_error(...)`) based on HTTP status (e.g. `413`) and error message substrings
  (“maximum context”, “too many tokens”, etc.), and maps it to the stable core status `AGENT_ERR_CONTEXT_TOO_LONG`.
  This lets hosts (CLI/daemon) implement “session rotation” retries without duplicating provider-specific string matching.
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
- The Web UI keeps the **last completed** run visible while an async job is running, and transient fetch/SSE errors
  do not clear the conversation (reduces the perception of “hangs”).

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

## Core Tool-Loop (Milestone 2)

Goal: move the **tool-call loop** (call LLM → parse tool calls → execute tools → feed tool results → repeat) into
the **portable core** so daemon/embedded targets can reuse the same control-flow and compaction policy.

### Goals

- **Core-owned loop control-flow**: retries, step limits, and compaction are consistent across hosts.
- **Core remains transport-agnostic**: no HTTP, no libcurl, no filesystem assumptions.
- **Core remains JSON-agnostic**:
  - It does not *parse* provider-specific JSON.
  - Tool arguments and tool results are treated as opaque UTF-8 strings (often JSON envelopes).
- **Host/provider adapters stay responsible for protocol shapes**:
  - OpenAI-compatible JSON request/response formatting/parsing stays in the host adapter.
  - Embedded providers can implement other formats while still using the same core loop.

### Additional goals (for the first cut)

- Full multimodal tool-loop content parts (text+image/audio/video) in the loop transcript, with consistent encoding.
- A stable long-term event schema across UI versions, with explicit versioning and migration notes.

### Architecture

Core introduces:

- `agent_chat_message_view_t`: a richer message view used by tool providers that can represent:
  - normal text messages (`role`, `content`)
  - assistant messages that include `tool_calls[]`
  - tool result messages that include `tool_call_id`
  - optional `name` (used for non-pinned compaction summary markers)
- `agent_tool_provider_t`: a host-supplied callback interface:
  - input: message transcript views + tool registry
  - output: assistant message (`content` + optional `tool_calls[]`)
  - providers can return `AGENT_ERR_CONTEXT_TOO_LONG` to request a “rotation” retry.
- `agent_tool_loop_run(...)`: the portable loop engine:
  - applies char-budget compaction (with deterministic summary insertion)
  - calls the provider
  - executes tools via `agent_tool_executor_t`
  - caps tool outputs before appending them back into the transcript
  - retries on context-too-long errors (host/provider signals via status)

Host adapters (CLI/daemon) provide:

- An OpenAI-compatible tool provider implemented with JSONCPP:
  - builds `/chat/completions` request JSON (`tools`, `tool_choice`, tool call/result message shapes)
  - parses tool calls from the response and returns them as structured tool calls
- Optional helpers for “tool output capping” and “tool output summary” that are JSON-aware (UI-friendly),
  while the core only treats tool outputs as strings.

### Host toolset (CLI/daemon)

The CLI/daemon toolset is designed around **OS-native tooling** plus **bounded filesystem inspection**:
- For **inspection** (read/list/stat), prefer bounded filesystem tools:
  - `fs_stat`: metadata (exists/type/size/mtime/ctime and binary hint; `birthtime` on platforms that support it); optional bounded line counting for small text files.
  - `fs_list`: bounded directory listing (supports recursion with depth/entry caps; supports `exclude_globs` to skip noisy paths; supports `respect_gitignore` best-effort).
  - `fs_find`: bounded file discovery (find files/dirs with depth/result caps; supports extension filters, `exclude_globs`, and `respect_gitignore` best-effort).
  - `fs_read`: bounded text reads with line-based pagination (`start_line`, `max_lines`, optional `end_line`) plus file timestamps.
  These exist primarily to control token usage and avoid “cat the world” context blow-ups.
- For **search**, use `text_search` (bounded) instead of `grep -R` when possible; optionally restrict by `extensions` to reduce scanning.
- For everything else, use `proc_exec` / `shell_exec` to run system-installed binaries for project inspection and file operations
  (`rg`, `find`, `git`, language toolchains, etc.).
- Use a dedicated diff-based editing tool (`file_apply_patch`) so file edits are auditable in the transcript
  (the tool result includes the unified diff that was applied).

Host default system hint (policy):
- When using the host toolset, host apps inject a one-time `system` message into an empty session to encourage
  **incremental inspection** (e.g. `rg/grep`, `head`, `tail`, `awk`, `sed -n`, and `fs_read` paging) instead of reading entire files.
- This is host policy only (not a core concept) and can be disabled/overridden by the host or client.

CLI-only: `--tools-root` (when set) is primarily used to control the working directory for `file_apply_patch`
(via `git -C <root> apply ...`). It is not intended as a strong security boundary (since `proc_exec`
can still invoke arbitrary commands in a YOLO host configuration).

Daemon note: `agentd` does not expose `tools_root` as an API parameter; session isolation is expected to be container/process-level.

Tool transcript persistence (host-only, day-1 pragmatic choice):
- The portable core session model is intentionally minimal and does not yet represent OpenAI tool-call metadata
  like `tool_call_id` and structured `tool` role messages.
- For host apps, the correct place to store full tool timelines is a **per-session audit log** (JSONL):
  - CLI writes per-run audit records to `~/.agent/sessions/<session>.events.jsonl`
  - `agentd` stores canonical state in SQLite (`docs/DB.md`) and exposes structured endpoints for history/replay.
  - Each record includes the prompt, final assistant text, and the structured `events` timeline (tool calls/results, LLM I/O).
- Session **messages** are kept clean (user/assistant content only), so future turns are not polluted by verbose tool output.
  This reduces token usage and avoids context blowups when switching between `tools=host` and `tools=none`.

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
- `POST /api/v1/session/new` → create a new unique session id (multi-client safe).
- `GET /api/v1/session?session_id=...` → get session messages.
- `DELETE /api/v1/session?session_id=...` → delete a session (messages + audit log).
- `GET /api/v1/session/audit?session_id=...` → fetch recent per-run audit entries (JSONL parsed to array).

Job lifecycle (daemon robustness):
- Async jobs (`run_async`) maintain an in-memory `JobState` with a bounded event ring buffer for UI progress.
- To keep the daemon safe for long-running usage, finished jobs are garbage-collected:
  - keep only a bounded number of recent jobs
  - drop jobs that have been done/error for longer than a TTL
  - cancellation flags are cooperative and checked at safe boundaries

Daemon authentication (optional, recommended when exposed beyond localhost):
- By default `agentd` binds to `127.0.0.1` and does not require auth.
- If `--auth-token` (or env `AGENTD_AUTH_TOKEN`) is set, daemon endpoints require:
  - `Authorization: Bearer <token>`
- This token is *daemon control-plane auth* and is distinct from provider API keys.
  - Provider keys are passed via request JSON (e.g. `api_key`) or via provider-specific headers (e.g. `X-OpenRouter-Key`).

`POST /api/v1/run` request (JSON):
- `prompt` (string, required)
- `session_id` (string, default `"default"`)
- `no_session` (bool, default `false`)
- `model`, `base_url`, `api_key` (optional overrides; if omitted, daemon uses env/config)
- `timeout_ms` (number; optional; provider HTTP timeout for this run)
- `stream_assistant` (bool; optional; request OpenAI-compatible SSE streaming (`stream: true`) and emit incremental `assistant_delta` events)
- `tools` (`"host"|"basic"|"none"`, default `"host"`)
- `max_steps` (number; `0` means unlimited)
- `trace` (bool; include transcript text)
- `yolo` (bool; if true, enables process execution tools; if false, exec tools are omitted)
- `verbose` (bool; if true, captures full tool outputs and raw request/response bodies into `events`)

Response (JSON):
- `ok` (bool)
- `assistant_text` (string)
- `trace_text` (string; full transcript between daemon↔LLM↔tools)
- `http_status`, `http_body` (best-effort diagnostics)
- `error` (best-effort message)
- `effective_yolo` (bool) so clients can display whether process execution tools were enabled.
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
- When `stream_assistant=true`, the daemon requests OpenAI-compatible SSE streaming (`stream: true`) and emits incremental
  `assistant_delta` events while the request is in-flight.
- For `tools="none"`, this provides streaming tokens for the assistant message.
- For tool-calling loops (`tools="basic"`/`"host"`), the host tool-provider reconstructs tool calls incrementally from streaming
  `delta.tool_calls` (best-effort; also supports legacy `delta.function_call`) and may also emit `assistant_delta` events on the
  final assistant step (provider-dependent).
  - Goal for the first cut: full coverage of every streaming variant across providers (with fallback when some ignore `stream: true`).
  - `assistant_delta` event payloads include:
    - `delta` (string): incremental assistant text
    - `step` (number): tool-loop step (0 for `tools="none"`)
    - `epoch` (number): context-rotation epoch / streaming attempt

Security notes (future):
- Binding to `127.0.0.1` avoids LAN exposure by default.
- Once broker/remote control is added, the daemon must implement an auth story (device provisioning, rotating tokens).

## Roadmap & Future Work (Next-gen)

This section consolidates the former `docs/NEXT_GEN.md` into this document. It captures future-proofing work for the full
stack:
- `agent_core` (portable core library)
- `agentd` (daemon + durable workflows)
- broker (secure relay / NAT traversal)
- WebUI (primary UX surface)

It is grounded in **current repo facts** (documented below) and proposes **design + implementation work** to make the
system more advanced, solid, efficient, and long-lived.

---

### Baseline facts (from this repo today)

#### agent_core
- C API with no environment-variable dependency; host handles env/flags.
- Session model + character-budget compaction (portable, tokenizer-free).
- Optional persistence interface (`agent/persist.h`) and portable `.sess` codec.
- Tool registry + host callback execution interface.

#### agentd
- Built as `agentd_lib` + thin `agentd` executable (optional embedded use).
- HTTP + SSE endpoints for runs, jobs, workflows, traces, diagnostics, etc.
- Durable workflows: DAG scheduling, retries/backoff, fairness, admission control, and restart recovery.
- Tool plugins (`--tool-plugin`) and out-of-process tool servers (`--tool-server-cmd`).
- SQLite-backed durable state (sessions, workflows, traces, memory, edge interop).
- Config store and runtime update endpoints (`/api/v1/config`, `/api/v1/config/update`).

#### broker
- Public control plane with OIDC/JWT for clients and mTLS for agent connectors.
- WebSocket connector (`/v1/agent/connect`) + HTTP proxy + SSE relay.
- Postgres-backed registry + membership audit.

#### WebUI
- React + Vite UI consuming agentd HTTP + SSE.
- Diagnostics panel, broker console, run settings, trace lookup.
- Playwright E2E tests (agentd host + broker flows).

---

### Target properties (future-proofing goals)

1) **Determinism & replayability**
   - Any run/workflow should be reproducible from its audit trail.
   - Replay should be possible without special provider state.

2) **Explicit contracts & versioning**
   - Every interop surface (agentd ↔ WebUI, broker ↔ connector, agentd ↔ agentd) should negotiate versions + capabilities.
   - Schema validation should be first-class and enforced in CI.

3) **Transport independence**
   - HTTP/SSE remains default, but agentd endpoints should be transport-agnostic by design.
   - Broker should be able to relay new transports without changing agentd handlers.

4) **Operational safety under load**
   - Strong backpressure, fairness, and admission control across the stack.
   - Clear, deterministic limits for tool loops, payload sizes, and long-running streams.

5) **Security with minimal friction**
   - Zero-trust posture for any non-local usage (auth, audit, safe defaults).
   - Non-interactive credential loading, explicit origin controls, and reliable policy enforcement.

6) **Composable intelligence**
   - Deterministic task graph + policy VM + tool servers = layered autonomy.
   - Provide a stable foundation for orchestration, verification, and collaboration.

7) **End-to-end security + identity**
   - First-class PKI/provisioning for nodes and connectors (not just hooks).
   - Signed capability manifests and run attestations with verifiable hashes.

8) **Real-time media support**
   - Audio streaming as a native workflow primitive (Opus/WebRTC transport).
   - Low-latency bidirectional media for “talk + tool” flows.

9) **Decentralized coordination**
   - Optional node consensus so clusters can negotiate locally when the platform is absent.
   - Explicit conflict resolution and safety policies for multi-node decisions.

---

### Promoted goals (explicit)

This project does **not** treat any “formerly deferred” items as non-goals. The following areas are **explicit goals**
tracked in `TODOS.md` as promoted workstreams:

- Tool loop: full multimodal transcript support + stable, versioned event schema with migrations.
- Streaming: core-layer streaming interface + provider compatibility matrix with full variant coverage.
- Audio streaming: Opus/WebRTC voice pipeline + broker relay + UI voice session controls.
- Tool plugins: sandbox/isolation, Windows loader, and embedded/MCU-compatible plugin path.
- Storage/analytics: DB query API as canonical surface, analytics layer, and binary blob storage tiers
  (see `docs/DB.md#blob-storage-tiers-design--status`).
- UI actions: stable public action API, autoplay unlock flow, and consented remote URL opens.
- Memory: observation capture + progressive disclosure retrieval flow (search → timeline → get).
- Interop/attestation: PKI provisioning + signed manifests/attestations + canonical JSON hashing + envelope confidentiality.
- AVM: scoped flag passthrough, host-effects policy, record/replay plumbing, and quorum/attestation.
- Node consensus: decentralized coordination protocol with conflict resolution + deterministic simulation tests.

---

### Architecture proposals (grounded, phased)

#### 1) agent_core (portable core)

**1.1 ABI-stable C API v1**
- Define an explicit ABI surface with versioned structs and sizes.
- Add `agent_core_version()` + `agent_core_caps()` to expose compiled features.

**1.2 Deterministic serialization boundary**
- Provide canonical JSON/CBOR encoding for session snapshots and tool call records.
- Use the existing `agent_json_c14n` and CBOR helpers to ensure stable hashing.

**1.3 Memory discipline for embedded**
- Optional arena allocator + bounded growth for session storage.
- Provide a compile-time `AGENT_CORE_LIMITS` block for MCU targets.

**1.4 Cross-runtime tool schema linting**
- Add a core helper to validate tool schema JSON (basic structural checks) before registering.

#### 2) agentd (daemon + workflows)

**2.1 Protocol capability negotiation**
- Add `GET /api/v1/caps` with:
  - protocol versions, feature flags, max limits, and enabled tool modes.
- WebUI and brokers should **fail fast** if required caps are missing.

**2.2 Replay-grade audit trail**
- Standardize a single event schema for run + workflow events.
- Add “replay bundles” (inputs + deterministic hashes) so a run can be re-executed.

**2.3 Deterministic tool-loop envelopes**
- Persist tool call inputs/outputs with stable hashing and bounded truncation metadata.
- Make replay validation part of CI for a small deterministic fixture set.

**2.4 Unified policy VM hook**
- Expose a deterministic pre/post hook interface (policy VM or rules engine) for:
  - tool allow/deny, run shaping, budget gating, retry policies.
- Keep it optional and sandboxed (out-of-process or limited VM).

**2.5 Performance & resiliency**
- Adopt a multi-queue scheduler that can separate “interactive” vs “batch” workloads.
- Add a persistent “work queue watermark” to enforce submission backpressure.

#### 3) broker (control plane + relay)

**3.1 Durable relay envelopes**
- Store in-flight relay metadata (request id, trace id, status) for audit/replay.
- Idempotency keys for proxy/orchestrate to make retries safe (implemented).

**3.2 Multi-transport relay layer**
- Keep WebSocket but add a transport-agnostic relay interface so new transports
  (e.g., gRPC or WebTransport) can be introduced without changing business logic.

**3.3 Broker-side policy hooks**
- Central policy checks (rate limits, allowlists, cost ceilings) before relay.
- Provide a policy audit stream (SSE) for operator visibility.

**3.4 Security & provisioning (broker + connector)**
- Establish a device provisioning flow: issue/rotate device certs, bind to `agent_id`, and record provenance.
- Store signed capability manifests (and hashes) so clients can verify device identity and capabilities.
- Add revocation lists and short-lived leaf cert rotation for compromised nodes.

**3.5 Media relay (broker)**
- Provide a media relay mode for Opus/WebRTC signaling that can live alongside the HTTP relay.
- Support TURN-like relay fallback for NAT traversal (initially minimal, expand later).

#### 4) WebUI (primary UX)

**4.1 Capability-aware UI**
- The UI should read `/api/v1/caps` and hide/disable unsupported features.

**4.2 Trace-first UX**
- Treat traces as the primary timeline; sessions are a lens, not the source of truth.
- Provide deterministic “replay from trace” workflows for debugging.

**4.3 Offline + state continuity**
- Persist client settings and last-known caps, and survive daemon restarts cleanly.

**4.4 Media-first UX**
- Add a minimal voice panel with streaming mic capture + playback.
- Show media session state in the trace timeline for audit/replay.

---

### Cross-cutting infrastructure

1) **Schema registry + CI checks**
   - Maintain versioned JSON Schemas for all public API payloads.
   - Enforce in CI (as already done for existing specs).

2) **Evidence & attestation**
   - Extend evidence bundles with cryptographic signatures.
   - Support “prove this run” artifacts that can be verified offline.

3) **Formal limits**
   - Standardize a single “limits” document generated from defaults (agentd + broker + core).
   - Keep docs + runtime consistent via tests.

4) **PKI + attestation**
   - Define a root CA + device cert model (mTLS + signed manifests).
   - Include signed evidence bundles and public verification tooling.

5) **Consensus protocol**
   - Specify a minimal consensus/coordination protocol for node clusters (leader election + task locks).
   - Provide a deterministic simulation harness for testing conflict scenarios.

---

### Phased roadmap (high leverage)

**Phase A: Contract foundation (short-term)**
- Implement `/api/v1/caps` and a common caps schema (agentd + WebUI + broker proxy).
- Create a single event schema for run + workflow events; add schema tests.
- Add an idempotency key to broker proxy/orchestrate requests (implemented).

**Phase B: Deterministic replay (mid-term)**
- Store replay bundles for deterministic runs (inputs + hashes + tool outputs).
- Add replay validation tests for a minimal fixture set.

**Phase C: Multi-transport readiness (mid-term)**
- Define a transport interface for broker + connector.
- Build at least one alternative transport in a feature flag (research prototype).

**Phase D: Policy VM integration (long-term)**
- Document policy VM hook interfaces + execution limits.
- Provide a stub policy runner + deterministic test fixtures.

---

### Related docs

- `docs/WORKFLOWS.md` (durable workflow semantics)
- `docs/BROKER.md` (broker protocols and auth model)
- `docs/PROTOCOL.md` (agentd ↔ WebUI protocol)
- `docs/DIAGNOSTICS.md` (health + provider smoke tests)
