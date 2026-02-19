# CLI Usage (`agent`)

Date: 2026-02-19

This guide covers the `agent` CLI and tool-loop behavior.

## One prompt (persisted session by default)

```bash
export OPENAI_API_KEY=...
./build/agent run "hello"
```

Notes:
- The CLI prints assistant text to **stdout**.
- With `--trace` (default), a compact transcript (tool calls + results + errors) is printed to **stderr** so stdout stays clean.
- Use `--transcript-jsonl <path>` to append raw tool-loop events as JSONL (for UI replay/debugging).

This uses session id `default` and stores it at `~/.agent/sessions/default.sess` (portable, JSON-free). If built with JSONCPP,
it also writes `~/.agent/sessions/default.json` for debugging/interoperability. If built without JSONCPP, `.sess` is the only
persisted session format.

## Proxy (when outbound networking requires it)

If your environment requires an HTTP proxy for outbound HTTPS, pass an explicit proxy URL (or set env `HTTPS_PROXY` / `http_proxy`):

```bash
./build/agent run "hello" --proxy http://localhost:8120
```

## Explicit session id

```bash
./build/agent run "continue" --session myproj
```

## Disable persistence (ephemeral run)

```bash
./build/agent run "one shot" --no-session
```

## OpenRouter (optional headers)

```bash
export OPENROUTER_API_KEY=...
export OPENROUTER_API_BASE=https://openrouter.ai/api/v1
export OPENROUTER_HTTP_REFERER=https://example.com
export OPENROUTER_X_TITLE="agent"
./build/agent run "hi" --base-url "$OPENROUTER_API_BASE" --model "google/gemini-2.0-flash-001"
```

## Pick an OpenRouter verification model (cheap + tools + multimodal)

This repo includes a utility that fetches OpenRouter's model catalog, filters to:
- multimodal-capable inputs (image/audio/video)
- OpenAI tools/tool_choice support
- total pricing within a range (default: $0.01–$0.50 per 1M tokens, prompt+completion)

It writes a report to `ref/openrouter/multimodal_latest.md` and prints a recommended model id:

```bash
python3 tools/openrouter_models.py --write
```

## DeepSeek tool use (calculator)

`deepseek-reasoner` can do tool calls but may reject forcing `tool_choice`. For deterministic tool verification use `deepseek-chat`:

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

## Multimodal attachments (images + files)

Attach local files via `--attach <path>` (repeatable).

Behavior:
- Images (`.png/.jpg/.webp/...`) are sent as base64 `data:` URLs (model must support image inputs).
- Non-image files are included as **best-effort text excerpts** (binary files get a short base64 preview).
- Providers/models vary on multimodal support; failures typically return HTTP `400` with “image input not supported”.

Example (OpenAI-compatible vision-capable model):

```bash
./build/agent run "Describe this image." \
  --no-session \
  --tools none \
  --base-url "https://api.openai.com/v1" \
  --model "<vision-capable-model-id>" \
  --attach ./image.png
```

Example (Moonshot / Kimi K2.5, OpenAI-compatible API; base64 images only):

```bash
./build/agent run "Describe this image." \
  --no-session \
  --tools none \
  --base-url "https://api.moonshot.cn/v1" \
  --model "kimi-k2.5" \
  --api-key "$KIMI_API_KEY_CN" \
  --attach ./image.png
```

## Extensible tools (embedded-friendly)

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

See `docs/TOOLS.md` for the built-in host tool list and token-safety guidance.

## Tool loop safety (repeat guard + caps)

Some failure modes look like “capture camera → register artifact → repeat forever”. To keep runs bounded, the tool loop
has a guard that stops if the model repeats the **exact same tool call** too many times:

- CLI: `--max-repeated-tool-calls <n>` (default: `12`; set `0` to disable)
- Daemon/UI: request field `max_repeated_tool_calls` (see `docs/PROTOCOL.md`)
- For daemon runs, `max_steps` can be left blank in the UI to use the daemon’s default step cap (`/api/v1/config: daemon.max_steps_default`).
- For worst-case safety, daemon also applies a default cap on total tool calls (`/api/v1/config: daemon.max_tool_calls_total_default`).
- To guard against oversized tool call arguments, daemon can apply a default cap on tool call JSON length
  (`/api/v1/config: daemon.max_tool_call_args_chars_default`, `0` disables).
- For targeted safety (without breaking benign high-frequency tools like `fs_read`), daemon can apply explicit per-tool limits:
  - daemon defaults are visible at `/api/v1/config: daemon.tool_call_limits_default`
  - configure via `agentd --tool-call-limit proc_exec=4 --tool-call-limit shell_exec=16 --tool-call-limit artifact_register=16 --tool-call-limit ui_action=16`
  - or via env: `AGENTD_TOOL_CALL_LIMITS_DEFAULT="proc_exec=4,shell_exec=16,artifact_register=16,ui_action=16"`
  - per-run overrides: request field `tool_call_limits` (see `docs/PROTOCOL.md`)
  - defaults can be persisted via `/api/v1/config/update`:
    `max_steps_default`, `max_tool_calls_total_default`, `max_tool_calls_per_tool_default`,
    `max_tool_call_args_chars_default`, `tool_call_limits_default`

Seamless compaction for tool loops:
- In `--tools basic` / `--tools host` mode, the tool loop applies the same char-budget policy as the core runner:
  keep pinned `system` prefix + keep last `K` messages.
- When messages are dropped, a short deterministic `system` summary is inserted into the request so the model
  has some continuity (no extra model call).
- If the provider rejects a request as too large (context length / too many tokens), the tool loop retries after
  compacting more aggressively. This is effectively “spawning a new session” for stateless backends.

## Assistant streaming (stdout)

For `agent run` / `agent chat`, pass `--stream-assistant` to stream assistant deltas to stdout (provider-dependent):
- `--tools none`: streams the assistant message for a single request (`stream: true`).
- `--tools basic|host`: streams assistant deltas during tool-loop steps (best-effort; depends on provider streaming and `delta.tool_calls`).

## Session vs audit

Session message history:
- `~/.agent/sessions/<id>.sess` (portable, JSON-free; primary)
- `~/.agent/sessions/<id>.json` (optional, written when built with JSONCPP)

These store **user/assistant conversation** only. Detailed tool timelines (tool calls/results + LLM request/response events)
are stored in the per-session audit log (`~/.agent/sessions/<id>.events.jsonl`) and surfaced via CLI `--trace` output.

Daemon (`agentd`) state is separate: sessions + audit live in SQLite (`db_path`, default: `<state_dir>/agentd.db`) and are
exposed via `GET /api/v1/session` and `GET /api/v1/session/audit`.

## UI actions (bidirectional UX)

Host tools include `ui_action` so the model can request UI-side actions in a typed and allowlisted way
(e.g. notifications, audio playback UI). See `docs/CLIENT.md`.

## Default host system hint (CLI/daemon)

When using `--tools host`, the CLI/daemon injects a one-time `system` message into an empty session to encourage
fast incremental inspection (use `rg/grep`, `head`, `tail`, `awk`, `sed -n`) instead of reading large files wholesale.

- CLI: disable with `--no-default-system` or override with `--system "<your prompt>"`.
- CLI: select a built-in prompt profile with `--system-profile default|jules_codex`.
- Daemon: disable with `./build/agentd --no-default-system` or per-request with `no_default_system: true`;
  override per-request with `system: "<your prompt>"`.
- Daemon: select a built-in prompt profile with `./build/agentd --system-profile default|jules_codex`,
  env `AGENTD_SYSTEM_PROFILE`, or per-request with `system_profile: "default" | "jules_codex"`.

## Optional LLM summaries for compaction (`--tools none`)

In `--tools none` mode, you can optionally provide `--summary-model <name>` to generate a short LLM summary of the
messages that are about to be dropped during compaction. The host inserts the summary as a `system` message starting with
`AGENT_SESSION_SUMMARY_PREFIX` so the core can treat it as **not pinned** (allowing it to be replaced/compacted later).

This is optional because it costs an extra model call; the default compaction behavior does not require it.

## Chat REPL

```bash
./build/agent chat --session default --tools host --tools-root .
```

Commands:
- `/exit` or `/quit` ends the REPL.
