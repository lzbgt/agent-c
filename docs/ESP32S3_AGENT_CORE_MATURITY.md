# ESP32-S3 Readiness: `agent_core` vs `agentd` (Maturity + Design)

Date: 2026-02-01

This document assesses whether the project can run “agent + tool use” on an **ESP32-S3-class MCU**,
and what to build next so the device can control attached peripherals from user prompts.

## Executive summary (fact-based)

### What is realistic on ESP32-S3 today

**Use `agent_core` (pure C library), not `agentd`.**

Facts from the repo:
- `agent_core` is **C11**, portable, and is explicitly documented for ESP32-S3 in `docs/EMBEDDED_C_API.md`.
- `agentd` / `agentd_lib` is **C++17**, depends on **libcurl + jsoncpp**, and typically **sqlite3** + filesystem.
  - Those are desktop/server dependencies and are not a practical fit for ESP-IDF builds.

So the maturity assessment is split into:
- **On-device agent (ESP32-S3)**: `agent_core` + device tools + an embedded provider adapter.
- **Desktop/server daemon**: `agentd` (sidecar mode, broker mode) for richer environments.

### “Maturity” rating (what you can build confidently)

| Layer | Target | Current maturity | Why |
|---|---|---:|---|
| `agent_core` (tool loop + session) | ESP32-S3 | **Good** | Pure C, custom allocator, hard safety limits; no HTTP/JSON assumptions. Core-only build + tests pass. |
| Provider adapter (HTTP/TLS + JSON parsing) | ESP32-S3 | **Missing (host responsibility)** | Not shipped by design; you must implement with ESP-IDF networking + a JSON library. |
| Tool executor (peripherals) | ESP32-S3 | **Project-specific** | Framework exists (`agent_tool_executor_t`), but you must implement tools + validation. |
| Persistence (NVS/flash/FS) | ESP32-S3 | **Moderate** | Core provides session codec + persistor interface; host must implement NVS/FS. |
| `agentd` / `agentd_lib` | ESP32-S3 | **Not a target** | C++17 + curl/jsoncpp/sqlite/filesystem/threading; intended for desktop/server. |

## Constraints you must design for (ESP32-S3-class MCU)

These are not repo-specific; they are practical MCU constraints that shape the integration:

- **RAM is limited** (especially if you don’t have PSRAM). TLS + JSON parsing can be memory hungry.
- **Flash is limited**. Pulling in heavy formatting / JSON libs increases binary size.
- **Network is lossy**. Provider calls need retries + timeouts and should support cancellation.
- **Real-time peripherals** must not be blocked by the LLM loop. Run the agent in its own FreeRTOS task.

To make this concrete for your board, we should pin:
- whether your ESP32-S3 build has PSRAM enabled and how much
- how you plan to connect to the LLM (Wi‑Fi direct, gateway, cellular modem, etc.)
- whether the device must work offline (local model) or can be cloud-dependent

## Dependency mapping: `agentd` vs `agent_core`

### `agent_core` (ESP32-S3 friendly)

Observed dependencies (from `core/src` + headers):
- Language: C11
- libc headers: `string.h`, `ctype.h`, `stdint.h`, `stddef.h`
- heap: `agent_malloc/agent_free` (overridable via `agent_set_allocator`)
- no filesystem, no sockets, no threads required by the core

Notes:
- `agent_core` includes an embedded-friendly persistence codec: `core/include/agent/session_codec.h`
- multimodal “parts” exist, but persistence codec v1 stores **role + content only** (no parts yet).

### `agentd` / `agentd_lib` (desktop/server)

Observed characteristics:
- Language: C++17
- Depends on `agent_host`, which requires:
  - libcurl
  - jsoncpp
- Daemon layer adds:
  - sqlite3 (when enabled)
  - filesystem usage
  - optional HTTP server thread (when `AGENTD_ENABLE_HTTP=ON`)

This is appropriate for a workstation/edge gateway, not for an MCU build.

## Architectures for “prompt → tool use → peripherals”

There are two viable patterns. Pick based on your network/security requirements.

### Option A (recommended): Remote agent, MCU is a tool endpoint

Run `agentd` on a gateway (desktop/server/RPi). The ESP32-S3 exposes a **small device RPC** interface:
- gateway sends “execute tool” requests
- MCU executes peripheral operation
- MCU replies with result JSON

Pros:
- MCU does not need to implement tool-calling LLM protocol, TLS quirks, or big JSON parsing.
- You keep secrets (LLM API keys) off the device.
- You can ship updates faster on the gateway.

Cons:
- Requires gateway connectivity.
- Requires an additional “device RPC” protocol (MQTT/HTTP/serial/custom).

This repo already has a broker concept (`docs/BROKER.md`) for routing requests to agents behind NAT; a similar pattern
works for routing tool calls to devices.

### Option B: On-device agent (`agent_core` runs on ESP32-S3)

The ESP32-S3 runs:
- `agent_core` tool loop
- an embedded provider adapter (HTTP/TLS + JSON parsing) that returns assistant text + tool calls
- a tool executor that controls peripherals
- optional persistor (NVS) for sessions

Pros:
- Device can accept prompts directly and act autonomously.
- No gateway required (beyond internet access to the LLM).

Cons:
- Harder engineering on MCU: TLS + JSON parsing + retries + time sync.
- Higher risk surface: device holds more “decision power”.

## On-device design (Option B): exact API boundaries

### 1) Tool registry (compile-time)

You register tools using:
- `agent_tool_registry_add(name, description, parameters_json)`

On MCU, keep schemas **small** and **strict**:
- prefer enums, bounded ints, required fields
- avoid large free-form strings where possible

### 2) Tool executor (peripherals)

You implement:
- `agent_tool_executor_t.execute(ctx, tool_name, arguments_json, out_result)`

Hard requirements for safety:
- validate `tool_name` is in an allowlist
- validate arguments: type, range, units, and *side effects*
- make tool operations idempotent when possible
- keep tool results compact JSON (token + RAM friendly)

### 3) Tool-capable provider (LLM adapter)

You implement:
- `agent_tool_provider_t.generate(ctx, req, out_resp)`

Provider responsibilities:
- serialize `req->messages` + tool schemas into your LLM API request
- call LLM (with timeouts + retries)
- parse response into:
  - `out_resp->assistant_content`
  - `out_resp->tool_calls[]` (name + arguments_json [+ id])

Core requirement:
- any allocations stored into `out_resp` must be compatible with `agent_free` (use `agent_malloc` / `agent_string_set_copy`).

### 4) Persistence (optional but usually needed)

For persistence, implement `agent_persistor_t` against:
- NVS (key-value): store `agent_session_codec_encode_v1` output per session id
- or a filesystem (SPIFFS/LittleFS) storing the same codec text

## Safety limits you should enable on MCU

The core already provides hard stops; on-device you should turn them on by default:

- `max_steps` (small default like 4–8)
- `max_tool_calls_total` (caps “multi tool calls in one step” runaway)
- `max_repeated_tool_calls` (prevents “repeat the same call forever”)
- `max_tool_result_chars` (prevents huge tool outputs from exploding tokens)
- `disable_tool_records=1` (reduces allocations)

Additionally, implement **tool-level** safety:
- per-tool call limits (`tool_call_limits`) for dangerous/expensive tools (motors, relays, OTA, etc.)
- “arming” or confirmation flow for irreversible actions

## Work items (what to implement next)

### Phase 0: Packaging + build proof

- Create an ESP-IDF component wrapper for `agent_core` (no provider included).
- Build a “hello tool” example that runs without Wi‑Fi (fake provider that returns a fixed tool call).
- Add a **host-side simulator** that runs `agent_core` with the same tool registry + executor, but logs everything for rapid iteration.
  - In this repo: the `esp32sim` harness (`tools/esp32sim.cpp`) builds on desktop and writes a JSONL log for inspection.

Exit criteria:
- `agent_core` compiles under ESP-IDF toolchain
- tool executor controls at least one peripheral (GPIO toggle) from a tool call

### Phase 1: Provider adapter

- Implement an ESP-IDF provider adapter (HTTP client + TLS + JSON parsing).
- Support tool calling response parsing for your chosen backend.

Exit criteria:
- end-to-end: prompt → LLM → tool call → GPIO result → assistant conclusion
- robust timeouts + retry + cancellation

### Phase 2: Session + Scene strategy

Decide:
- whether the MCU needs a “Scene” concept locally (often unnecessary; Scene is mainly for rich clients)
- how sessions are persisted (NVS vs FS vs remote)

Exit criteria:
- device can resume last session after reboot

## Notes on “Scene” for MCUs

The durable Scene described in `docs/CLIENT_AGENTD_SPEC.md` is a **daemon-side** persistence feature (DB-backed).
On MCU, it is usually better to:
- represent UI state in the client (mobile/web)
- keep the device focused on **actuation + sensing tools**

If you do need a local scene, implement it as a thin mapping of entity ids to props in RAM, not as a DB feature.
