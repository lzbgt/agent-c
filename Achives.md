# Achives (Done)

Completed milestones and notable tasks.

## 2026-01-29

- Drafted `DESIGN.md` specifying env-free core, optional persistence, injected transport/provider approach, and char-budget compaction.
- Implemented `agent_core` (portable C) with session storage + char-budget compaction:
  - `core/include/agent/agent.h`
  - `core/src/agent_session.c`
- Added a core provider interface + runner (host-injected LLM backend):
  - `core/include/agent/provider.h`
  - `core/include/agent/runner.h`
  - `core/src/agent_runner.c`
- Implemented desktop CLI host adapter (`agent`) with:
  - env/flags parsing (`cli/src/main.cpp`)
  - libcurl OpenAI-compatible HTTP call (`cli/src/openai_client.cpp`)
  - file-backed session persistence (`cli/src/session_store.cpp`)
- Added multimodal-ready message parts API (text/image/audio/video parts) in the core (initial scaffold):
  - `core/include/agent/parts.h`
- Added a tool-call loop in the CLI (basic calculator tool) and a DeepSeek tools smoke test:
  - `cli/src/tool_loop.cpp`
  - `cli/src/toolset_basic.cpp`
  - `tests/deepseek_tools_smoke.sh`
- Added DeepSeek reasoner tool-call smoke test (no forced tool choice):
  - `tests/deepseek_reasoner_tools_smoke.sh`
- Added core tool registry + executor interface to support embedded hardware tools (GPIO, etc.):
  - `core/include/agent/tools.h`
  - `core/src/agent_tools.c`
- Implemented a host toolset for CLI/daemon (process exec + diff-based file edits) built on the same tool interfaces:
  - `cli/src/toolset_host.cpp`
  - `tests/test_host_toolset.cpp`
- Improved CLI error reporting for OpenAI-compatible backends by extracting provider error messages from non-2xx responses:
  - `cli/src/openai_client.cpp`
  - `cli/src/tool_loop.cpp`
- Added a daemon-first direction and initial implementation:
  - `agentd` executable with a minimal local HTTP+JSON API (`GET /api/v1/health`, `POST /api/v1/run`)
  - `ui/` Web UI scaffold (React + Vite + Tailwind) to drive the daemon day 1
- Improved Web UI observability and rendering:
  - daemon returns structured `events` for verbose inspection (LLM + tool timeline)
  - UI renders assistant markdown and tool outputs (including diff patches)
  - daemon can serve local artifacts for preview (`GET /api/v1/file`)
- Added session browsing + audit log persistence:
  - `GET /api/v1/sessions`, `GET /api/v1/session`, `GET /api/v1/session/audit`, `DELETE /api/v1/session`
  - per-run audit records stored under `~/.agent/sessions/<id>.events.jsonl`
- Added a simple interactive REPL (`agent chat`) supporting the same session + tools flow:
  - `cli/src/main.cpp`
- Added unit tests (`tests/test_session.c`) and integration smoke tests (OpenRouter + DeepSeek) that can read test keys from `project.md`.
- Refactored `agentd` `POST /api/v1/run` to share a single implementation path (`run_request_to_json`) and added `rpc_status` hints for request validation errors.
- Improved OpenRouter model catalog tool (`tools/openrouter_models.py`) to filter for models supporting OpenAI tool calls and to include this in the report table.
- Added a DeepSeek host-tool smoke test to verify non-calculator tool calling (`tests/deepseek_host_tools_smoke.sh`).
- Added an OpenRouter host-tool smoke test to verify tool calling via OpenRouter (`tests/openrouter_host_tools_smoke.sh`).
- Implemented seamless compaction for tool-call loops (CLI+daemon) with a deterministic “dropped messages” summary event (`cli/src/tool_loop.cpp`).
- Added automatic “session rotation” for tool loops: if a provider rejects a request as too large, retry after more aggressive compaction (`cli/src/tool_loop.cpp`).
- Downloaded DeepSeek API docs pages used for tool-calling, pricing, multi-round chat, and context caching reference into `ref/deepseek/` (HTML snapshots).
- Added daemon-level smoke verification (`agentd_smoke`) and fixed loopback binding in the embedded-friendly HTTP server (`daemon/src/http_server.cpp`).
