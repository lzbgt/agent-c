# Camera Capture Tool (Draft)

Date: 2026-01-30

This document specifies a dedicated host tool, `camera_capture`, intended to prevent runaway loops where a model
repeatedly captures camera frames and re-sends them to the UI.

## Problem

In tool mode, a model may keep requesting:
- `proc_exec` / `shell_exec` commands that capture images (ffmpeg/imagesnap/etc)
- `artifact_register` for each captured file

Even when the user task is conceptually complete, the model may not infer a stop condition, so deterministic guardrails
are required (`docs/LIMITS.md`). Additionally, a *purpose-built tool* makes it easier to:
- capture exactly once (single-shot semantics)
- return standardized metadata
- emit explicit UI events (`artifact`, optional `ui_action`)

## Goals

- Provide a **single tool call** that captures one image and returns an artifact payload.
- Keep the interface **portable** and host-controlled (backend selection is a host concern).
- Make it testable without camera hardware (mock backend writes a deterministic SVG image).
- Support UI integration without regex parsing: the tool loop emits a derived `artifact` event.

## Non-goals

- Cross-platform “guaranteed working” camera backend (depends on OS tooling and permissions).
- Recording video or streaming (future).
- Bypassing OS/browser permission prompts.

## Tool: `camera_capture` (tools=host)

Arguments (JSON):
- `path` (string, optional): output path (relative to tools root in scoped mode). Default: `camera_capture.svg` for mock, `camera_capture.jpg` for real backends.
- `backend` (string, optional): `"auto" | "ffmpeg" | "mock"`. Default: `"auto"`.
- `timeout_ms` (int, optional): max time for capture backend. Default: 8000.
- `title` (string, optional): UI label.
- `register_artifact` (bool, optional): include `data.artifact` metadata for UI. Default: true.
- `notify` (bool, optional): include a `data.action` of type `notify`. Default: false.

Return (tool output string; JSON envelope):
- `ok` (bool)
- `error` (string, optional)
- `data` (object)
  - `tool`: `"camera_capture"`
  - `backend`: `"mock" | "ffmpeg" | ..."`
  - `path`: requested path (string)
  - `resolved_path`: resolved host path (string)
  - `mime`: best-effort MIME
  - `artifact` (object, optional): compatible with `artifact_register` artifact schema
  - `action` (object, optional): compatible with `ui_action` action schema (e.g. notify)
  - `output` (string): human-readable summary

## Tool loop derived events

When the tool loop observes a `tool_result` for `camera_capture`:
- If the tool output contains `data.artifact`, the tool loop emits an `artifact` event.
- If the tool output contains `data.action`, the tool loop emits a `ui_action` event.

This ensures UI rendering does not depend on parsing stdout text.

## Safety defaults

Operators should cap camera capture in the daemon default `tool_call_limits_default` (see `/api/v1/config`), e.g.:
- `camera_capture=2` or `4`

And still keep global bounds:
- `max_steps_default`
- `max_tool_calls_total_default`

## Sandbox / policy notes

- In `agentd`, `camera_capture` is only exposed when the request is **exec-enabled** (daemon `yolo=true`), to avoid
  scoped/safe inspection runs from advertising device/capture tools.
- In `host_policy=readonly`, the tool is omitted and rejected (defense-in-depth), because it writes a file and may interact
  with device hardware.
