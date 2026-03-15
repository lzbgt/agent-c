# Agentic Framework Vision (Beyond OpenClaw)

Date: 2026-02-19

This document answers two questions:
1) As a **user/client**, what should a cutting-edge agentic framework deliver?
2) As a **system architect/designer**, what properties must the platform guarantee?

Important constraint: this repo does not carry a full OpenClaw spec. The list below is therefore **not** a claim
about OpenClaw’s capabilities. It is a concrete, fact-based **vision for this project** that aims to exceed a
typical gateway/plugin model by providing a full-stack, durable, policy-driven agent platform.

References:
- System goals + roadmap: `DESIGN.md`
- Current capabilities: `docs/HANDBOOK.md`, `docs/AGENTD.md`, `docs/WORKFLOWS.md`, `docs/MEMORY.md`
- Broker + control plane: `docs/BROKER.md`

Clarification: “Beyond OpenClaw” means **going past a gateway/plugin model** into a full-stack,
durable, policy-driven agent platform with evidence-grade runs, multi-agent orchestration,
and replayable outcomes.

---

## 1) User / Client Expectations (product-level)

These are the features a sophisticated user should feel immediately, regardless of whether they use WebUI,
CLI, or a custom client.

1) **Trustworthy execution**
   - Runs are **replayable** from captured inputs and tool outputs.
   - Deterministic audit trails with clear “what happened, when, and why.”

2) **Durable autonomy**
   - Long-running tasks survive restarts (durable workflows + job recovery).
   - Tasks can be paused, resumed, and re-tried without losing state.

3) **Memory that scales with time**
   - Progressive disclosure (index → timeline → details) with citations.
   - Salience + recap engines keep context useful as history grows.

4) **Multi-agent collaboration**
   - Coordinated teams with roles, shared memory, and explicit handoffs.
   - Quorum or policy-gated actions for safety-sensitive operations.

5) **Rich, multimodal interaction**
   - Streaming responses and tool calls, not just final results.
   - Native audio/voice workflows for “talk + tool” use-cases.

6) **Safety and consent**
   - User-approved UI actions (opens, downloads, automations).
   - Approval queues for sensitive actions (quorum/roles when needed).
   - Clear indications of “yolo” / elevated tool permissions.

7) **Deployment flexibility**
   - Runs locally, behind NAT, or in a brokered environment.
   - “Bring your own keys” and zero-secret-sharing deployment paths.

8) **Cost and latency control**
   - Explicit budget caps, retry policies, and model routing.
   - Diagnostics that identify provider failures quickly.

9) **Evidence-grade outcomes**
   - Exportable run bundles that capture inputs, tool IO, and outputs.
   - Built-in evaluation hooks so users can compare runs across models/settings.

10) **Interoperability**
   - OpenAI-compatible APIs plus standardized tool/agent protocols.
   - Portable artifacts so results can move between clients and deployments.

11) **Run comparison + regression guardrails**
   - Side-by-side run diffs with captured evidence.
   - Optional CI gating for critical workflows (eval packs).

12) **Interactive debugging + rewind**
   - Step-level tool call traces with rerun and “tweak + replay” workflows.
   - Safe prompt edits that preserve audit lineage and comparison history.

13) **Data governance**
   - Explicit retention windows and erase/export controls.
   - Redaction-aware artifacts with provenance preserved.

---

## 2) Architect / Designer Expectations (platform-level)

These are structural guarantees for maintainability, extensibility, and safety over time.

1) **Versioned contracts everywhere**
   - Capabilities, event schemas, and protocol envelopes are versioned.
   - Clients fail fast when required features are missing.

2) **Determinism + replay**
   - Tool inputs/outputs are captured with stable hashing.
   - Replay bundles can validate correctness in CI.

3) **Transport-agnostic core**
   - HTTP/SSE is the default; transports are pluggable.
   - Broker relays operate on transport-agnostic envelopes.

4) **Policy hooks and sandboxing**
   - Deterministic pre/post hooks for tool allow/deny and budgets.
   - Tool servers or plugin sandboxes with explicit resource limits.

5) **Isolation + resource accounting**
   - Hard caps on CPU/memory/disk/network for tool execution.
   - Deterministic accounting for cost, tokens, and resource usage.

6) **Security and identity**
   - Broker + connector provisioning with mTLS, JWT, and audit.
   - Signed capability manifests and attestations for runs.

7) **Tenant and secret isolation**
   - Clear boundaries between deployments, keys, and artifacts.
   - “Bring your own keys” is first-class with auditable key sources.

8) **Scalable scheduling**
   - Fairness, backpressure, and admission control are first-class.
   - Separate interactive vs batch workloads with clear limits.

9) **Portability**
   - Portable core with no env or filesystem dependency.
   - Embedded/VM targets can supply custom persistence and transport.

10) **Operational clarity**
   - Diagnostics endpoints reveal provider health and DB status.
   - Evidence bundles support incident response and audits.

11) **Evaluation as a contract**
   - Deterministic eval packs with fixed inputs and expected outputs.
   - Regression baselines tied to model/provider versions.

12) **Data lineage + retention controls**
    - Retention policy enforcement with export/erase APIs.
    - Provenance tags and redaction-aware evidence bundles.

---

## 3) What this repo already provides (facts)

From `DESIGN.md` and current docs:
- Portable `agent_core` C API with no env dependency.
- `agentd` daemon with HTTP/SSE APIs, durable workflows, and SQLite persistence.
- Broker with mTLS for connectors and OIDC/JWT for clients.
- WebUI and CLI clients; tool servers and plugin support.
- Memory retention + salience + recap architecture, with progressive disclosure.
- Replay bundles, signed attestation bundles, and WebUI run-diff evidence views for deterministic runs.
- Broker team registry CRUD (teams, members, quorum rules), sync/async team runs, persisted team-run approvals, and WebUI approval/run status surfaces.
- Agentd tool-level quorum enforcement with approval request/update/resolved events and approval queue APIs.
- Server-side run attestation signing for `/api/v1/run/attestation` (HMAC-SHA256 or Ed25519) with verification tooling and an Ed25519 smoke test.

---

## 4) Delta to become “cutting-edge”

The following areas drive the biggest step-change beyond a typical gateway/plugin model:

1) **Multi-agent team orchestration**
   - Agent groups, role constraints, shared memory spaces.
   - Policy/quorum gates for sensitive actions + deterministic approval records.

2) **Policy VM + deterministic enforcement**
   - Explicit policy manifests and deterministic hooks.
   - Replay validation with policy outcomes included.

3) **End-to-end attestation**
   - Signed run attestations + capability manifests.
   - Verifiable hashes for tool IO and workflow traces.

4) **Real-time media at the workflow layer**
   - Voice streaming as a first-class run type.
   - Broker relay for low-latency media signaling.

5) **Transport independence + capability negotiation**
   - Broker can introduce new transports without changing agentd.
   - Clients negotiate protocol + feature caps at connect time.

6) **Evaluation + regression gating**
   - Canonical eval packs for deterministic run checks in CI.
   - Model/version drift detection with reproducible baselines.

7) **Approval-ready collaboration**
   - WebUI approval queues with quorum + role context.
   - Async team runs with approval state changes reflected in SSE.

8) **Deterministic scheduling + isolation**
   - Admission control, quotas, and fairness guarantees.
   - Tool execution caps (CPU/memory/disk/network) with per-run budgets.

9) **Autonomous continuity + dynamic agent formation**
   - Background runs persist across UI refresh/reconnect (server-backed resume state).
   - Orchestrator can create/retire agents on demand and keep user input optional.

---

## 5) Near-term focus (actionable)

These are the most leveraged next steps grounded in current architecture:

1) **Team + role orchestration spec**
   - Define agent group model, shared memory, and quorum semantics.
   - Completed core v0 surface: persisted approvals + async team runs + WebUI approvals shipped.
   - Remaining work is deeper autonomy/policy layering on top of the shipped team surface.

2) **Policy hook MVP (implemented 2026-02-19)**
   - Deterministic policy hook interface (pre/post run + tool call).
   - Configurable allow/deny + budget caps with audit logging.

3) **Autonomy continuity (refresh-safe + server-backed state)**
   - Persist active job/watch state in client prefs so the UI can resume long-running runs after refresh/reconnect.
   - Use broker/daemon storage when available, with localStorage as fallback.

4) **Dynamic agent creation + orchestrator autoscale (spec + MVP)**
   - Orchestrator declares desired agent roles/capabilities; spawn adapter provisions agents to match.
   - Auto-retire idle agents and persist a run-time team roster so the user only intervenes on drift.

3) **Attestation bundle format**
   - Completed 2026-03-15: canonical replay hashing (`agent_json_c14n_v1`), signed
     attestation bundles, verification tooling, and host-smoke coverage are shipped.

4) **Voice workflow spec**
   - Audio streaming protocol and loopback/relay foundations are shipped.
   - Completed 2026-03-15: browser-side WebRTC workflow controls, a host-side agentd media peer RTP proof, and an agentd-managed media-peer runtime surface are shipped.
   - Remaining gap: replace the managed Node/Playwright child with an embedded long-lived agentd-native media service.

5) **Node consensus**
   - Centralized platform-led quorum and coordination are shipped.
   - Completed 2026-03-15: a deterministic node-native consensus core and simulation harness are shipped for
     vote/commit protocol definition, duplicate suppression, trust-epoch compatibility, split-brain, and quorum recovery.
   - Completed 2026-03-15: live UM-BMP relay/observability for `edge_node_consensus_frame_v1` is shipped through
     `/api/v1/edge/message`, `/api/v1/edge/outbox`, and node-read consensus summaries.
   - Completed 2026-03-15: autonomous node-side control loops are shipped in the host bring-up path via
     `agentd_edge_consensus_node` plus `agentd_edge_consensus_autonomous_smoke`.
   - Completed 2026-03-15: agentd can now own that loop as a managed runtime via
     `/api/v1/edge/node/consensus_runtime`, with runtime visibility surfaced on node-read APIs.
   - Still open: replace the current host-managed helper with embedded node-native adoption plus membership timers and
     recovery policy beyond the current bring-up/runtime harness.

6) **Scheduling + isolation MVP**
   - Completed 2026-03-15: admission control + per-run budgets + tool execution caps landed with
     evidence logs and host-smoke coverage.

7) **Data governance controls**
   - Completed 2026-03-15: retention enforcement, session erase + analytics export endpoints, and
     redaction-aware replay/attestation evidence are implemented with host-smoke coverage.

8) **Run comparison + evidence diff UX**
   - Completed 2026-03-15: side-by-side run diffs (replay + DB evidence + attestation)
     and browser-stored regression baselines are shipped in WebUI.

8) **Approval queues + tool-level quorum gating**
   - Completed 2026-03-15: WebUI approval queues, run-level quorum approvals, and
     agentd tool-level quorum enforcement are shipped.

These are tracked in `TODOS.md` with weighted priorities.

---

## 6) Acceptance tests (evidence-driven checks)

The expectations above should be **provable** by deterministic checks, not just by docs.
Below are concrete “evidence” tests that either exist now or are planned in `TODOS.md`.

Implemented evidence (facts):
- Policy hook enforcement + audit smoke: `tests/agentd_policy_hooks_smoke.sh`.
- Replay bundle sanity: `tests/agentd_run_replay_smoke.sh`.
- Replay attestation verification: `tests/agentd_run_attestation_ed25519_smoke.sh`.
- Team registry + team-run fan-out/quorum coverage:
  `tests/broker_team_runs_compose_smoke.sh`,
  `tests/broker_team_runs_quorum_compose_smoke.sh`,
  `tests/broker_team_run_events_sse_compose_smoke.sh`,
  `tests/broker_team_quorum_events_sse_compose_smoke.sh`.
- Tool-level approval gating:
  `tests/agentd_approval_rules_smoke.sh`,
  `tests/agentd_approval_roles_smoke.sh`,
  `ui/e2e/approval_queue_panel.spec.ts`.
- Run-diff evidence UX:
  `ui/e2e/run_diff_panel.spec.ts`.
- Durable workflow + budget guards: `tests/agentd_workflow_*_smoke.sh`.
- Memory timeline + search surfaces: `tests/agentd_workflow_memory_timeline_smoke.sh`,
  `tests/agentd_workflow_memory_search_smoke.sh`.
- Data governance:
  `tests/agentd_memory_retention_smoke.sh`,
  `tests/agentd_session_delete_governance_smoke.sh`,
  `tests/agentd_db_analytics_export_governance_smoke.sh`.
- OTA continuity: `tools/verify_ota_continuity.sh`.

Planned evidence (tracked in `TODOS.md`):
- Multi-agent team orchestration smoke (roles, shared memory, quorum gates).
- Voice workflow loopback (end-to-end low-latency media path).
