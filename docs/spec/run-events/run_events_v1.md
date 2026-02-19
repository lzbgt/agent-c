# Run/Workflow Event Schema v1

Date: 2026-02-19

This document defines the **canonical event envelope** used by `agentd` runs, durable workflows,
and broker relay surfaces. The goal is a **single, versioned schema** so UIs, tools, and tests
can validate and reason about events deterministically.

## Goals

- Provide a single, versioned **event envelope** for run and workflow events.
- Keep the envelope stable, while allowing **event-specific payloads** to evolve.
- Enable schema validation in CI for docs and fixtures.

## Schema location

- JSON Schema: `docs/spec/run-events/schema/run_event_v1.schema.json`
- Payload schemas (v1):
  - `docs/spec/run-events/schema/run_event_payload_assistant_delta_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_assistant_message_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_user_message_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_tool_call_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_tool_result_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_llm_usage_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_artifact_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_ui_action_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_heartbeat_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_error_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_workflow_created_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_workflow_cancel_requested_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_task_status_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_workflow_status_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_workflow_done_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_workflow_budget_exceeded_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_memory_checkpoint_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_workflow_canceled_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_step_state_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_step_retry_scheduled_v1.schema.json`
  - `docs/spec/run-events/schema/run_event_payload_step_dispatched_v1.schema.json`
- Fixtures: `docs/spec/run-events/fixtures/run_events_v1.jsonl`
- CI: `run_events_spec_sanity_tests` validates schema presence and fixture envelopes.

## Envelope (v1)

Each event is a JSON object with:

- `type` (string, required): event type token (e.g. `tool_call`, `tool_result`, `assistant_delta`, `error`).
- `data` (any, optional): event payload; shape is **type-specific**.
- `ts_unix_ms` (number, optional): best-effort timestamp.
- `step` (number, optional): tool-loop step index (0 when `tools="none"`).
- `epoch` (number, optional): streaming/rotation epoch index.
- `source` (string, optional): origin label (daemon, broker, workflow, edge, client).
- `run_id` / `workflow_id` / `session_id` (optional): correlation identifiers when available.
- `schema` (string, optional): explicit schema identifier for the payload (e.g. `run_event_payload_tool_call_v1`).
  For the standard v1 payloads listed below, this field is **required** and must match the expected schema name.

The envelope is intentionally permissive so new event types can be introduced without breaking
existing consumers. Event-specific payload schemas will be added incrementally as the registry grows.

## Payload expectations (validated in CI)

The fixture set is validated for these common event payloads:

- `assistant_delta`: `data.delta` string; optional `data.step`, `data.epoch` (integers >= 0).
-   Schema: `run_event_payload_assistant_delta_v1`
- `assistant_message`: `data.assistant_content` string, `data.has_tool_calls` integer >= 0; optional multimodal fields.
-   Schema: `run_event_payload_assistant_message_v1`
- `user_message`: `data.user_content` string; optional multimodal fields.
-   Schema: `run_event_payload_user_message_v1`
- `tool_call`: `data.tool_name`, `data.tool_call_id`, `data.arguments_json` strings.
-   Schema: `run_event_payload_tool_call_v1`
- `tool_result`: `data.tool_name`, `data.tool_call_id`, `data.content` strings.
-   Schema: `run_event_payload_tool_result_v1`
- `llm_usage`: `data.prompt_tokens`, `data.completion_tokens`, `data.total_tokens` integers >= 0.
-   Schema: `run_event_payload_llm_usage_v1`
- `artifact`: `data.path`, `data.kind` strings; optional `data.mime`, `data.repeat`, `data.autoplay`.
-   Schema: `run_event_payload_artifact_v1`
- `ui_action`: `data.action.type` string.
-   Schema: `run_event_payload_ui_action_v1`
- `heartbeat`: `data.phase` integer >= 0.
-   Schema: `run_event_payload_heartbeat_v1`
- `error`: `data.reason` string; optional `data.error`, `data.steps_executed`, `data.max_steps`.
-   Schema: `run_event_payload_error_v1`
- `workflow_created`: `data.workflow_id` string; optional `data.status`, `data.trace_id`, `data.session_id`, `data.goal`, `data.priority`, `data.steps`.
-   Schema: `run_event_payload_workflow_created_v1`
- `workflow_cancel_requested`: `data.workflow_id` string; `data.status` string; `data.cancel_requested` bool.
-   Schema: `run_event_payload_workflow_cancel_requested_v1`
- `task_status`: `data.workflow_id`, `data.task_id`, `data.status` strings; `data.attempt`, `data.max_attempts` integers.
-   Schema: `run_event_payload_task_status_v1`
- `workflow_status`: `data.workflow_id`, `data.status` strings; optional `data.prev_status`, `data.cancel_requested`.
-   Schema: `run_event_payload_workflow_status_v1`
- `workflow_done`: `data.workflow_id` string; `data.ok` bool; `data.status` string; `data.result_json_present` bool.
-   Schema: `run_event_payload_workflow_done_v1`
- `workflow_budget_exceeded`: `data.workflow_id` string; `data.reason` string; budget counters (integers).
-   Schema: `run_event_payload_workflow_budget_exceeded_v1`
- `memory_checkpoint`: `data.workflow_id`, `data.task_id` strings; `data.checkpoint` object (`path`, `sha256`).
-   Schema: `run_event_payload_memory_checkpoint_v1`
- `workflow_canceled`: `data.workflow_id` string.
-   Schema: `run_event_payload_workflow_canceled_v1`
- `step_state`: `data.workflow_id`, `data.step_id`, `data.state` strings.
-   Schema: `run_event_payload_step_state_v1`
- `step_retry_scheduled`: `data.workflow_id`, `data.step_id` strings; `data.attempt_next`, `data.ready_utc_ms` integers.
-   Schema: `run_event_payload_step_retry_scheduled_v1`
- `step_dispatched`: `data.workflow_id`, `data.step_id`, `data.node_id`, `data.idempotency_key` strings; `data.attempt`, `data.outbox_id` integers.
-   Schema: `run_event_payload_step_dispatched_v1`

Notes:
- `workflow_created` is used by both durable workflows (status/trace/session fields) and edge workflows (goal/priority/steps).
  Producers should emit whichever fields apply; consumers must treat the payload as a permissive union.

## Assistant message payload (current)

When `type="assistant_message"`, the `data` object includes:

- `assistant_content` (string): assistant text (multimodal prefix removed when present).
- `has_tool_calls` (number): `1` if tool calls were present, else `0`.
- `assistant_mm_json` (string, optional): raw JSON string from the multimodal prefix (`__AGENT_MM_V1__...`).
- `assistant_mm_bytes` (number, optional): byte length of the raw multimodal JSON.
- `assistant_mm_truncated` (number, optional): `1` when `assistant_mm_json` was capped for event safety.

Consumers that need structured multimodal content should parse `assistant_mm_json` when present.

## User message payload (current)

When `type="user_message"`, the `data` object includes:

- `user_content` (string): user text (multimodal prefix removed when present).
- `user_mm_json` (string, optional): raw JSON string from the multimodal prefix (`__AGENT_MM_V1__...`).
- `user_mm_bytes` (number, optional): byte length of the raw multimodal JSON.
- `user_mm_truncated` (number, optional): `1` when `user_mm_json` was capped for event safety.

Consumers that need structured multimodal content should parse `user_mm_json` when present.

## Notes

- UIs should treat unknown `type` values as opaque debug cards.
- Producers should avoid breaking changes to existing `type` payloads. If needed, add a new `type`
  or version the payload schema and update the `schema` identifier accordingly.
