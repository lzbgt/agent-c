# Platform ↔ Edge “Embedded Agent” Interop Spec (Draft v0.1)

Status: **Draft** (for review)

Source note:
- This spec was originally drafted in the node/firmware workspace (`../urine_monitor`) and synced here for the platform/broker implementation.
- Canonical reference revision: `urine_monitor` commit `278ad9e59a39061f550fe8c30768db275e454379`.

Machine-readable contract (this repo):
- JSON Schema (best-effort, v0.1):
  - `docs/spec/um-eais/schema/umbmp-envelope-v0.1.schema.json`
  - `docs/spec/um-eais/schema/um-eais-core-v0.1.schema.json`
  - `docs/spec/um-eais/schema/um-eais-platform-extensions-v0.1.schema.json`
- Golden transcript fixtures (replay-ready JSONL):
  - `docs/spec/um-eais/fixtures/umbmp_workflow_submit_cancel_v0.1.jsonl`
  - `docs/spec/um-eais/fixtures/umbmp_task_loop_v0.1.jsonl`

Purpose:
- Standardize the interface between:
  - **MCU firmware** (sensors/actuators + RTOS tasks),
  - an embedded **agent runtime** (`agent_core` tool-loop),
  - and the **platform/broker** that coordinates many heterogeneous nodes.
- Enable reliable **parallel** and **sequential** multi-node workflows (e.g. “enter room → turn on light+TV+AC”).
- Make node capabilities **discoverable** so orchestration can be capability-driven (not hard-coded node IDs).

This spec is **transport-agnostic**:
- It defines payloads and semantics.
- It does **not** mandate MQTT vs WebSocket vs HTTP vs LoRa frames.

---

## 0) Scope and assumptions

### 0.1 In-scope
- Capability discovery: how a node advertises what it can do, in a machine-readable way.
- Tool-use standardization: how `agent_core` calls *device tools* and how tools behave.
- Broker/platform messaging contract: task assignment, progress events, dedupe, deadlines.
- Workflow representation: parallel/sequential/conditional task graphs across nodes.
- Safety metadata: hazards, permissions, rate limits.

### 0.2 Expanded goals (v0.1)
Previously out-of-scope; now explicit goals for this spec and implementation plan:
- **Full security design** (mTLS, signing, provisioning). We include hooks and fields, and now require a complete PKI story.
- **Audio streaming / codec protocols** (Opus/WebRTC). Voice should be first-class on top of these primitives.
- **Distributed consensus between nodes** (nodes negotiating among themselves without a platform coordinator).

### 0.3 Design stance (important)
This spec assumes the platform/broker is the **primary coordinator**:
- Platform decomposes a user goal into a workflow graph.
- Nodes execute assigned steps and report events.
- Nodes may still run local `agent_core` for low-latency “local UX” (voice, local autonomy), but platform remains the
  system-of-record for multi-node coordination.

Reason: it is the fastest path to correctness for parallel + conditional workflows and avoids “emergent” distributed behavior.

---

## 1) Terminology

- **Node**: a single physical device (ESP32/STM32/etc.) with sensors/actuators.
- **Hub/Gateway**: an optional device relaying node traffic to the platform (e.g. LoRa hub).
- **Platform**: the orchestration layer (server/broker) that:
  - discovers nodes,
  - maintains state,
  - schedules workflows,
  - enforces safety and policy.
- **Embedded agent**: `agent_core` running on the node, capable of calling tools and (optionally) calling an LLM provider.
- **Tool**: a function that can be invoked with JSON arguments and returns a JSON result.
- **Capability Manifest**: a JSON document advertising node identity, hardware presence, tools, and runtime limits.
- **Workflow**: a DAG (directed acyclic graph) of steps with dependencies, concurrency, and conditions.

---

## 2) Goals (what “works” means)

### 2.1 Heterogeneous fleet scaling
Given N nodes with different hardware, the platform can:
- discover each node’s capabilities,
- select a compatible node for each workflow step,
- and execute the workflow reliably.

### 2.2 Deterministic execution
Even with network retries, the system avoids duplicated side-effects:
- idempotency keys,
- step dedupe,
- explicit timeouts and deadlines.

### 2.3 Safety first
Tools carry metadata enabling policy enforcement:
- hazard tags (e.g. `motor`, `heat`, `privacy_camera`),
- side-effect level,
- rate limits,
- permissions.

---

## 3) Spec set (UM‑EAIS v0.1)

This document defines a coherent set of contracts:

1) **UM‑ACDS**: Capability Discovery & Tool Schema Manifest
2) **UM‑BMP**: Broker Messaging Protocol (register, discover, task, events)
3) **UM‑WF**: Workflow Graph Format (parallel/sequential/conditional)
4) **UM‑EEM**: Execution & Event Model (idempotency, retries, deadlines)
5) **UM‑SAFE**: Safety & Permissions Metadata (policy gates)

Each section below is intentionally small. If v0.1 is accepted, we can split into separate docs.

---

## 4) UM‑ACDS — Capability Discovery & Tool Schema Manifest

### 4.1 Manifest requirements

The node MUST be able to produce a single JSON object (UTF‑8) called the **Capability Manifest**.
The manifest MUST be stable for a given firmware build + configuration, except for:
- hardware presence probe results,
- resource telemetry (optional),
- and connectivity/health state (optional).

The manifest MUST include a hash (`caps_sha256`) so the platform can cache it.

### 4.2 Manifest (top-level fields)

Required:
- `spec_version`: `"um-acds/0.1"`
- `manifest_version`: semver string for the manifest schema used by this node build
- `caps_sha256`: sha256 digest as 64 hex chars (optionally prefixed `sha256:`) of canonical JSON encoding of the manifest with volatile fields excluded
- `node`: identity + firmware info
- `runtime`: agent runtime + resource limits
- `hardware`: presence + probe time
- `tools`: tool definitions (schemas + metadata)
- `safety`: policy hooks (hazards, allow/deny lists, rate limits)

Recommended:
- `tags`: free-form list for routing (e.g. `room:lobby`, `role:camera`, `site:lab`)
- `transport_hints`: human-friendly hints (baud, HTTP base URL, etc.)

### 4.3 Node identity

`node.node_id` MUST be stable across reboots.
Recommended sources (in order):
1) provisioned UUID
2) chip MAC-based UUID
3) serial number printed on device

### 4.4 Hardware presence

`hardware.presence` MUST be a map of:
- peripheral key → `"present" | "absent" | "unknown"`

Example keys:
- `radio.lora.sx1262`
- `ui.led.ws2812`
- `sensor.camera`
- `sensor.weight.nau7802`

Rules:
- If hardware is absent, the node SHOULD omit dependent tools from `tools[]` (preferred).
- If tools remain, they MUST fail with `{ok:false,error:"hardware_absent"}` (see tool result envelope).

### 4.5 Tools: naming and schema strategy (chosen defaults)

#### 4.5.1 Tool naming
Tool names MUST be hierarchical dot-separated namespaces:
- `system.device_status`
- `ui.led.ws2812.control`
- `home.light.switch`
- `camera.detect.dirt`
- `robot.clean.zone`

Rationale: scales to large fleets; avoids collisions; improves LLM tool selection.

#### 4.5.2 Tool schemas in-manifest
The manifest MUST include full JSON Schema for tool parameters (OpenAI function schema-compatible).
It MAY additionally include:
- `schema_sha256` for caching/dedup
- `schema_ref` for registry-based reuse

Rationale: avoids runtime coupling on an external schema registry.

#### 4.5.3 Strictness by tool class
- **Actuator tools** MUST set `additionalProperties: false` and validate inputs strictly.
- **Sensor tools** SHOULD set `additionalProperties: true` and ignore unknown fields.

Rationale: actuator safety > backward compatibility; sensor evolution > strictness.

### 4.6 Tool Definition object (per tool)

Each tool entry MUST include:
- `name` (string)
- `description` (string, short)
- `kind` (enum): `actuator | sensor | system | ui | radio | debug`
- `parameters_schema` (JSON Schema object)
- `timeout_ms` (int)
- `idempotent` (bool)
- `side_effect_level` (enum): `none | low | high`
- `hazards` (array of strings, may be empty)

Optional:
- `rate_limit`: `{ max_per_minute, cooldown_ms }`
- `requires_hardware`: array of hardware presence keys
- `result_schema`: JSON Schema of returned `data` field (useful for platform validation)

---

## 5) UM‑BMP — Broker Messaging Protocol (payload-level)

### 5.1 Message envelope
All broker messages MUST be JSON objects with:
- `msg_id` (uuid)
- `ts_utc_ms` (int)
- `type` (string)
- `from` (string)
- `to` (string or null)
- `body` (object)

Optional:
- `auth` (object): token/signature placeholder
- `trace` (object): correlation ids

### 5.2 Core message types

#### Registration + discovery
- `NODE_HELLO`
  - body: `{ node_id, model, fw_git_sha, caps_sha256 }`
- `NODE_HEARTBEAT`
  - body: `{ node_id, health, caps_sha256, battery_pct?, rssi? }`
- `PLATFORM_CAPS_REQ`
  - body: `{ node_id, want: "full" | "hash" }`
- `NODE_CAPS_RSP`
  - body: `{ node_id, manifest }` (UM‑ACDS manifest)

#### Tasking
- `TASK_ASSIGN`
  - body: `{ task_id, step_id, idempotency_key, mode, deadline_utc_ms, payload }`
  - `mode`:
    - `"invoke"`: platform provides an explicit tool call
    - `"agent"`: platform provides a goal prompt for on-device `agent_core`
- `TASK_ACK`
  - body: `{ task_id, step_id, accepted: bool, reason? }`
- `TASK_EVENT`
  - body: `{ task_id, step_id, state, progress?, result?, error? }`
- `TASK_DONE`
  - body: `{ task_id, step_id, result }`
- `TASK_FAILED`
  - body: `{ task_id, step_id, error }`

#### Sensor/events
- `SENSOR_EVENT`
  - body: `{ node_id, event_type, ts_utc_ms, data, confidence? }`

### 5.3 Reliability + idempotency (normative, v0.1)

**ID token rule (recommended default):**
- All identifiers that participate in dedupe/correlation MUST be an `id_token`:
  - `node_id`, `task_id`, `step_id`, `workflow_id`, `idempotency_key`, `trace_id`
- `id_token` charset: `[A-Za-z0-9_.:-]`, length `1..128`.

**Transport retry rule (required):**
- Nodes and gateways MUST assume **at-least-once delivery**.
- Platforms MUST assume duplicates can arrive out-of-order.

**`msg_id` rule (required):**
- `msg_id` is the transport-level dedupe key for UM‑BMP ingress.
- Gateways SHOULD keep `msg_id` stable when retrying the *same* payload (MQTT/LoRa retransmit).

**Crash window rule (platform):**
- The platform persists inbound envelopes before applying side effects.
- A crash can occur after persistence but before side-effects; therefore the same `msg_id` may be reprocessed after restart.

**Tasking idempotency rule (required for `TASK_*`):**
- The platform emits `TASK_ASSIGN.body.idempotency_key` and MUST store it as the task’s correlation key.
- Nodes MUST echo the same `idempotency_key` on all subsequent task lifecycle messages:
  - `TASK_ACK`, `TASK_EVENT`, `TASK_DONE`, `TASK_FAILED`.

Rationale:
- Prevents applying a late event from an older dispatch attempt to a newer attempt.
- Allows the platform to ignore mismatched messages without breaking forward-compat.

**Correlation rule (recommended):**
- When a higher-level orchestration exists:
  - edge workflows: `workflow_id` SHOULD be reused as `task_id` for step messages (`task_id == workflow_id`, `step_id == step_id`)
  - durable workflows: `trace_id` SHOULD remain stable across the workflow run and be copied into envelopes as `trace.trace_id`.

---

## 6) UM‑WF — Workflow Graph Format

### 6.1 Workflow requirements
A workflow MUST be representable as a DAG of steps with explicit dependencies.
Steps can execute:
- sequentially (dependencies),
- in parallel (fan-out),
- conditionally (branching on event/state).

### 6.2 Workflow object (suggested minimal)
- `workflow_id` (uuid)
- `submitted_by` (user id / platform id)
- `goal` (string)
- `constraints` (object)
- `steps` (array)

Each step:
- `step_id` (string)
- `kind` (enum): `invoke_tool | run_agent | parallel | condition`
- `depends_on` (array of step_id)
- `target` (node selector)
- `payload` (object)

### 6.3 Node selection (capability routing)
`target` supports:
- direct: `{ node_id: "..." }`
- capability match (preferred for fleets):
  - `{ match_any: { requires_tools: [...], tags_all?: [...], tags_any?: [...], tags_none?: [...] } }`

Rationale: avoids hardcoding node IDs; enables replacing devices without reprogramming workflows.

### 6.4 Example: “enter room → turn on light + TV + AC”
High-level intent: parallel fan-out.

- Step A: `parallel`
  - Branch 1: invoke `home.light.switch {state:"on"}`
  - Branch 2: invoke `home.tv.power {state:"on"}`
  - Branch 3: invoke `hvac.ac.power {state:"on", setpoint_c: 24}`

---

## 7) UM‑EEM — Execution & Event Model (reliability)

### 7.1 Idempotency (mandatory)
Every `TASK_ASSIGN` MUST include `idempotency_key`.
Nodes MUST dedupe:
- If the same `idempotency_key` is seen again, the node MUST NOT re-execute side effects.
- It SHOULD return the cached prior result (or a deterministic “already_done” result).

### 7.2 Timeouts and deadlines
Each step has:
- `timeout_ms`: maximum execution time per attempt
- `deadline_utc_ms`: latest acceptable completion time (platform-level)

Node MUST:
- stop execution if `timeout_ms` exceeded and report `TIMED_OUT`.
- reject new work if it cannot meet deadline (optional in v0.1, recommended in v0.2).

### 7.3 Standard state machine
Nodes MUST report state transitions via `TASK_EVENT.state`:
- `QUEUED`
- `RUNNING`
- `SUCCEEDED`
- `FAILED`
- `CANCELED`
- `TIMED_OUT`

Optional:
- `WAITING_RESOURCE` (e.g. actuator locked)
- `WAITING_DEPENDENCY` (rare for nodes; mostly platform-side)

---

## 8) UM‑SAFE — Safety & permissions metadata

### 8.1 Hazard tags
Tools SHOULD declare hazards from a shared vocabulary:
- `motor`, `heat`, `sharp`, `liquid`, `electric_high`, `privacy_camera`, `privacy_mic`, `door_lock`

### 8.2 Side-effect level
Tools MUST declare:
- `side_effect_level`: `none | low | high`

Platform SHOULD use this to decide:
- whether user confirmation is required,
- whether automation rules can run unattended,
- whether additional constraints apply (time-of-day, occupancy, etc.).

### 8.3 Rate limits
Tools MAY declare a `rate_limit`:
- `max_per_minute`
- `cooldown_ms`

Node SHOULD enforce local rate limits even if the platform misbehaves.

---

## 9) Tool execution contract (MCU firmware ↔ embedded agent runtime)

This section standardizes the interface between the device application (peripherals) and `agent_core`.

### 9.1 Tool result envelope (mandatory)
All tool executions MUST return JSON:
- `ok` (bool)
- `error` (string, optional)
- `data` (object, optional)

Recommended standard `error` codes:
- `invalid_args`
- `hardware_absent`
- `busy`
- `timeout`
- `not_supported`

### 9.2 Actuator tool requirements
Actuator tools MUST:
- validate arguments strictly (reject unknown fields),
- clamp numeric ranges,
- have bounded behavior for “forever” actions (must be stoppable by another tool or duration).

### 9.3 Sensor tool requirements
Sensor tools SHOULD:
- return a timestamped measurement
- include units in the data payload (explicit)

### 9.4 Resource locking (recommended)
To avoid conflicts in parallel workflows, tools MAY declare a `resource_lock`:
- e.g. `resource_lock: "ui.led"`, `resource_lock: "hvac"`, `resource_lock: "robot.motion"`

Node MUST serialize operations that require the same lock.

---

## 10) Example tool: `ui.led.ws2812.control` (single LED)

Suggested schema (actuator; strict):
- `action` (required): `off | solid | blink | rolling`
- `color` (optional):
  - `hex` string `#RRGGBB`, or
  - `{ r,g,b }` integers 0..255
- `period_ms` (optional): integer 10..10000
- `duration_s` (optional): number >= 0 (0 means “forever”)

Result:
- `{ ok:true, data:{ applied:{...} } }`

---

## 11) Open questions (v0.2+)

1) Security hardening:
   - attestation, signing, capability spoof prevention.
2) Cross-node “atomicity”:
   - e.g. turn on light+TV+AC must be “all or rollback”.
3) Streaming + low-latency voice:
   - Opus/WebSocket gateway as a separate spec layer.

---

## 12) Appendix: Minimal JSON examples (copy/paste)

### 12.1 NODE_HELLO
```json
{
  "msg_id": "2ef0c6a2-6f6a-4b3e-8d89-3cbb7f4cbf1d",
  "ts_utc_ms": 1760000000000,
  "type": "NODE_HELLO",
  "from": "node:50787d15a1d4",
  "to": "platform",
  "body": {
    "node_id": "50787d15a1d4",
    "model": "esp32s3n16r8_audio_proto",
    "fw_git_sha": "05b40627",
    "caps_sha256": "sha256:..."
  }
}
```

### 12.2 Tool definition (manifest snippet)
```json
{
  "name": "ui.led.ws2812.control",
  "kind": "actuator",
  "description": "Control the onboard WS2812 RGB LED",
  "timeout_ms": 500,
  "idempotent": false,
  "side_effect_level": "low",
  "hazards": ["visual_only"],
  "parameters_schema": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "action": { "type": "string", "enum": ["off", "solid", "blink", "rolling"] },
      "hex": { "type": "string" },
      "r": { "type": "integer", "minimum": 0, "maximum": 255 },
      "g": { "type": "integer", "minimum": 0, "maximum": 255 },
      "b": { "type": "integer", "minimum": 0, "maximum": 255 },
      "period_ms": { "type": "integer", "minimum": 10, "maximum": 10000 },
      "duration_s": { "type": "number", "minimum": 0 }
    },
    "required": ["action"]
  }
}
```

### 12.3 TASK_ASSIGN (invoke)
```json
{
  "msg_id": "8d4f4c5b-2f7b-44b5-9f2c-9f6b7dcb2f0f",
  "ts_utc_ms": 1760000001234,
  "type": "TASK_ASSIGN",
  "from": "platform",
  "to": "node:50787d15a1d4",
  "body": {
    "task_id": "task_enter_room_001",
    "step_id": "step_light_on",
    "idempotency_key": "sha256:...",
    "mode": "invoke",
    "deadline_utc_ms": 1760000008000,
    "payload": {
      "tool": "ui.led.ws2812.control",
      "args": { "action": "solid", "hex": "#00FF00", "duration_s": 2 }
    }
  }
}
```

---

## Appendix A: Task result attestation (policy-enforced platform extension)

This appendix documents an **enforceable extension** implemented by the platform (`agentd`) on top of UM‑EAIS v0.1.
It is intended as an incremental step toward UM‑EAIS v0.2 “attestation + correlation”.

Policy toggles (agentd):
- `edge_attest_required`: when true, invoke-mode `TASK_DONE` must include `result.attest` with a valid `result_sha256`
  matching the platform-computed hash.
- `edge_attest_require_sig`: when true, `result.attest` must include a signature that verifies using configured keys
  (same key registry as UM‑BMP envelope auth).

### A.1 Node → platform: `result.attest` blob

On `TASK_DONE`, nodes SHOULD include an additional object under `body.result.attest` (required when policy demands it):

```json
{
  "type": "TASK_DONE",
  "body": {
    "task_id": "t1",
    "step_id": "s1",
    "idempotency_key": "k1",
    "result": {
      "ok": true,
      "data": { "x": 1 },
      "attest": {
        "result_sha256": "sha256:0123...cdef",
        "kid": "node-key-id",
        "alg": "ed25519",
        "sig": "base64(signature)",
        "ts_utc_ms": 1760000008000,
        "trace_sha256": "sha256:0123...cdef",
        "state_sha256": "sha256:0123...cdef",
        "note": "optional free-form metadata"
      }
    }
  }
}
```

Conventions:
- Keys are intentionally **not standardized yet** (UM‑EAIS v0.2 will define the normative surface).
- The platform treats `attest` as an **opaque JSON object** and persists it as `edge_tasks.attest_json` (bounded).
- When enforcement is enabled, the platform requires a matching `result_sha256` and (optionally) a valid signature.

### A.2 Platform-side deterministic hash surface: `edge_tasks.result_sha256`

When the platform receives a terminal `TASK_DONE` (and persists `result_json`), it also computes:
- `edge_tasks.result_sha256 = sha256(result_json_bytes)`

Notes:
- This is a platform-side replay/correlation surface. It does not prove remote computation integrity by itself.
- If a node provides `attest.result_sha256` and it differs from the platform’s `result_sha256`, the platform records a
  mismatch marker on the persisted task event (`_attest_result_sha256_mismatch`) and (when enforcement is enabled) treats
  the task as `FAILED`.
