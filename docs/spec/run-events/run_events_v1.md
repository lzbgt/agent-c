# Run/Workflow Event Schema v1 (Draft)

Date: 2026-02-14

This document defines the **canonical event envelope** used by `agentd` runs, durable workflows,
and broker relay surfaces. The goal is a **single, versioned schema** so UIs, tools, and tests
can validate and reason about events deterministically.

## Goals

- Provide a single, versioned **event envelope** for run and workflow events.
- Keep the envelope stable, while allowing **event-specific payloads** to evolve.
- Enable schema validation in CI for docs and fixtures.

## Schema location

- JSON Schema: `docs/spec/run-events/schema/run_event_v1.schema.json`
- Fixtures: `docs/spec/run-events/fixtures/run_events_v1.jsonl`

## Envelope (v1)

Each event is a JSON object with:

- `type` (string, required): event type token (e.g. `tool_call`, `tool_result`, `assistant_delta`, `error`).
- `data` (any, optional): event payload; shape is **type-specific**.
- `ts_unix_ms` (number, optional): best-effort timestamp.
- `step` (number, optional): tool-loop step index (0 when `tools="none"`).
- `epoch` (number, optional): streaming/rotation epoch index.
- `source` (string, optional): origin label (daemon, broker, workflow, edge, client).
- `run_id` / `workflow_id` / `session_id` (optional): correlation identifiers when available.
- `schema` (string, optional): explicit schema identifier when needed for payloads.

The envelope is intentionally permissive so new event types can be introduced without breaking
existing consumers. Event-specific payload schemas will be added incrementally as the registry grows.

## Notes

- UIs should treat unknown `type` values as opaque debug cards.
- Producers should avoid breaking changes to existing `type` payloads. If needed, add a new `type`
  or include a `schema` version in `data`.
