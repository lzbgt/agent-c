# Agentd ↔ Web UI Protocol (Draft)

Date: 2026-01-30

This document defines the **bidirectional** (UI → agentd → agent/tools → UI) protocol surface for rich interactions,
with a focus on:

- multi-agent-safe session handling (UI can create/list/resume sessions without collisions)
- explicit “artifact” signaling (agent can register host files like images/audio/video and provide playback hints)
- optional DB-backed troubleshooting queries (agentd can expose read-only DB endpoints when `--db-path` is enabled)

This project is rolling; schema may evolve, but **typed events** are preferred over UI heuristics.

## Goals

- Make troubleshooting possible by having a **machine-readable event log** instead of relying on regex parsing of tool output.
- Support **multiple agents/daemons** safely:
  - default UI behavior should not collide on a shared `"default"` session id
  - each daemon instance can use its own state/session root
- Allow agents to present created host files (image/audio/video) and request UI actions like “play audio once/N times”.

## Non-goals

- Stable, long-term, versioned public API guarantees (rolling project).
- Browser-autoplay bypass. Browsers typically require **user gesture** to play audio/video.

## Sessions

### Why a new-session endpoint is required

The UI historically defaulted to a fixed session id (`"default"`). With multiple browser tabs or multiple clients,
this causes collisions and makes debugging confusing.

### Endpoint: create a new session id

- `POST /api/v1/session/new`

Request (JSON, optional fields):
- `session_id` (string, optional): request a specific id (must be safe filename)
- `create_files` (bool, optional, default `true`): whether to create an empty session on disk immediately

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `created` (bool) whether a new session was created (false means it already existed)
- `error` (string, optional)

Notes:
- The daemon still supports `GET /api/v1/sessions` and `GET /api/v1/session?session_id=...` for listing/resuming.
- Multi-agent safety is achieved by:
  - generating **unique** session ids by default (UUID-ish)
  - allowing daemon operators to set separate state/session roots per daemon instance

### Endpoint: list recent artifacts for a session (from audit)

- `GET /api/v1/session/artifacts?session_id=...&max_bytes=...&max_artifacts=...&include_rotated=0|1&max_files=...`

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `count` (number)
- `artifacts` (array): flattened list of `artifact` events extracted from per-run audit JSONL

Notes:
- This endpoint is intended for UI indexing/browsing (cross-run).
- Artifact payload schema matches the `artifact` event described below.

### Endpoint: read session audit tail (file-backed)

UIs can inspect recent per-run audit entries (prompt + events) from the session-scoped audit JSONL:

- `GET /api/v1/session/audit?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

The audit file may be rotated when it grows large:
- current: `<session_id>.events.jsonl`
- backups: `<session_id>.events.jsonl.1`, `.2`, ...

## Runs (UI → agentd)

The Web UI drives the daemon by posting run requests (sync or async). In addition to standard settings (provider config,
tools mode, sandbox knobs), the daemon supports an explicit runaway-loop safety knob:

- `max_repeated_tool_calls` (int, optional): stops a run if the model repeats the **exact same tool call** too many times.
  - default: `12`
  - `0` disables the guard (not recommended)

Additionally, tool loops can be bounded by step count:

- `max_steps` (int, optional): max number of tool-loop steps (provider calls).
  - omitted → daemon default applies (see `/api/v1/config: daemon.max_steps_default`)
  - `0` means unlimited (not recommended for long-running daemons)

For robust runaway protection (especially when a model requests many tool calls in a single step), runs can also be bounded by:

- `max_tool_calls_total` (int, optional): max number of tool calls executed in total.
  - omitted → daemon default applies
  - `0` means unlimited
- `max_tool_calls_per_tool` (int, optional): max number of tool calls executed per tool name.
  - omitted → daemon default applies (often `0`/disabled)
  - `0` means unlimited

For targeted safety (and to avoid breaking benign high-frequency tools like `fs_read`), runs may also specify an explicit
per-tool map:

- `tool_call_limits` (array, optional): list of `{ tool: string, max_calls: int }`.
  - If an entry exists for a tool name:
    - `max_calls = 0` means unlimited for that tool (explicit disable).
    - otherwise the tool is capped at `max_calls`.
  - If no entry exists for a tool name, the tool falls back to `max_tool_calls_per_tool` (if set) or unlimited.
  - omitted → daemon default applies (see `/api/v1/config: daemon.tool_call_limits_default`)

This exists to prevent pathological “capture camera → send to UI → repeat” loops when the model does not infer a natural stop
condition from the conversation alone.

## Tool registry introspection (UI → agentd)

The UI can query the daemon’s effective tool registry (schemas) to render tool docs and validate tool-call arguments:

- `GET /api/v1/tools?tools=host|basic|none&tools_root=...&yolo=0|1&host_policy=full|readonly&session_id=<id>`

Notes:
- When `session_id` is provided (and `tools=host`), the returned registry may include **session-scoped tools**
  such as `ui_wait_event` (which depends on a per-session client event log).

## UI Actions (agent → UI)

Artifacts cover “render this file”; some workflows need explicit UI intent (e.g. show a notification, request an audio play).

Protocol:
- Host tool `ui_action` returns a JSON envelope with `data.tool="ui_action"` and `data.action={...}`.
- The host tool loop emits a derived `ui_action` event to the UI (similar to derived `artifact` events).

See: `docs/UI_ACTION.md`.

## Client events (UI → agentd)

In addition to prompts/runs, the UI can send structured client events back to the daemon (e.g. “audio finished playing”).
These are optionally appended into the session message history and (when `--db-path` is enabled) mirrored into the troubleshooting DB.

See: `docs/UI_CLIENT_EVENTS.md`.
See also: `docs/CLIENT_COLLAB.md`, `docs/CLIENT_STATE.md`, `docs/CLIENT_RPC.md`, `docs/CLIENT_PROBE.md`.

### Endpoint: list client events (file-backed)

For debugging without enabling SQLite, UIs can read the tail of the session-scoped client event log:

- `GET /api/v1/session/client_events?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

This reads from `<sessions_root>/<session_id>.client_events.jsonl` and returns parsed JSON objects.
When `include_rotated=1`, the daemon may also include data from rotated backups (`.client_events.jsonl.1`, `.2`, …)
to fill the requested `max_bytes` budget.

### Endpoint: list observed clients (file-backed)

For multi-client debugging, the daemon can list distinct clients that have posted events recently:

- `GET /api/v1/session/clients?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

Clients are reported based on the `payload.client` identity fields in the client event log.

## Waiting for UI acknowledgements (host tool)

When using tool loops, an agent may need to wait for a UI/user acknowledgement within a single run (e.g. audio playback finished).
For this, the host toolset exposes a cooperative polling tool:
- `ui_wait_event`
- `ui_wait_any` (OR join)
- `ui_wait_all` (AND join)

See: `docs/UI_WAIT_EVENT.md`.
Preferred (client-agnostic) names for the same tools:
- `client_wait_event`
- `client_wait_any`
- `client_wait_all`

## Artifacts

### Problem

The UI can preview files by heuristically extracting `*.png|*.mp3|*.mp4` tokens from tool output, but:
- it is fragile (false positives, missing files, no metadata)
- it cannot express UI intent (e.g. “this file is an audio clip; please play it twice”)
- it makes repeated tool calls more likely (agent can’t reliably know the UI “received” the artifact)

### Design

Add an explicit artifact signaling path:

1. Agent (via a host tool) “registers” a file it created/produced.
2. The host tool returns a structured JSON envelope that includes `data.tool="artifact_register"` and `data.artifact={...}`.
3. The tool loop emits a derived `artifact` event to UIs.

### Host tool: `artifact_register` (tools=host)

Purpose:
- Tell the UI “this host file is an artifact; please render it as media and apply playback hints”.

Arguments (JSON):
- `path` (string, required): path to the file (relative to tools root when in scoped mode)
- `kind` (string, optional): `"image" | "audio" | "video" | "text" | "file"`
- `mime` (string, optional): explicit MIME type (else best-effort from extension)
- `title` (string, optional): UI title/label
- `autoplay` (bool, optional): request UI to attempt autoplay (UI may require user consent)
- `repeat` (int, optional): for audio/video, request play N times (default 1)

Return (tool output string; JSON envelope):
- `ok` (bool)
- `error` (string, optional)
- `data` (object)
  - `tool`: `"artifact_register"`
  - `artifact`: object containing resolved metadata:
    - `path` (string)
    - `resolved_path` (string, best-effort)
    - `kind` (string)
    - `mime` (string)
    - `size_bytes` (number, optional)
    - `mtime_unix_ms` (number, optional)
    - `title` (string, optional)
    - `autoplay` (bool, optional)
    - `repeat` (number, optional)

### Event: `artifact`

Event object (same envelope shape as other `events` entries):
- `type`: `"artifact"`
- `data`: object
  - `step` (number, optional): tool-loop step
  - `tool_call_id` (string, optional)
  - `tool_name` (string, optional; `"artifact_register"`)
  - `artifact` (object): same as returned from tool output

UI behavior:
- Render images/videos/audio using `GET /api/v1/file?path=...&yolo=...`
- For audio:
  - show a normal audio player
  - if `autoplay=true`, the UI may attempt playback only after explicit user opt-in
  - if `repeat>1`, provide a “Play xN” option (and implement best-effort loop)

### Backwards compatibility

The UI may keep heuristic media extraction as a fallback, but should prefer explicit `artifact` events.

## Camera capture (host tool)

To avoid models repeatedly capturing camera frames via `proc_exec` loops, the host toolset includes a dedicated
single-shot tool: `camera_capture`.

See: `docs/CAMERA_CAPTURE.md`.
