# Agentd ↔ Web UI Protocol

Date: 2026-02-19

This document defines the **bidirectional** (UI → agentd → agent/tools → UI) protocol surface for rich interactions,
with a focus on:

- multi-agent-safe session handling (UI can create/list/resume sessions without collisions)
- explicit “artifact” signaling (agent can register host files like images/audio/video and provide playback hints)
- optional DB-backed troubleshooting queries (agentd can expose read-only DB endpoints when `--db-path` is enabled)

This project is rolling; schema evolves with versioned event types, but **typed events** are preferred over UI heuristics.

Event envelope reference:
- `docs/spec/run-events/run_events_v1.md`

## Goals

- Make troubleshooting possible by having a **machine-readable event log** instead of relying on regex parsing of tool output.
- Support **multiple agents/daemons** safely:
  - default UI behavior should not collide on a shared `"default"` session id
  - each daemon instance can use its own state/session root
- Support **broker deployments** cleanly when multiple agentd instances register under the same `agent_id`
  - UI may target a specific deployment via broker header `X-Agentd-Deployment`
  - UI scopes local session selection by deployment id to avoid collisions
- Allow agents to present created host files (image/audio/video) and request UI actions like “play audio once/N times”.

## Additional goals

- Provide stable, long-term, versioned public API guarantees with a clear deprecation policy.
- Deliver reliable audio/video playback with explicit gesture-based unlock flows that respect browser autoplay rules.

## Sessions

### Why a new-session endpoint is required

The UI historically defaulted to a fixed session id (`"default"`). With multiple browser tabs or multiple clients,
this causes collisions and makes debugging confusing.

### Endpoint: create a new session id

- `POST /api/v1/session/new`

Request (JSON, optional fields):
- `session_id` (string, optional): request a specific id (must be safe filename)
- `create_files` (bool, optional, default `true`): legacy name; currently means “eagerly create the session row in the DB” (by writing empty messages)

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `created` (bool) whether a new session was created (false means it already existed)
- `error` (string, optional)
- `err` (string, optional; alias of `error`)
- `code` (string, optional; stable-ish error code derived from the message)

Notes:
- The daemon still supports `GET /api/v1/sessions` and `GET /api/v1/session?session_id=...` for listing/resuming.
- Multi-agent safety is achieved by:
  - generating **unique** session ids by default (UUID-ish)
  - allowing daemon operators to set separate state/session roots per daemon instance
  - when using the broker, the UI also includes `brokerDeploymentId` in its session scope key so switching
    deployments does not reuse the same session id unintentionally

### Endpoint: upload session files (for multimodal prompts)

The Web UI can upload local files into a session folder, and then reference them in subsequent run requests via `input_files`.

- `POST /api/v1/session/upload`

Request (JSON):
- `session_id` (string, required)
- `files` (array, required): list of:
  - `name` (string, required): safe filename (ASCII, no path separators)
  - `mime` (string, optional): MIME type hint (used for UI preview + inlining decisions)
  - `data_base64` (string, required): file bytes (base64)

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `files` (array): list of accepted uploads:
  - `name` (string)
  - `mime` (string, optional)
  - `kind` (string, optional): `"image" | "audio" | "video" | "text" | "file"`
  - `path` (string): session-relative path (e.g. `uploads/<ts>_<idx>_<name>`)
  - `bytes` (number): decoded size
- `errors` (array, optional): rejected uploads:
  - `index` (number)
  - `name` (string, optional)
  - `code` (string, optional): `invalid_entry | invalid_name | missing_data | invalid_base64 | file_too_large | invalid_path | write_failed`
  - `error` (string, optional)
  - `bytes` (number, optional)
  - `max_bytes` (number, optional)

Notes:
- This endpoint requires daemon DB enabled/open (same as most session endpoints).
- Uploads are stored under `<sessions_root>/session_<session_id>/uploads/`.
- Upload size is capped (best-effort) to keep the daemon memory-bounded.
  - Config: `--upload-max-bytes <n>` or `AGENTD_UPLOAD_MAX_BYTES` (decoded bytes; default 32 MiB; `0` disables per-file cap).
- If no files are accepted, the endpoint returns `ok=false` and HTTP 400 with an `errors[]` array.
- The daemon also enforces a global HTTP body limit via `AGENTD_HTTP_MAX_BODY_BYTES` (default 64 MiB).

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

## Durable Scene (server-owned, DB-backed)

For UIs that render a “Scene” panel, agentd supports a **durable, server-owned Scene snapshot** per session.
This is intended to make Scene rendering refresh-proof without relying on browser storage.

Endpoints:
- `GET /api/v1/session/scene?session_id=...` → `{ ok, session_id, updated_unix_ms, scene }`
- `POST /api/v1/session/scene/apply` with `{ session_id, ops:[...] }` → `{ ok, apply, scene }`

Notes:
- These endpoints require the daemon DB to be enabled/open.
- The `clear` op exists server-side, but clients should treat it as destructive.
  - WebUI disables one-click “Clear Scene” and does not send `clear` ops from the browser.

Quick reference (client implementers): `docs/CLIENT.md`.

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
- `max_tool_call_args_chars` (int, optional): max length of a single tool call `arguments_json` (best-effort).
  - omitted → daemon default applies
  - `0` means unlimited
- `max_tool_result_chars` (int, optional): cap tool outputs before re-inserting into the prompt (best-effort).
  - omitted → daemon default applies
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

Tools ceiling:
- Run requests cannot exceed the daemon’s `--tools` setting (e.g., a daemon started with `--tools basic` will reject `tools:"host"`).
  Such requests return HTTP 400.

Automation profile override:
- `automation_profile` (string, optional): named profile that maps to daemon defaults.
  - `full`: `yolo_default=true`, `host_policy=full`, `policy_mode=off`
  - `guided`: `yolo_default=false`, `host_policy=readonly`, `policy_mode=audit`
  - `strict`: `yolo_default=false`, `host_policy=readonly`, `policy_mode=enforce`
  - `custom`: no override (use daemon config as-is)
- The daemon reports the applied profile as `effective_automation_profile` in the run response.
- If `automation_profile` is omitted, the daemon uses its compiled defaults (currently equivalent to `full`).

### Run replay bundles (deterministic audit)

`agentd` persists a **redacted replay bundle** for session-backed runs (best-effort) and exposes it via:

- `GET /api/v1/run/replay?run_id=...`
- `GET /api/v1/run/attestation?run_id=...` (small attestation bundle referencing the replay hash)

The replay bundle includes a redacted request/response snapshot plus tool records, and a deterministic hash token
(`agent_json_c14n_v1`) for offline verification.

See: `docs/WORKFLOWS.md` (Run replay bundles section).

### Run attestation bundles (signed replay references)

Attestation bundles are **small, signed references** to replay bundles. They are intended for CI,
audits, and external registries that want a stable proof of what was executed without embedding
the full replay payload.

See: `docs/spec/run_attestation_bundle_v1.md` (format + signing input).

### Multimodal inputs (`input_files`)

Run requests may optionally include `input_files` to reference session-uploaded files (via `POST /api/v1/session/upload`):

- `input_files` (array, optional): each entry is either:
  - a string `path` (session-relative), or
  - an object `{ path, name?, mime?, kind? }`

Behavior:
- `input_files` requires session persistence (`no_session=false`) because files live under the session folder.
- For OpenAI-compatible providers/models that support multimodal messages, agentd translates these into a `messages[].content` array
  containing text + image parts (and text blocks for non-image attachments).
- Not all models support image input. Some models (and some providers) will reject image parts with an HTTP `400`.
  - In that case, use a vision-capable model, or remove `input_files`.

## Tool registry introspection (UI → agentd)

The UI can query the daemon’s effective tool registry (schemas) to render tool docs and validate tool-call arguments:

- `GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=<id>`

Notes:
- When `session_id` is provided (and `tools=host`), the returned registry may include **session-scoped tools**
  such as `ui_wait_event` (which depends on a per-session client event log).
- Requests cannot exceed the daemon’s `--tools` ceiling; exceeding it returns HTTP 400.
- The response includes `daemon_tools` and (when provided) `requested_tools` for clarity.
- Requires auth when `--auth-token` is enabled (same as other control-plane endpoints).

## UI Actions (agent → UI)

Artifacts cover “render this file”; some workflows need explicit UI intent (e.g. show a notification, request an audio play).

Protocol:
- Host tool `ui_action` returns a JSON envelope with `data.tool="ui_action"` and `data.action={...}`.
- The host tool loop emits a derived `ui_action` event to the UI (similar to derived `artifact` events).

See: `docs/CLIENT.md` (UI actions and client RPC).

## Client events (UI → agentd)

In addition to prompts/runs, the UI can send structured client events back to the daemon (e.g. “audio finished playing”).
These are optionally appended into the session message history and (when `--db-path` is enabled) mirrored into the troubleshooting DB.

See: `docs/CLIENT.md` (client events, state snapshots, and RPC).

Moderator control plane:
- `POST /api/v1/moderator/directive` publishes a moderator directive (`type=moderator_directive`).
- `POST /api/v1/moderator/task` publishes a moderator task (`type=moderator_task_published`).
- `GET /api/v1/moderator/events?session_id=...` lists moderator events (filtered from client events).

Moderator directive payload (POST /api/v1/moderator/directive):
- `session_id` (string, required)
- `directive` (string, required)
- `scope` (string, optional; advisory routing label)
- `assignees` (string[], optional; advisory targets, empty → broadcast)
- `priority` (int, optional)
- `metadata` (object, optional)
- `append_to_session` (bool, optional)
- `actor` (object, optional; default role=moderator)

Moderator task payload (POST /api/v1/moderator/task):
- `session_id` (string, required)
- `title` (string, required)
- `detail` (string, optional)
- `assignees` (string[], optional; advisory targets, empty → broadcast)
- `tags` (string[], optional)
- `priority` (int, optional)
- `status` (string, optional; default `open`)
- `metadata` (object, optional)
- `append_to_session` (bool, optional)
- `actor` (object, optional; default role=moderator)


### Endpoint: list client events (file-backed)

For debugging without enabling SQLite, UIs can read the tail of the session-scoped client event log:

- `GET /api/v1/session/client_events?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

Legacy note: older builds read from `<sessions_root>/<session_id>.client_events.jsonl`. Current agentd builds store client events in the DB (canonical).
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

See: `docs/CLIENT.md` (client_wait_* tools).
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
- `path` (string, required): filesystem path to the file (absolute or relative)
  - In agentd sessions, `artifact_register` will **copy** the file into the session’s `out/` directory (best-effort) and return a session-relative `artifact.path` like `out/<name>`.
  - UIs should fetch it via `GET /api/v1/file?session_id=<sid>&path=<artifact.path>`.
- `kind` (string, optional): `"image" | "audio" | "video" | "text" | "file"`
- `mime` (string, optional): explicit MIME type (else best-effort from extension)
- `title` (string, optional): UI title/label
- `autoplay` (bool, optional): request UI to attempt autoplay (UI may require user consent)
- `repeat` (int, optional): for audio/video, request play N times (default 1)
- `blob_id` (string, optional): content-addressed blob id (`sha256:<hex>`) when stored in the blob tier

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
    - `blob_id` (string, optional)

### Event: `artifact`

Event object (same envelope shape as other `events` entries):
- `type`: `"artifact"`
- `data`: object
  - `step` (number, optional): tool-loop step
  - `tool_call_id` (string, optional)
  - `tool_name` (string, optional; `"artifact_register"`)
  - `artifact` (object): same as returned from tool output

UI behavior:
- Render images/videos/audio using `GET /api/v1/file?session_id=<sid>&path=<artifact.path>`
- If `artifact.blob_id` is present, UIs may also fetch bytes via `GET /api/v1/blob?blob_id=...` (range-friendly)
- For audio:
  - show a normal audio player
  - if `autoplay=true`, the UI may attempt playback only after explicit user opt-in
  - if `repeat>1`, provide a “Play xN” option (and implement best-effort loop)

### Backwards compatibility

The UI may keep heuristic media extraction as a fallback, but should prefer explicit `artifact` events.
