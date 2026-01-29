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

The CLI/daemon toolset is designed around **OS-native tooling**:
- Use `proc_exec` / `shell_exec` to run system-installed binaries for project inspection and file operations
  (`ls`, `find`, `cat`, `rg`, `rm`, `git`, language toolchains, etc.).
- Use a dedicated diff-based editing tool (`file_apply_patch`) so file edits are auditable in the transcript
  (the tool result includes the unified diff that was applied).

`--tools-root` (when set) is primarily used to control the working directory for `file_apply_patch`
(via `git -C <root> apply ...`). It is not intended as a strong security boundary (since `proc_exec`
can still invoke arbitrary commands in a YOLO host configuration).

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
