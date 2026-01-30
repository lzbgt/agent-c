# Agentd ↔ Web UI Protocol (Draft)

Date: 2026-01-30

This document defines the **bidirectional** (UI → agentd → agent/tools → UI) protocol surface for rich interactions,
with a focus on:

- multi-agent-safe session handling (UI can create/list/resume sessions without collisions)
- explicit “artifact” signaling (agent can register host files like images/audio/video and provide playback hints)

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

