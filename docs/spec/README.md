# Spec Index

Date: 2026-02-19
Status: reference index (rolling)

This folder contains versioned protocol and subsystem specifications. Each spec
declares its own status (draft/rolling/implemented) in the document header.

## Core protocol

- `run-events/run_events_v1.md`: canonical run/workflow event envelope + payload schemas.
- `run_request_refactor_v1.md`: run request refactor record (implemented).

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
