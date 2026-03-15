# Edge Agent Interop (UM‑EAIS v0.1) — Platform/Broker Support in `agentd`

Date: 2026-02-04

This repo implements the **platform/broker** side of the UM‑EAIS v0.1 draft spec (transport-agnostic payload semantics).

Canonical spec (copied from `../urine_monitor`):
- `docs/spec/um-eais/um-eais-v0.1.md` (copied from `../urine_monitor` commit `278ad9e5`)
- `docs/spec/um-eais/edge_agent_interop_handoff_to_agent_repo.md` (node-side handoff checklist, copied from the same source)

This document describes the **HTTP transport mapping** implemented by `agentd` for that payload-level spec.

Current status:
- Shipped: authenticated JSON/CBOR envelope ingress (HMAC + Ed25519), replay-window and
  node-binding policy knobs, best-effort task attestation verification, enforceable
  `edge_attest_required` / `edge_attest_require_sig` policy, and platform-signed
  node manifest bundle export via `GET /api/v1/edge/node/manifest_bundle`, plus durable
  trust-root rotation via `GET /api/v1/edge/auth/trust_roots` and
  `POST /api/v1/edge/auth/trust_roots/rotate`, per-node provisioning via
  `POST /api/v1/edge/auth/provision_node`, and revocation bundle/control via
  `GET /api/v1/edge/auth/revocations` plus `POST /api/v1/edge/auth/revocations/update`,
  plus node-pollable signed bundle distribution via:
  - `POST /api/v1/edge/node/manifest_bundle/send`
  - `POST /api/v1/edge/auth/trust_roots/send`
  - `POST /api/v1/edge/auth/revocations/send`
  - `POST /api/v1/edge/auth/cert_roots/send`
  - `POST /api/v1/edge/auth/cert_roots/verify_chain`
- Shipped: operator-side certificate-root bundle inspection and `openssl verify` chain checks via
  `tools/edge_cert_roots_tool.py`.
- Shipped: optional inline manifest identity certificate-chain enforcement on `NODE_CAPS_RSP`
  via `edge_auth_require_manifest_cert_chain`, backed by the same durable PEM root bundle and
  surfaced through node/manifest reads as `identity_cert_verify`.
- Shipped: AES-GCM encrypted UM-BMP body support (`body_enc`) with fail-closed ingress enforcement
  via `edge_confidentiality_required`, configured key slots via `edge_confidentiality_keys`, and
  encrypted outbox delivery on the manifest/trust-root/revocation/cert-root send helpers via
  `confidential_kid`.
- Still open: certificate-bound transport / envelope identity beyond manifest-ingest verification.

Executable contract artifacts (this repo):
- Schemas: `docs/spec/um-eais/schema/` (envelope + core + platform extensions)
- Golden transcript fixtures: `docs/spec/um-eais/fixtures/` (JSONL)
- Proof: `ctest` includes `um_eais_spec_sanity_tests`, `agentd_edge_interop_transcript_replay_smoke`, and
  `agentd_edge_interop_task_loop_replay_smoke`.

## Design

UM‑BMP defines:
- the **message envelope** (`msg_id`, `ts_utc_ms`, `type`, `from`, `to`, `body`)
- the **semantics** for discovery + tasking + events

`agentd` also ships an encrypted-body profile on top of that envelope:
- callers may send `body_enc` (`umbmp_body_enc_v1`, AES-256-GCM) instead of plaintext `body`
- `/api/v1/config/update` can enforce that profile with `edge_confidentiality_required=true`
- the platform’s bundle send helpers can emit `body_enc` by supplying `confidential_kid`

UM‑BMP does *not* mandate a transport. `agentd` provides a simple, robust mapping:

- **Ingress**: `POST /api/v1/edge/message` (any UM‑BMP envelope; idempotent by `msg_id`)
- **Egress**: `GET /api/v1/edge/outbox?node_id=...&cursor=0&limit=256` (per-node outbox cursor)

This makes it easy to layer:
- MQTT gateway (topic → HTTP bridge)
- WebSocket gateway
- LoRa hub backhaul

…without changing payload semantics.

## Node-initiated collaboration (platform extensions)

In addition to the canonical UM‑EAIS v0.1 message types, `agentd` implements a small set of platform-side extensions that
enable **node-initiated orchestration** (handoff to the coordinator) over the same ingress pipe:

- `WORKFLOW_SUBMIT` (node → platform): persist a new edge workflow (`edge_workflows`, `edge_workflow_steps`)
- `WORKFLOW_CANCEL` (node → platform): cancel an edge workflow (`CANCELED`)
- `DURABLE_WORKFLOW_SUBMIT` (node → platform): submit a **durable workflow** to the platform workflow engine
  (same semantics as `POST /api/v1/workflow/submit`, but transported over UM‑BMP via `POST /api/v1/edge/message`)
- `DURABLE_WORKFLOW_CANCEL` (node → platform): cancel a durable workflow
  (same semantics as `POST /api/v1/workflow/cancel`, but transported over UM‑BMP via `POST /api/v1/edge/message`)

Details: `docs/spec/um-eais/um-eais-platform-extensions-v0.1.md`

ACK note:
- For `WORKFLOW_SUBMIT` and `WORKFLOW_CANCEL`, the platform enqueues a best-effort outbox `WORKFLOW_ACK` so non-HTTP transports can observe
  submit/cancel outcomes.

Security note:
- For node-submitted durable workflows, the platform forces `allow_inline_api_keys=false` (nodes should not ship provider keys).
  Use daemon defaults (`--base-url`, `--api-key`, or provider keys) or a trusted gateway that authenticates and sets defaults.

## Endpoints

All endpoints require daemon auth when `agentd` is started with `--auth-token`.

### Ingest UM‑BMP envelopes

`POST /api/v1/edge/message`

Wire encoding:
- Default: `Content-Type: application/json` (envelope is JSON object).
- Optional MCU/gateway profile: `Content-Type: application/cbor` (envelope is CBOR map with the same keys/shape).
  - This is intended for constrained transports (LoRa/MQTT bridges) where JSON overhead is material.
  - The platform requires definite-length CBOR items and text-string map keys (no indefinite-length streaming items).
  - MCU encoder notes: `docs/spec/um-eais/mcu_cbor_encoder_notes.md`

This stores the envelope durably (`edge_inbox_messages`) and updates platform state:
- `NODE_HELLO`, `NODE_HEARTBEAT` update `edge_nodes`
  - `NODE_HEARTBEAT` best-effort persists `body.health` (and optional `battery_pct` / `rssi`) into `edge_nodes.health_json`,
    surfaced via `GET /api/v1/edge/node`.
- `NODE_CAPS_RSP` stores manifest + extracts tags/tools/presence
- `CONSENSUS_FRAME` validates `body.frame` as `edge_node_consensus_frame_v1`, relays it to recipient node
  outboxes, and persists a sender-side `health.consensus` summary
- `TASK_*` messages update `edge_tasks` + append `edge_task_events`
- `SENSOR_EVENT` appends `edge_sensor_events`

If the platform sees a new/unknown `caps_sha256`, it queues a `PLATFORM_CAPS_REQ` to the node outbox.

The repo now also ships a host-side node bring-up loop:
- `tools/agentd_edge_consensus_node.cpp` polls `GET /api/v1/edge/outbox`, processes relayed
  `CONSENSUS_FRAME` messages through the deterministic consensus core, and posts generated
  vote/commit frames back through `POST /api/v1/edge/message`
- `tests/agentd_edge_consensus_autonomous_smoke.sh` proves multi-node autonomous election/commit over that path

Envelope authenticity (optional, UM‑BMP auth v0.4):
- Envelopes MAY include an `auth` object:
  - `auth.alg` (string):
    - `"hmac-sha256"`: HMAC over canonical JSON bytes (`agent_json_c14n_v1`)
    - `"hmac-sha256-cbor"`: HMAC over deterministic CBOR bytes (`daemon/src/cbor_encode.*`)
    - `"ed25519"`: Ed25519 signature over canonical JSON bytes (`agent_json_c14n_v1`)
    - `"ed25519-cbor"`: Ed25519 signature over deterministic CBOR bytes (`daemon/src/cbor_encode.*`)
  - `auth.kid`: key id selecting an operator-provisioned shared secret (HMAC) or public key (Ed25519)
  - `auth.seq`: optional monotonic sequence number (anti-replay; when enabled by the platform)
  - `auth.sig`: base64 of the signature bytes:
    - 32 bytes for HMAC-SHA256
    - 64 bytes for Ed25519
  - Signing input (all algs):
    - signing input is the envelope with `auth.sig` removed (auth metadata like `kid`/`seq` stays in the signed bytes)
    - for `*-cbor` algs: deterministic CBOR bytes with:
      - definite lengths only
      - text-string map keys ordered by UTF‑8 byte length, then lexicographically by UTF‑8 bytes
      - integers encoded in minimal CBOR form
      - floats encoded as float64 when present
        - IMPORTANT: preserve numeric types for signing (a CBOR float must stay a CBOR float in the signing input,
          even if numerically integral like `87.0`)
        - Recommendation: avoid floats in signed envelopes when possible; prefer integers/fixed-point on MCUs
      - embedded helper: `agent_core` provides a minimal deterministic CBOR writer (`agent/cbor_det.h`) for MCU bring-up
- Operator controls:
  - `edge_auth.required` is surfaced via `GET /api/v1/config`.
  - `POST /api/v1/config/update` supports:
    - `edge_auth_required: true|false`
    - `edge_auth_require_ts: true|false` (when true, requires `ts_utc_ms > 0` on authenticated envelopes)
    - `edge_auth_max_skew_ms: <int>` (when > 0, rejects authenticated envelopes if `abs(now-ts_utc_ms)` exceeds this window)
    - `edge_auth_require_seq: true|false` (when true, requires monotonic `auth.seq` on authenticated envelopes)
    - `edge_auth_kid_policy: "any"|"match_node"|"node_prefix"` (best-effort binding between `from:"node:<id>"` and `auth.kid`)
    - `edge_auth_hmac_keys: { "<kid>": "<secret>", "<kid2>": null }` (null clears)
    - `edge_auth_ed25519_pubkeys: { "<kid>": "<base64(pubkey32)>", "<kid2>": null }` (null clears)
    - `edge_confidentiality_required: true|false` (when true, `/api/v1/edge/message` requires encrypted `body_enc`)
    - `edge_confidentiality_keys: { "<kid>": "<secret>", "<kid2>": null }` (null clears)
    - `edge_attest_required: true|false` (when true, invoke-mode `TASK_DONE` must include `result.attest` with a matching `result_sha256`)
    - `edge_attest_require_sig: true|false` (when true, invoke-mode `TASK_DONE` attestation signatures must verify)
  - Trust-root rotation control plane:
    - `GET /api/v1/edge/auth/trust_roots` returns a safe bundle with `rotation_epoch`, `hmac_kids`, `ed25519_pubkeys`, and optional `attest`
    - `POST /api/v1/edge/auth/trust_roots/rotate` applies a monotonic rotation epoch and updates the HMAC / Ed25519 trust-root set (`mode:"merge"` or `mode:"replace"`)
    - `POST /api/v1/edge/auth/trust_roots/send` enqueues the same signed trust-root bundle to a recipient node’s outbox as `PLATFORM_TRUST_ROOTS_BUNDLE`
  - Certificate-root control plane:
    - `GET /api/v1/edge/auth/cert_roots` returns the durable PEM certificate-root bundle with optional `attest`
    - `POST /api/v1/edge/auth/cert_roots/rotate` applies a monotonic certificate-root epoch and updates the current PEM root-chain set
    - `POST /api/v1/edge/auth/cert_roots/send` enqueues the same signed certificate-root bundle to a recipient node’s outbox as `PLATFORM_CERT_ROOTS_BUNDLE`
    - `edge_auth_require_manifest_cert_chain: true|false` requires `NODE_CAPS_RSP.body.manifest.identity.cert_pem`
      plus optional `cert_chain_pem[]` to verify against the stored PEM root set before the manifest is accepted
  - Confidential payload profile:
    - `body_enc` (`umbmp_body_enc_v1`) carries an AES-256-GCM encrypted JSON body object
    - `confidential_kid` on the manifest/trust-root/revocation/cert-root send helpers emits encrypted outbox envelopes
  - Per-node provisioning helpers:
    - `GET /api/v1/edge/auth/node_binding?node_id=...` shows the effective `kid_policy` match set for one node
    - `POST /api/v1/edge/auth/provision_node` provisions HMAC / Ed25519 trust roots for one node while enforcing the active `edge_auth_kid_policy`
  - Revocation control plane:
    - `GET /api/v1/edge/auth/revocations` returns the durable revoked-`kid` / revoked-node bundle with optional `attest`
    - `POST /api/v1/edge/auth/revocations/update` applies a monotonic revocation epoch and updates the revoked `kid` / node-id set (`mode:"merge"` or `mode:"replace"`)
    - `POST /api/v1/edge/auth/revocations/send` enqueues the same signed revocation bundle to a recipient node’s outbox as `PLATFORM_REVOCATIONS_BUNDLE`
  - Behavior:
    - If `edge_auth_required=true`: missing/invalid `auth` is rejected with HTTP 401 (no inbox persistence).
    - If `edge_auth_required=false`: unsigned envelopes are accepted, but if `auth` is present it must verify.
    - If `edge_confidentiality_required=true`: plaintext `body` is rejected with HTTP 401 and callers must supply valid `body_enc`.
    - Revoked `auth.kid` values and revoked `from:"node:<node_id>"` identities are rejected even if the underlying key material is still configured.

Task-loop correctness note (recommended, enforced by platform for known `TASK_*` types):
- Nodes SHOULD echo `idempotency_key` for all task lifecycle messages (`TASK_ACK/TASK_EVENT/TASK_DONE/TASK_FAILED`),
  matching the `TASK_ASSIGN.body.idempotency_key` they received. The platform rejects missing/invalid `idempotency_key`
  to prevent cross-attempt/cross-task corruption under retries.

Reliability note (important):
- The platform *persists* all inbound envelopes and dedupes persistence by `msg_id`.
- If a duplicate `msg_id` is received:
  - if the prior message was already processed, the platform returns early (dedupe) and does not re-apply side effects.
  - if the daemon crashed after persisting the inbox row but before applying side effects, the platform may reprocess the message
    (at-least-once) to avoid permanent drops.
- replay safety depends on message type: node-initiated handoffs are designed to be idempotent (`workflow_id` / `idempotency_key`),
    while event-style messages may produce duplicate logs if a crash occurred after side effects but before the processed marker was written.

Attestation note (v0.2/v0.3; enforceable via policy):
- If a node includes `body.result.attest`, the platform persists that blob under `edge_tasks.attest_json` and **excludes**
  `attest` from the `edge_tasks.result_sha256` hash surface to avoid self-referential hashing.
- Best-effort: if `attest` includes `{kid,alg,sig,ts_utc_ms,result_sha256}`, the platform verifies the signature when possible and
  emits evidence under task events (visible via `GET /api/v1/trace?trace_id=...`).
- When `edge_attest_required=true`, invoke-mode `TASK_DONE` must include `result.attest` with a valid `result_sha256` (matching
  the platform-computed hash). When `edge_attest_require_sig=true`, the attestation signature must also verify using the configured keys.

### Poll node outbox

`GET /api/v1/edge/outbox?node_id=...&cursor=0&limit=256`

Wire encoding:
- Default: JSON (`application/json`).
- Optional: CBOR (`Accept: application/cbor`) returns a CBOR-encoded response body with the same JSON-shaped fields.

Returns messages in ascending `outbox_id` order. The node should:
- persist its last `cursor_next`
- re-poll with `cursor=<cursor_next>`

### Debug registry helpers

- `GET /api/v1/edge/nodes`
  - compact node registry list
  - includes `consensus` when the node has emitted relayed decentralized consensus traffic
- `GET /api/v1/edge/node?node_id=...`
  - one registry record with tags/tools/hardware presence/health summary
  - includes `consensus` when present in `edge_nodes.health_json.consensus`
- `POST /api/v1/edge/node/consensus_runtime`
  - starts or stops the managed host-side `agentd_edge_consensus_node` helper for one edge node
- `GET /api/v1/edge/node/consensus_runtime?node_id=...`
  - reports the managed helper status plus the latest final result JSON emitted by the consensus tool
- `GET /api/v1/edge/node/caps?node_id=...`
- `GET /api/v1/edge/node/manifest_bundle?node_id=...`
- `POST /api/v1/edge/node/manifest_bundle/send`
- `POST /api/v1/edge/auth/trust_roots/send`
- `GET /api/v1/edge/auth/cert_roots`
- `POST /api/v1/edge/auth/cert_roots/rotate`
- `POST /api/v1/edge/auth/cert_roots/send`
- `POST /api/v1/edge/auth/cert_roots/verify_chain`
- `POST /api/v1/edge/auth/revocations/send`

Manifest bundle note:
- The manifest bundle endpoint returns the stored manifest plus derived `tools`, `tags`, and
  `hardware_presence` surfaces as one exportable artifact.
- When `AGENTD_RUN_ATTEST_HMAC_*` or `AGENTD_RUN_ATTEST_ED25519_*` is configured, `agentd`
  signs the bundle and returns an `attest` block so operators can treat it as a verified
  control-plane artifact rather than only an unsigned registry read.
- The send helper enqueues that same signed bundle to a recipient node’s outbox as
  `PLATFORM_MANIFEST_BUNDLE`, so non-HTTP node transports can consume peer identity/capability
  material over the shipped UM-BMP poll path.
- The edge-auth send helpers enqueue the current signed trust-root and revocation bundles as
  `PLATFORM_TRUST_ROOTS_BUNDLE` and `PLATFORM_REVOCATIONS_BUNDLE`, so nodes can poll
  control-plane trust changes through the same outbox transport instead of relying on a direct HTTP pull.
- The certificate-root helper does the same for PEM certificate-root bundles via
  `PLATFORM_CERT_ROOTS_BUNDLE`, giving operators a durable signed distribution lane for
  X.509-style root material even before full inline certificate-chain validation is enforced in envelope auth.
- Agentd can now own the lifecycle of the shipped autonomous consensus helper directly; `GET /api/v1/edge/node` and
  `GET /api/v1/edge/nodes` surface `consensus_runtime` when a node has a managed runtime record alongside the existing
  protocol-level `consensus` summary.
- The shipped helper is no longer the only place the loop exists: the election scheduler and frame-routing logic now
  live in reusable core code (`EdgeConsensusNodeLoop`), which is the intended stepping stone toward embedded/node-native
  adoption.
- Those same send helpers accept `confidential_kid`, which emits the outbox envelope with AES-GCM
  `body_enc` instead of plaintext `body` for peer/control-plane payload confidentiality.
- Operators can inspect those bundles, emit a CA file, and run `openssl verify` against candidate
  leaf/intermediate certs with `tools/edge_cert_roots_tool.py`.
- Operators can seal/open AES-GCM `body_enc` payloads with `tools/edge_confidentiality_tool`.
- `POST /api/v1/edge/auth/cert_roots/verify_chain` exposes the same current-root verification lane
  through `agentd` itself, returning structured success/failure metadata for candidate PEM chains.
- When a node manifest carries `manifest.identity.cert_pem` and optional `cert_chain_pem`, both
  `GET /api/v1/edge/node` and `GET /api/v1/edge/node/manifest_bundle` surface a best-effort
  `identity_cert_verify` summary against the current PEM root set.

### Platform helper: enqueue TASK_ASSIGN

`POST /api/v1/edge/task/assign`

Creates (or dedupes) an `edge_tasks` row and enqueues a UM‑BMP `TASK_ASSIGN` to the target node outbox.

Targeting:
- explicit: `node_id`
- capability routing: `match_any { requires_tools, tags_all, tags_any, tags_none }`

Trace correlation (best-effort):
- The platform accepts an optional `trace` object on `POST /api/v1/edge/task/assign` and forwards it to the node as
  `TASK_ASSIGN.trace` (envelope-level field). Recommended key: `trace.trace_id`.

Safety/rate gates (best-effort, platform-side):
- For `mode:"invoke"`, the platform requires a stored node manifest (`NODE_CAPS_RSP`) so it can inspect tool metadata.
- For `mode:"invoke"`, the platform requires `payload.args` to be an object, and validates it (best-effort) against the
  tool argument schema from the stored manifest when present:
  - preferred: `manifest.tools[].parameters_schema`
  - fallback: `manifest.tools[].parameters`
  The platform enforces a small, deterministic subset of JSON Schema keywords (`type`, `enum`, `required`,
  `properties`, `additionalProperties:false`, `items`) to catch shape mismatches early for MCU/actuator tools.
- For `mode:"invoke"`, if the tool definition includes a `resource_lock` key, the platform blocks parallel dispatch
  of another `TASK_ASSIGN` to the same node using the same lock while an existing task is `QUEUED` or `RUNNING`.
  The platform returns HTTP 429 (`resource_locked`) so orchestrators can backoff/retry.
- For `mode:"invoke"`, if the stored manifest tool definition includes a `result_schema`, the platform validates
  `TASK_DONE.body.result.data` against it (best-effort subset, fail-closed). This prevents malformed tool outputs from
  flowing into workflows and memory.
- Denies tools tagged with hazard `privacy_camera` by default (unless explicitly allowed via request).
- Denies `side_effect_level:"high"` by default (unless explicitly allowed via request).
- Enforces per-tool `rate_limit` from the manifest (`max_per_minute`, `cooldown_ms`) using platform-side state.

### Task status

`GET /api/v1/edge/task?task_id=...&step_id=...`

### Automation rules (SENSOR_EVENT → actions)

- `POST /api/v1/edge/rule/upsert`
- `GET /api/v1/edge/rules`
- `DELETE /api/v1/edge/rule?rule_id=...`

Rules are evaluated during `SENSOR_EVENT` ingestion. Supported actions:
- `type:"task_assign"`: enqueue a `TASK_ASSIGN` (invoke or agent) to a node (same behavior as `POST /api/v1/edge/task/assign`)
- `type:"durable_workflow_submit"`: submit a durable workflow to the platform workflow engine

For both action types, the platform runs the same cooldown gate: it fires when:
- `event_type` matches
- `confidence >= min_confidence`
- `cooldown_ms` has elapsed since `last_fired_utc_ms`

Durable workflow action notes:
- If `action.workflow.inputs.sensor_event` is missing, the platform injects it (best-effort) so workflow tasks can template against
  `${input.sensor_event.json:/...}`.

### Durable edge workflows (UM‑WF)

- `POST /api/v1/edge/workflow/submit`
- `POST /api/v1/edge/workflow/cancel`
- `GET /api/v1/edge/workflow?workflow_id=...&include_steps=1`
- `GET /api/v1/edge/workflows?status=...&limit=...`
- `GET /api/v1/edge/workflow/events?workflow_id=...&cursor=0&limit=256`
- `GET /api/v1/edge/workflow/stream?workflow_id=...&cursor=0` (SSE)

Workflows are executed by a background runner in `agentd`:
- dispatches `invoke_tool`/`run_agent` steps via `TASK_ASSIGN`
- supports `depends_on` sequencing, parallel fan-out, and `join` (`all|any`)

## Quick smoke flow (single node)

1) Node sends `NODE_HELLO` → platform queues `PLATFORM_CAPS_REQ`
2) Node polls outbox, receives caps req, responds with `NODE_CAPS_RSP`
3) Platform enqueues `TASK_ASSIGN` (`mode:"invoke"`) to call a device tool
4) Node replies with `TASK_ACK` + `TASK_EVENT` + `TASK_DONE`

Proof:
- `ctest` includes `agentd_edge_interop_smoke` and `agentd_edge_workflow_submit_message_smoke`.
- `ctest` includes `agentd_edge_auth_hmac_smoke`.
- `ctest` includes `agentd_edge_auth_provision_node_smoke`.
- `ctest` includes `agentd_edge_auth_revocations_smoke`.
- `ctest` includes `agentd_edge_auth_trust_roots_rotate_smoke`.
- `ctest` includes `agentd_edge_manifest_bundle_send_smoke`.
- `ctest` includes `agentd_edge_auth_ed25519_smoke`.
- `ctest` includes `agentd_edge_auth_hmac_cbor_wire_smoke`.
- `ctest` includes `agentd_edge_auth_ed25519_cbor_wire_smoke`.
- `ctest` includes `agentd_edge_task_attest_required_smoke`.

## Storage

DB tables are documented in `docs/DB.md`:
- `edge_nodes`
- `edge_inbox_messages`
- `edge_outbox_messages`
- `edge_tasks`
- `edge_task_events`
- `edge_sensor_events`
- `edge_tool_rate_state`
- `edge_rules`
- `edge_workflows`
- `edge_workflow_steps`
- `edge_workflow_events`
