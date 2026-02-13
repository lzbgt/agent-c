# Next-Gen Architecture & Roadmap (Bleeding Edge)

Date: 2026-02-14

This document consolidates **future-proofing** work for the full stack:
- `agent_core` (portable core library)
- `agentd` (daemon + durable workflows)
- broker (secure relay / NAT traversal)
- WebUI (primary UX surface)

It is grounded in **current repo facts** (documented below) and proposes **design + implementation work** to make the
system more advanced, solid, efficient, and long-lived.

---

## Baseline facts (from this repo today)

### agent_core
- C API with no environment-variable dependency; host handles env/flags.
- Session model + character-budget compaction (portable, tokenizer-free).
- Optional persistence interface (`agent/persist.h`) and portable `.sess` codec.
- Tool registry + host callback execution interface.

### agentd
- Built as `agentd_lib` + thin `agentd` executable (optional embedded use).
- HTTP + SSE endpoints for runs, jobs, workflows, traces, diagnostics, etc.
- Durable workflows: DAG scheduling, retries/backoff, fairness, admission control, and restart recovery.
- Tool plugins (`--tool-plugin`) and out-of-process tool servers (`--tool-server-cmd`).
- SQLite-backed durable state (sessions, workflows, traces, memory, edge interop).
- Config store and runtime update endpoints (`/api/v1/config`, `/api/v1/config/update`).

### broker
- Public control plane with OIDC/JWT for clients and mTLS for agent connectors.
- WebSocket connector (`/v1/agent/connect`) + HTTP proxy + SSE relay.
- Postgres-backed registry + membership audit.

### WebUI
- React + Vite UI consuming agentd HTTP + SSE.
- Diagnostics panel, broker console, run settings, trace lookup.
- Playwright E2E tests (agentd host + broker flows).

---

## Target properties (future-proofing goals)

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

---

## Architecture proposals (grounded, phased)

### 1) agent_core (portable core)

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

### 2) agentd (daemon + workflows)

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

### 3) broker (control plane + relay)

**3.1 Durable relay envelopes**
- Store in-flight relay metadata (request id, trace id, status) for audit/replay.
- Idempotency keys for proxy/orchestrate to make retries safe (implemented).

**3.2 Multi-transport relay layer**
- Keep WebSocket but add a transport-agnostic relay interface so new transports
  (e.g., gRPC or WebTransport) can be introduced without changing business logic.

**3.3 Broker-side policy hooks**
- Central policy checks (rate limits, allowlists, cost ceilings) before relay.
- Provide a policy audit stream (SSE) for operator visibility.

### 4) WebUI (primary UX)

**4.1 Capability-aware UI**
- The UI should read `/api/v1/caps` and hide/disable unsupported features.

**4.2 Trace-first UX**
- Treat traces as the primary timeline; sessions are a lens, not the source of truth.
- Provide deterministic “replay from trace” workflows for debugging.

**4.3 Offline + state continuity**
- Persist client settings and last-known caps, and survive daemon restarts cleanly.

---

## Cross-cutting infrastructure

1) **Schema registry + CI checks**
   - Maintain versioned JSON Schemas for all public API payloads.
   - Enforce in CI (as already done for existing specs).

2) **Evidence & attestation**
   - Extend evidence bundles with cryptographic signatures.
   - Support “prove this run” artifacts that can be verified offline.

3) **Formal limits**
   - Standardize a single “limits” document generated from defaults (agentd + broker + core).
   - Keep docs + runtime consistent via tests.

---

## Phased roadmap (high leverage)

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

## Related docs

- `DESIGN.md` (current architecture and constraints)
- `docs/WORKFLOWS.md` (durable workflow semantics)
- `docs/BROKER.md` (broker protocols and auth model)
- `docs/PROTOCOL.md` (agentd ↔ WebUI protocol)
- `docs/DIAGNOSTICS.md` (health + provider smoke tests)
