# Spec Index

Date: 2026-02-19
Status: reference index (rolling)

This folder contains versioned protocol and subsystem specifications. Each spec
declares its own status (draft/rolling/implemented) in the document header.

## Core protocol

- `run-events/run_events_v1.md`: canonical run/workflow event envelope + payload schemas.
- `run_request_refactor_v1.md`: run request refactor record (implemented).
- `run_attestation_bundle_v1.md`: signed attestation bundle format for replay hashes (draft).
- `run_diff_v0.md`: run diff + evidence comparison contract (draft).
- `policy_hooks_v0.md`: policy hook contract (pre/post run + tool decisions).
- `approval_queue_v0.md`: approval queue + tool-level quorum gating (draft).
- `automation_mode_v0.md`: automation profile + moderator control plane (draft).
- `team_orchestration_v0.md`: team orchestration model (roles, shared memory, quorum gates).
- `orchestrator_console_v0.md`: WebUI orchestration console (roles/backends, run monitor, reload-safe).
- `autonomous_orchestrator_v0.md`: autonomous orchestration model (goal guard, low drift, dynamic allocation).
- `agent_spawn_adapter_v0.md`: spawn adapter CLI + provisioning contract (draft).
- `eval_pack_v0.md`: deterministic eval pack format for regression gating (draft).

## Streaming

- `streaming/core_stream_v1.md`: streaming contract and decoder expectations.
- `streaming/audio_stream_v0.md`: audio streaming signaling (draft).

## OTA

- `ota/agentd_ota_v0.md`: agentd OTA control plane and operator handoff.

## Tool plugins

- `tool_plugins_sandbox_v0.md`: plugin sandboxing and host policy rules.
- `tool_plugins_embedded_v0.md`: embedded/MCU tool plugin ABI (draft).

## Memory

- `memory/memory_dynamic_policy_v0.md`: retention + salience policy model.

## Agent/edge interop

- `agentd-agentd/agentd_agent_interop_v0_1.md`: agentd-to-agentd interop contract.
- `um-eais/`: embedded/edge interop specs, schemas, and fixtures.

## AVM + VM ports

- `avm_capsule_run_v0.md`: AVM capsule execution contract.
- `agent_vm_port_v0.md`: agent VM port integration contract.

## WebUI

- `webui_workflow_graph_editor_v1.md`: drag-and-drop workflow graph editor (implemented).
- `webui_server_prefs_v1.md`: server-side WebUI connection profile persistence (implemented).
