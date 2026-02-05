# Handoff — Implement Platform/Broker Support for MCU Embedded Agent Interop (UM‑EAIS v0.1)

Source:
- Copied from `../urine_monitor/docs/spec/edge_agent_interop_handoff_to_agent_repo.md`
- Original reference revision: `urine_monitor` commit `278ad9e5` (see that repo for history)

Audience: the engineer/agent working in `../agent` repo (platform/broker framework).

Purpose:
- Archive the interop spec and provide a clear “what to build next” checklist so the platform can coordinate
  multiple heterogeneous MCU nodes running embedded `agent_core`.

This repo (`urine_monitor`) is the **node/firmware** side. The multi-node orchestration should live in `../agent`.

---

## 1) Canonical spec (source of truth)

Canonical interop spec (UM‑EAIS v0.1):
- `docs/spec/platform_edge_agent_interop_v0_1.md`
- Git commit in this repo: `278ad9e5` (pushed to `origin/master`)

This spec set includes:
- UM‑ACDS (capability manifest + tool schemas)
- UM‑BMP (broker messaging payloads)
- UM‑WF (workflow DAG format: parallel/sequential/conditional)
- UM‑EEM (execution model: idempotency, deadlines/timeouts, state machine)
- UM‑SAFE (safety metadata: hazards, side-effect level, rate limits)

Recommendation for `../agent` repo:
- Copy this file into `../agent/docs/spec/um-eais/um-eais-v0.1.md` (or similar) and keep a note that the canonical
  source is `urine_monitor` commit `278ad9e5`.

---

## 2) System architecture stance (decision)

Platform/broker is the **primary coordinator**:
- It is the system-of-record for multi-node workflows.
- It decomposes user requests into a workflow DAG.
- It schedules steps across nodes using capability routing.

Nodes still run embedded `agent_core` for:
- local UX (voice wake word + quick local actions)
- low-latency decisions that don’t require cross-node consensus

But for cross-node coordination (“turn on light + TV + AC”, “camera detects dirt → robot cleans”), planning should be
platform-owned, not distributed across nodes.

This avoids distributed consensus complexity and makes idempotency + retries enforceable.

---

## 3) Current status in `urine_monitor` (node-side already working)

Node/firmware has already been updated and verified to support:
- Embedded `agent_core` tool-loop on ESP32‑S3 N16R8
- A demonstrable actuator tool: WS2812 LED control
- Kimi/Moonshot tool-calling compatibility (reasoning replay requirements)
- Hardware presence detection for missing LoRa module (warn once + disable)
- Host-side verifiers for KWS + ASR mock + docker-compose smoke

Relevant commits in this repo:
- `05b40627`: “ESP32-S3 N16R8: embed agent_core + voice stack v0”
- `278ad9e5`: “docs: draft edge agent interop spec v0.1” (canonical spec document)

Verified behavior (real hardware):
- Board: ESP32‑S3 N16R8 with INMP441 + MAX98357A + WS2812
- Example interactive path: UART CLI → `AGENTCORE RUN make the led blink red every 1 second forever`
  results in on-device `agent_core` calling tool `led_control` successfully.

Important provider detail:
- Moonshot/Kimi returns HTTP 400 unless the request includes `reasoning_content` on assistant messages in tool-call flows,
  and the returned reasoning content must be replayed across the subsequent tool messages.
  (This is also implemented in `../agent`’s `openai_tool_provider.cpp`.)

---

## 4) What should be implemented in `../agent` (platform/broker responsibilities)

### 4.1 UM‑ACDS consumer + node registry
Implement a “capability registry” that:
- stores each node’s latest manifest (and `caps_sha256`)
- indexes tools and tags for routing
- stores hardware presence (so the platform doesn’t schedule impossible actions)

### 4.2 UM‑BMP message handling (minimal viable set)
Implement these message types and their persistence/observability:
- `NODE_HELLO`, `NODE_HEARTBEAT`
- `PLATFORM_CAPS_REQ`, `NODE_CAPS_RSP`
- `TASK_ASSIGN`, `TASK_ACK`
- `TASK_EVENT`, `TASK_DONE`, `TASK_FAILED`
- `SENSOR_EVENT`

Transport is flexible (MQTT/WebSocket/HTTP) — payload semantics must match the spec.

### 4.3 UM‑EEM execution semantics (reliability)
Platform must enforce:
- idempotency keys: dedupe repeated dispatch due to retries
- deadlines/timeouts: consistent failure modes
- state machine: `QUEUED → RUNNING → SUCCEEDED/FAILED/TIMED_OUT`

### 4.4 UM‑WF workflow engine (parallel/sequential/conditional)
Implement a minimal workflow runner that supports:
- `parallel` fan-out + join
- explicit `depends_on` sequencing
- `condition` on incoming events/state (e.g. camera dirt detection)

### 4.5 Capability-based node selection
Support selecting nodes by:
- required tools (`requires_tools`)
- tags (`tags_all`, `tags_any`, `tags_none`)
- optional “site/room” routing (e.g. `room:lobby`)

### 4.6 Safety gates (UM‑SAFE)
Start with metadata-driven policy:
- hazard tags + side-effect level
- per-tool rate limit
- allow/deny lists

Even a minimal “deny privacy_camera by default” gate is valuable early.

---

## 5) Suggested MVP milestones (fastest path)

### Milestone 0: Node registry + discovery
- Accept `NODE_HELLO`
- Request manifest if `caps_sha256` unknown
- Store manifest; expose `GET /nodes` and `GET /nodes/{id}/caps` for debug

### Milestone 1: Task dispatch (explicit tool invocation)
- `TASK_ASSIGN mode:"invoke"` that contains `{tool,args}`
- Node replies with `TASK_ACK`, `TASK_EVENT`, `TASK_DONE/FAILED`
- Platform dedupes via `idempotency_key`

### Milestone 2: Parallel workflow
- User goal → platform builds a `parallel` DAG
- Dispatch to multiple nodes concurrently
- Join behavior: `all` / `any`

### Milestone 3: Event-triggered conditional workflow
- Receive `SENSOR_EVENT` → trigger a DAG with `condition` → actuation

---

## 6) Acceptance tests (what “done” means)

### 6.1 Local integration (single node)
Given one node advertising `ui.led.ws2812.control` (or the existing LED tool name),
platform can:
1) discover node capabilities
2) dispatch a tool invocation
3) observe completion state

### 6.2 Parallel workflow
Given nodes A/B/C that each advertise one tool:
- `home.light.switch`
- `home.tv.power`
- `hvac.ac.power`

The platform accepts a user goal “enter room, turn on light, tv, ac” and executes:
- 3 tool calls in parallel, then joins on “all”

### 6.3 Conditional workflow
Given camera node emits `SENSOR_EVENT {event_type:"dirt_detected", confidence:0.9}`,
platform triggers robot node action `robot.clean.zone`.

### 6.4 Idempotency
If the same `TASK_ASSIGN` is retried (same `idempotency_key`), platform must not cause duplicated side-effects.

---

## 7) Notes / constraints

- `urine_monitor` should NOT embed the platform orchestrator; it should remain a node/firmware + node-side tools repo.
- `../agent` should avoid hardcoding node IDs; use capability routing from manifests.
- Tool schemas should be treated as the compatibility contract; do not silently “accept anything” for actuators.

---

## 8) Quick pointers for cross-repo alignment

The platform should assume OpenAI-compatible tool calling shape:
- `tools: [{type:"function", function:{name, description, parameters}}]`
- `tool_calls[]` with `id`, `function.name`, `function.arguments`
- tool result messages include `tool_call_id`

Moonshot/Kimi special constraint (must support):
- assistant tool-call messages require `reasoning_content` field
- reasoning must be replayed across subsequent tool-calling turns

This is already implemented in `../agent`’s provider code and was required on-device too.

