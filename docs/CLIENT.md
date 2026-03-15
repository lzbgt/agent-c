# Client Collaboration and API (Living)

Date: 2026-02-19

Design context: see `DESIGN.md` for system architecture; this document focuses on client-facing collaboration and APIs.

This document consolidates the client-facing design and API surfaces for `agentd`.
It replaces the previous split across:
- CLIENT_AGENTD_SPEC
- CLIENT_COLLAB
- CLIENT_ENTITIES
- CLIENT_PROFILES
- CLIENT_PROBE
- CLIENT_RPC
- CLIENT_STATE
- UI_ACTION
- UI_CLIENT_EVENTS
- UI_WAIT_EVENT

If you are implementing a new client (mobile app, Slack bot, CLI front-end, etc.),
this is the canonical reference.

## Scope

- Client identity and session safety
- Bidirectional collaboration (events, UI actions, client RPC)
- Durable Scene (entities) model
- Deterministic definition-of-done (DoD) handshakes
- Client-facing HTTP API endpoints

See also:
- docs/PROTOCOL.md for the broader run + artifact protocol
- docs/spec/run-events/run_events_v1.md for event schema envelopes
- docs/LIMITS.md for size and safety caps

## Concepts

- client: any UI/integration that drives the daemon (WebUI, Slack, mobile)
- session: namespace for message history, durable Scene, client events
- run: one LLM execution (sync or async)
- event: structured log record (artifacts, UI actions, client acks)
- Scene: a per-session, server-owned JSON object mapping entity_id -> entity

## Authentication and correlation

- If `agentd` starts without an auth token, requests are accepted without auth.
- If `agentd` is configured with `--auth-token`, clients must send:
  - Authorization: Bearer <token>
- Health endpoints are always unauthenticated:
  - GET /api/v1/health
  - GET /api/v1/ready
  - GET /metrics

Correlation headers:
- X-Request-Id: client-provided request id (echoed back by the daemon)
- X-Trace-Id: for /api/v1/run and /api/v1/run_async, if trace_id is omitted
  in JSON, the daemon uses the header value (must pass trace_id_is_safe)

## Session id safety

session_id is treated like a filename key and must pass session_id_is_safe:
- length 1..200
- characters: [A-Za-z0-9_.-]
- must not contain "/", "\\", or traversal segments ("." or "..")

Clients should treat invalid ids as a client-side bug and avoid retries.

## Client identity

Clients should send a client identity with events and run requests:

{
  "client": {
    "id": "webui",
    "kind": "webui",
    "instance_id": "tab-123"
  }
}

Fields:
- client.id: stable logical id (webui, slack, mobile, etc)
- client.kind: human-friendly kind (optional but recommended)
- client.instance_id: per-instance id (tab, device), used for debugging

All client identity fields are bounded (max 200 chars, no control chars).

## Event model

Client events are the append-only collaboration surface. They are used for:
- UI acknowledgements (artifact_rendered, ui_action_shown)
- client RPC results/progress
- client state snapshots

### Client event endpoint (client -> agentd)

POST /api/v1/session/client_event
POST /api/v1/session/ui_event  (legacy alias)

Request shape:

{
  "session_id": "sess-...",
  "type": "artifact_rendered",
  "ts_unix_ms": 1730000000000,
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": { "tool_call_id": "call_123", "path": "out/foo.wav" },
  "append_to_session": true
}

Notes:
- ts_unix_ms is optional (daemon will use "now")
- append_to_session default is true; when true, a synthetic user message is
  appended so the next run can see the event in session history

Persistence:
- DB mirror: client_events table (when --db-path is enabled)
- File-backed log (always): <session_id>.client_events.jsonl
  - rotated when large; canonical store is DB when enabled

Read-back helpers:
- GET /api/v1/session/client_events?session_id=...
- GET /api/v1/db/client_events?session_id=...&limit=...&offset=...
- GET /api/v1/session/clients?session_id=... (distinct clients observed)

### Suggested event types

- artifact_rendered
- ui_action_shown
- client_state
- client_capabilities
- client_rpc_result
- client_rpc_progress
- notification_shown
- notification_ack

Moderator events (server-issued, stored as client events):
- moderator_directive
- moderator_task_published
  - directive `data` may include `scope` and `assignees` (advisory routing hints)
  - task `data.task` may include `assignees` (advisory routing hints)

## Agent -> client actions (ui_action)

The host tool `ui_action` lets the agent request UI-side actions.
It emits a ui_action event with structured payload.

Tool name: ui_action
Arguments:
- type (string, required)
- title (string, optional)
- message (string, optional)
- path (string, optional)
- mime (string, optional)

Allowlisted action types (initial v1):
- notify
- request_client_state
- client_rpc
- collab_rpc (alias for client_rpc)
- client_probe (legacy alias; prefer client_rpc)

### client_rpc action shape (v1)

{
  "type": "client_rpc",
  "title": "Optional short title",
  "rpc_id": "rpc_123",
  "rpc": {
    "kind": "dom_query",
    "args": { "selector": "#app", "fields": ["tag", "id"] }
  },
  "auto_run": true,
  "side_effects": false
}

Notes:
- rpc_id is the correlation id (recommended: tool_call_id)
- rpc.kind must be allowlisted by the client
- rpc.args must be bounded
- auto_run requests immediate execution when permitted

## Client RPC (collaboration surface)

Client RPC is the universal request/response mechanism:
1) agent requests a client_rpc via ui_action
2) client executes a allowlisted handler
3) client posts client_rpc_result and optional client_rpc_progress events
4) agent waits deterministically using client_wait_event/client_wait_any/client_wait_all

### Result event: client_rpc_result

{
  "session_id": "...",
  "type": "client_rpc_result",
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": {
    "rpc_id": "rpc_123",
    "request_tool_call_id": "call_abc",
    "rpc_kind": "media_observe",
    "ok": true,
    "result": { "observing": 1 }
  },
  "append_to_session": false
}

### Progress event: client_rpc_progress

{
  "session_id": "...",
  "type": "client_rpc_progress",
  "data": {
    "rpc_id": "rpc_123",
    "name": "ended",
    "payload": { "ts_unix_ms": 0 }
  }
}

### Safety model

- Clients enforce a strict allowlist of rpc kinds
- Outputs must be bounded (size and count caps)
- Side effects are opt-in (client-side consent gates)

## Entity / Scene model (client-agnostic UI)

DOM mutation is WebUI-specific. The universal abstraction is entities:

- entity_query: read-only introspection
- entity_apply: create/update/delete/action/clear

The daemon can persist a durable Scene (server-side) per session:
- GET /api/v1/session/scene
- POST /api/v1/session/scene/apply

### entity_query example

{
  "kind": "entity_query",
  "args": { "entity_kind": "canvas2d", "id_prefix": "plot-", "limit": 50 }
}

### entity_apply example

{
  "kind": "entity_apply",
  "side_effects": true,
  "args": {
    "ops": [
      {"op": "create", "id": "plot-1", "entity_kind": "canvas2d", "title": "Sine plot", "props": {"width": 640, "height": 240}},
      {"op": "action", "id": "plot-1", "action": "plot_sine", "args": {"amplitude": 1, "frequency": 2}}
    ]
  }
}

### WebUI Canvas2D scripts

The WebUI supports a canvas2d entity with a scriptable draw surface:
- ctx: CanvasRenderingContext2D
- canvas: the canvas element
- width, height: numbers
- props: entity props
- args: props.script_args (or props.args)

This allows powerful rendering without hardcoding client ops.

## Client state snapshots

A bounded snapshot event is supported for "state of the world" checks:

- client posts type="client_state" via /api/v1/session/client_event
- agent can request via ui_action type="request_client_state"
- agent waits with client_wait_event(type="client_state", data_match={query_id:...})

Example payload:

{
  "type": "client_state",
  "data": {
    "query_id": "q1",
    "media": [
      {"kind": "video", "path": "out.mp4", "paused": false, "ended": false, "current_time": 1.23, "duration": 10.0}
    ]
  }
}

Clients should keep snapshots small and bounded.

## Deterministic waits (client_wait_*)

Host tools allow the agent to wait for client acks within a single run.
These are cooperative waits (polling the client event log).

Tool: client_wait_event (preferred; ui_wait_event is the deprecated alias)
Arguments:
- type (required)
- timeout_ms (default 30000, max 300000)
- after_unix_ms (optional)
- path (optional convenience filter for data.path)
- data_match (partial object match)
- max_bytes (optional, default 262144)

Return:
- ok=true with the matched event, or ok=false with timeout/cancelled

Join waits:
- client_wait_any (preferred; ui_wait_any is deprecated)
- client_wait_all (preferred; ui_wait_all is deprecated)

## Definition of Done (DoD) handshake

For UI-visible effects, a host tool returning ok:true does not prove the
user/UI observed the result. Use a handshake:

1) agent produces an effect (artifact_register or ui_action)
2) client posts a correlated event (artifact_rendered, ui_action_shown, or client_rpc_result)
3) agent waits once using client_wait_event or client_wait_all

Recommended patterns:
- Artifact: wait for artifact_rendered with tool_call_id
- UI action: wait for ui_action_shown with tool_call_id
- Client RPC: wait for client_rpc_result with rpc_id

## Client profiles (system prompt extensions)

agentd can inject a client profile as an additional system prompt snippet
based on client.kind. This keeps user prompts clean and captures DoD guidance.

Behavior (current):
- When starting a new session and tools=host, agentd inserts a host system
  prompt (unless disabled) and then appends CLIENT_PROFILE=<kind> if available.
- Profiles live in daemon/src/client_profiles.cpp

## HTTP API quick reference (client-facing)

Service / config:
- GET /api/v1/health
- GET /api/v1/ready
- GET /metrics
- GET /api/v1/config
- POST /api/v1/config/update
- POST /api/v1/ota/update
- GET /api/v1/ota/status

Tools and files:
- GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=...
- GET /api/v1/file?path=...&session_id=...
  - If path is relative and session_id is provided, it is resolved under the
    session root; path traversal segments are rejected.
  - When --file-session-realpath-strict is enabled, symlinks that resolve
    outside the session root are rejected (realpath confinement).
  - If path is relative and session_id is omitted, it is resolved under the
    daemon working directory (no host-scope sandboxing).
  - If path is absolute, it is served directly (auth required).

Memory (durable + deterministic):
- POST /api/v1/memory/consolidate
- POST /api/v1/memory/retention/enforce
- GET /api/v1/memory/checkpoints
- GET /api/v1/memory/index
- GET /api/v1/memory/correlate
- GET /api/v1/memory/query

Sessions:
- GET /api/v1/sessions
- POST /api/v1/session/new
- GET /api/v1/session?session_id=...
- DELETE /api/v1/session?session_id=...&broker_token=...   # optional broker_token lets agentd clean up an owned voice broker session during erase
- POST /api/v1/session/upload
- GET /api/v1/session/audit?session_id=...&max_bytes=...
- GET /api/v1/session/artifacts?session_id=...&max_bytes=...&max_artifacts=...
- GET /api/v1/session/client_events?session_id=...
- GET /api/v1/session/scene?session_id=...
- POST /api/v1/session/scene/apply

Client events:
- POST /api/v1/session/client_event
- POST /api/v1/session/ui_event (legacy alias)

Notes:
- All /api/v1/* endpoints (except health/ready/metrics) require auth when enabled.
- The endpoint catalog mirrors daemon/src/agentd_api.cpp.
