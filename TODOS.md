# Roadmap / TODOs (next highest leverage)

Date: 2026-02-03

This file is the forward-looking roadmap. It intentionally focuses on **framework-level** leverage (capabilities that unlock many downstream features),
with each item having a concrete “proof” so it can be verified in CI (`tools/verify.sh` / `ctest` / `tools/verify_compose_stack.sh`).

## Baseline (already shipped)

- Broker relay with OIDC + mTLS, plus WebUI broker mode.
- Provider reliability (retry/backoff/timeouts) + smoke tests.
- DB-backed job durability + truthful terminal status (`interrupted`) after daemon restart.
- Structured durable memory + deterministic conflict handling + memory policy knobs per run request.
- Multi-agent orchestration:
  - daemon: `POST /api/v1/orchestrate` (fan-out + optional session writeback)
  - broker: `POST /v1/orchestrate` (fan-out across multiple agents via broker relay)

## P0 (highest leverage; makes this a “framework”)

### 1) Tool plugins: make tools composable without recompiling

Problem (fact from code/docs):
- `agentd` has a `ToolExtension` injection point, but it currently requires **in-process embedding** (or code changes) to add tools (`docs/AGENTD_LIB.md`).
- For an “agentic framework”, adding new capabilities must be a packaging problem, not a fork/rebuild problem.

Deliverables:
- `agentd` supports `--tool-plugin <path>` (repeatable) to load **tool plugins** at runtime.
- Stable plugin ABI (dlopen-based) that:
  - provides a manifest of tool schemas (name/description/parameters)
  - executes tools and returns JSON result
- Docs + sample plugin.

Proof:
- A new smoke test starts `agentd` with a plugin, confirms `/api/v1/tools` lists the new tool, and runs a stub tool-loop that invokes it.

### 2) “Docs as truth”: auto-check + sync critical docs with runtime truth

Problem (fact from repo):
- `docs/DB.md` describes schema v6, but the current daemon DB schema is v8 (see `daemon/src/agent_db.cpp`).
- Drift in docs makes the project hard to extend safely (client integrations depend on accurate schemas).

Deliverables:
- Update the DB doc to the current schema and add a small “how to verify” section.
- Add a lightweight automated check (CI/ctest) that fails if docs claim a different schema version than the binary.

Proof:
- `ctest` includes a unit test that asserts:
  - doc mentions current schema version
  - doc includes required tables (`jobs`, `scene_states`, etc.)

### 3) API contracts: ship an OpenAPI spec for agentd + broker

Problem:
- The project already has a rich HTTP surface, but it is only specified in prose (`docs/PROTOCOL.md`, `docs/BROKER.md`).
- Without a machine-readable spec, clients drift and you can’t generate typed SDKs.

Deliverables:
- `docs/openapi/agentd.yaml` and `docs/openapi/broker.yaml` capturing endpoints + request/response schemas.
- WebUI uses generated types (or a checked-in TS client) derived from the spec.

Proof:
- `tools/verify.sh` runs a spec validation step (lint) and WebUI build uses generated types without manual drift.

### 4) Observability: trace a single user intent across UI ⇄ agentd ⇄ broker

Problem:
- Debugging distributed agent systems requires correlation IDs and structured logs; otherwise issues become “he said/she said”.

Deliverables:
- Introduce `trace_id` (client-provided or daemon-generated) and propagate it:
  - UI requests
  - daemon run records + events
  - broker relay audits + orchestrate results
- Add an endpoint/UI view to pull a run’s correlated timeline (events + tool calls + retries).

Proof:
- A smoke test asserts `trace_id` round-trips from request → DB rows → response.

### 5) Workflow engine: orchestration becomes a first-class “agent graph”

Problem:
- Fan-out is useful, but real multi-agent work needs dependencies (DAG), budgets, and explicit aggregation strategies.

Deliverables:
- Extend orchestration request to support:
  - task graph (dependencies)
  - per-task budgets (steps, tool caps, memory policy)
  - aggregation strategy (best-of, vote, summarize, strict consensus)
- Expose an LLM-callable tool (host-side) to invoke orchestration safely (guarded by policy).

Proof:
- A deterministic stub-server test executes a DAG and validates topological ordering + aggregation behavior.

## P1 (big wins after P0)

### 6) Tool servers (subprocess / stdio) + remote device tools

Deliverables:
- Spawn a “tool server” process (stdio JSON-RPC or similar) that registers tools and executes them out-of-process.
- Use this to integrate:
  - hardware/device tools (ESP32 via serial/MQTT)
  - browser automation (Playwright) without embedding it directly into agentd

Proof:
- A smoke test runs a tool server that provides a single tool and verifies end-to-end execution.

### 7) Memory v2: semantic retrieval (embeddings / FTS) + dedupe

Deliverables:
- Add an indexable memory store (SQLite FTS5 or embedding vectors) and retrieval ranking.
- Add explicit memory scopes:
  - per-session, per-agent, per-user (broker identity), and shared team memory.

Proof:
- Unit tests show:
  - retrieval returns the most relevant memory items
  - conflicting facts are resolved deterministically

### 8) “Agent packs”: versioned profiles and shareable capabilities

Deliverables:
- Define a pack format:
  - system prompt profile
  - tool enablement + policy
  - default model/provider routing
- WebUI can select packs and export/import them.

Proof:
- A pack can be exported, imported, and produces deterministic effective config snapshots.
