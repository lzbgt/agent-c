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
- Added a simple interactive REPL (`agent chat`) supporting the same session + tools flow:
  - `cli/src/main.cpp`
- Added unit tests (`tests/test_session.c`) and integration smoke tests (OpenRouter + DeepSeek) that can read test keys from `project.md`.
