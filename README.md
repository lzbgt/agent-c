# agent (prototype)

This repo is an early scaffold for a **portable agent core** (env-free, persistence-agnostic) plus a **desktop CLI host adapter** (env/config + persistence + HTTP).

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`ctest` includes two network smoke tests (OpenRouter + DeepSeek). They will run if keys are present
either via environment variables or `project.md`. Disable them with:

```bash
export AGENT_DISABLE_NETWORK_TESTS=1
```

Network tests also assume an HTTP proxy may be required; the scripts default to `http://localhost:8120`
via `https_proxy` / `http_proxy`.

Note: there are additional host-tool network smokes (DeepSeek/OpenRouter) that validate bounded tools like
`fs_read` and `fs_find` end-to-end (model → tool call → tool output).

## CLI usage

### One prompt (persisted session by default)

```bash
export OPENAI_API_KEY=...
./build/agent run "hello"
```

This uses session id `default` and stores it at `~/.agent/sessions/default.json`.

### Proxy (when outbound networking requires it)

If your environment requires an HTTP proxy for outbound HTTPS, pass an explicit proxy URL (or set env `HTTPS_PROXY` / `http_proxy`):

```bash
./build/agent run "hello" --proxy http://localhost:8120
```

### Explicit session id

```bash
./build/agent run "continue" --session myproj
```

### Disable persistence (ephemeral run)

```bash
./build/agent run "one shot" --no-session
```

### OpenRouter (optional headers)

```bash
export OPENROUTER_API_KEY=...
export OPENROUTER_API_BASE=https://openrouter.ai/api/v1
export OPENROUTER_HTTP_REFERER=https://example.com
export OPENROUTER_X_TITLE="agent"
./build/agent run "hi" --base-url "$OPENROUTER_API_BASE" --model "google/gemini-2.0-flash-001"
```

### Pick an OpenRouter verification model (cheap + tools + multimodal)

This repo includes a small host/dev utility that fetches OpenRouter's model catalog, filters to:
- multimodal-capable inputs (image/audio/video)
- OpenAI tools/tool_choice support
- total pricing within a range (default: $0.01–$0.50 per 1M tokens, prompt+completion)

It writes a report to `ref/openrouter/multimodal_latest.md` and prints a recommended model id:

```bash
python3 tools/openrouter_models.py --write
```

### DeepSeek tool use (calculator)

`deepseek-reasoner` can do tool calls but may reject forcing `tool_choice`, so for deterministic tool verification use `deepseek-chat`:

```bash
./build/agent run "Use calculator to compute (2+2)*10. Return exactly: 40" \
  --no-session \
  --base-url "https://api.deepseek.com" \
  --api-key "$DEEPSEEK_API_KEY" \
  --model "deepseek-chat" \
  --tools basic \
  --force-tool calculator \
  --require-tool-call
```

### Extensible tools (embedded-friendly)

Tool definitions and execution are host-pluggable:
- Core stores tool schemas in a portable registry: `core/include/agent/tools.h`
- Hosts implement the executor callback (GPIO, sensors, local RPC, etc.)
- CLI wires:
  - `basic` toolset (calculator) via `cli/src/toolset_basic.cpp`
  - `host` toolset (subprocess exec + diff-based file edits) via `cli/src/toolset_host.cpp`

Host toolset usage:

```bash
./build/agent run "List files, then show README.md" --tools host --tools-root .
```

Seamless compaction for tool loops:
- In `--tools basic` / `--tools host` mode, the tool loop applies the same char-budget policy as the core runner:
  keep pinned `system` prefix + keep last `K` messages.
- When messages are dropped, a short deterministic `system` summary is inserted into the request so the model
  has some continuity (no extra model call).
- If the provider rejects a request as too large (context length / too many tokens), the tool loop retries after
  compacting more aggressively. This is effectively “spawning a new session” for stateless backends.

Session vs audit:
- Session message history (`~/.agent/sessions/<id>.json`) stores **user/assistant conversation** only.
- Detailed tool timelines (tool calls/results + LLM request/response events) are stored in the per-session audit log
  (`~/.agent/sessions/<id>.events.jsonl`) and surfaced via daemon/UI (and CLI `--trace` output).

Host tool names:
- `shell_exec` (runs `/bin/sh -lc <cmd>`, returns JSON envelope with `exit_code`, `timed_out`, `truncated`, `output`)
- `proc_exec` (runs an argv array via `posix_spawnp`, no shell; returns JSON envelope with `argv`, `exit_code`, `timed_out`, `truncated`, `output`)
- `file_apply_patch` (applies a unified diff via `git apply`; returns the patch as a diff-style audit trail)
- `fs_stat` (file/dir metadata; returns structured fields + a human-readable `output`)
- `fs_list` (bounded directory listing; returns structured `entries` + `output`)
- `fs_find` (bounded file discovery; returns structured `entries` + `output`)
- `fs_read` (bounded file read with pagination by line; returns `content`/`output` + `has_more` + `next_start_line`)
- `text_search` (token-safe substring search; returns structured `matches` + `output`)

Notes:
- For **inspection** (read/list/stat), prefer `fs_list` / `fs_read` / `fs_stat` because they provide bounded output and pagination
  (helps prevent token/context blow-ups). Use `rg/grep` first, then `fs_read` for narrow line ranges.
- For **file discovery**, prefer `fs_find` over `find`/`tree` when you need predictable output size and default excludes.
- For **search**, prefer `text_search` over `grep -R` when you need predictable output size (bounded matches + per-file size limits).
  - Optional: use `extensions` (e.g. `[".cpp",".h"]`) to restrict scanning.
- For host-side **mutating** file operations (remove/move/rename), prefer OS-native commands via `proc_exec` / `shell_exec` (e.g. `rm`, `mv`, `git`).
- For file edits, prefer `file_apply_patch` so the tool output includes a diff-style record of the change.
- Tool outputs are capped before being inserted back into the next LLM request context, to avoid overflowing the context window.

Default host system hint (CLI/daemon):
- When using `--tools host`, the CLI/daemon injects a one-time `system` message into an empty session to encourage
  **fast incremental inspection** (use `rg/grep`, `head`, `tail`, `awk`, `sed -n`) instead of reading large files wholesale.
- CLI: disable with `--no-default-system` or override with `--system "<your prompt>"`.
- Daemon: disable with `./build/agentd --no-default-system` or per-request with `no_default_system: true`;
  override per-request with `system: "<your prompt>"`.

Optional LLM summaries for compaction (`--tools none`):
- In `--tools none` mode, you can optionally provide `--summary-model <name>` to generate a short LLM summary of the
  messages that are about to be dropped during compaction.
- The host inserts the summary as a `system` message starting with `AGENT_SESSION_SUMMARY_PREFIX` so the core can treat it
  as **not pinned** (allowing it to be replaced/compacted later instead of growing the pinned prefix forever).
- This is optional because it costs an extra model call; the default compaction behavior does not require it.

### Chat REPL

```bash
./build/agent chat --session default --tools host --tools-root .
```

Commands:
- `/exit` or `/quit` ends the REPL.

## Core library

- Header: `core/include/agent/agent.h`
- Scope: session model + char-budget compaction + role helpers
- No environment variable access in the core (host-only concern).

## Daemon + Web UI (day-1)

The recommended UX direction is **daemon-first**: run `agentd` locally and use the Web UI (or CLI) as a client.

### Run the daemon

```bash
./build/agentd --host 127.0.0.1 --port 8123
```

Proxy override:

```bash
./build/agentd --host 127.0.0.1 --port 8123 --proxy http://localhost:8120
```

Health check:

```bash
curl http://127.0.0.1:8123/api/v1/health
```

YOLO vs host-scoped tools:
- Default daemon mode is YOLO (unrestricted) to match local development needs.
- To scope file edits to a workspace root, set `tools_root` to `@host` (host scope configured by daemon).
- Clients can also pass `yolo: false` + `tools_root: "@host"` in `POST /api/v1/run`.

Verbose inspection:
- Pass `verbose: true` to `POST /api/v1/run` to return a structured `events` log suitable for UIs
  (LLM request/response, tool calls, tool results).
- Pass `trace: true` to also return a plain-text transcript (`trace_text`).
- Optional: pass `max_capture_bytes` to cap large verbose event payloads for UI stability (default: 256KB; daemon clamps for tool loops).
- `agentd` ignores `SIGPIPE` so client disconnects (UI refresh, SSE close) do not terminate the daemon.
- The daemon also serves files for previews at `GET /api/v1/file?path=<...>&yolo=0|1` (up to 10MB).

Assistant streaming (provider-dependent):
- For `tools: "none"` runs, clients can set `stream_assistant: true` to request OpenAI-compatible SSE streaming (`stream: true`).
  The daemon will emit `assistant_delta` events during the run so the UI doesn’t look stuck.

Tool schema introspection (extensible tools):
- `GET /api/v1/tools?tools=host|basic|none&tools_root=@host|@cwd|...&yolo=0|1` returns the tool registry the daemon will expose:
  `name`, `description`, `parameters_json` (OpenAI-compatible JSON Schema).
- This is intended for “day-1 rich UI” features (rendering tool info, validating tool-call args) and for future clients.

OpenRouter model discovery (for verification + multimodal/tools filtering):
- `GET /api/v1/openrouter/models?min_total=0.01&max_total=0.50&require_multimodal_input=1&require_tools=1&limit=50`
  fetches OpenRouter’s `/models` catalog (using `OPENROUTER_API_KEY` or an `Authorization: Bearer ...` header),
  filters it, sorts by total price ($/1M prompt+completion), and returns a recommended cheapest model id.

Session browsing:
- `GET /api/v1/sessions` lists known sessions.
- `GET /api/v1/session?session_id=<id>` returns the message history.
- `GET /api/v1/session/audit?session_id=<id>` returns recent per-run audit entries (prompt + assistant + events).

Async runs (UI-friendly):
- `POST /api/v1/run_async` starts a background run and returns `{ ok, job_id }`.
- `GET /api/v1/job?job_id=<id>` returns `{ ok, status, result? }` (result shape matches `/api/v1/run`).
  - For live UI progress without SSE/WebSocket:
    - Add `include_events=1` to include the tool/LLM `events` captured so far (best-effort).
    - Use `cursor=<n>&max_events=<m>` to tail new events incrementally while the job is running.
- `GET /api/v1/job/stream?job_id=<id>&cursor=<n>` streams job progress via SSE:
  - emits `agent_event` (same objects as the `events` array)
  - ends with `job_done` containing the final `result`
- While jobs are running, the daemon may emit `heartbeat` events when no other events have been produced for a short period.
  This makes UIs resilient to slow providers and long-running tools (e.g. `sleep`, builds) without requiring token streaming.

Job cancellation (best-effort):
- `POST /api/v1/job/cancel?job_id=<id>` requests cancellation.
- Cancellation is cooperative:
  - the tool loop will stop at safe boundaries (between tool calls / LLM requests)
  - long-running host tools (`shell_exec` / `proc_exec`) will terminate their subprocess when cancellation is requested

### Run the Web UI

```bash
cd ui
NPM_CONFIG_CACHE=../build/npm-cache npm ci
npm run dev
```

Then open the dev server URL (defaults to `http://localhost:5173`) and point it at the daemon base URL.
Using a repo-local `NPM_CONFIG_CACHE` avoids permission issues with the global npm cache in some environments.

UI rendering notes:
- The UI renders a **Conversation** (message cards) derived from the daemon `events` stream:
  user prompt → assistant messages → tool calls/results.
- Markdown is rendered with GFM + syntax highlighting for code blocks.
- The settings panel is collapsible and all settings persist in the browser via `localStorage`.
- If outbound networking requires a proxy, set **HTTPS proxy** in the UI settings (it is sent as `proxy` in `POST /api/v1/run`).

Host filesystem tools (token safety):
- `fs_list` is designed for bounded output and now excludes common huge directories by default (e.g. `node_modules`, `build`, `dist`).
  - To include them, pass `use_default_excludes: false` (and/or `exclude_names` to fine-tune).
- `fs_list`, `fs_find`, and `text_search` support `exclude_globs` (fnmatch) to filter out noisy paths (generated files, vendored code, etc.).
- `fs_stat` supports an optional bounded line count for small text files (`count_lines: true`) so the model can decide whether a file is huge without dumping it.
- `fs_read` supports paging (`start_line`, `max_lines`, `end_line`) and a character cap (`max_chars`).
