# Client State Snapshot (Draft)

Date: 2026-01-30

For autonomous decisions, an agent needs access to the **state of the world** it interacts with.
In this project, the “world” includes:
- the agent runtime (tool results, files, logs)
- **collaborating clients** (Web UI, mobile app, Slack bot) which have their own UI state and user interactions

Browsers and chat platforms do not allow a daemon to directly read DOM state. Instead, clients must report state via:
- **events** (append-only facts like “video started playing”)
- optional **snapshots** (current state, e.g. “these media elements exist and are playing”)

This document defines a minimal snapshot event: `client_state`.

## Event: `client_state`

Posted by a client via:
- `POST /api/v1/session/client_event` with `type="client_state"`

Payload shape:

```json
{
  "session_id": "...",
  "type": "client_state",
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": {
    "query_id": "q1",
    "ts_unix_ms": 0,
    "media": [
      {
        "kind": "video",
        "src": "/api/v1/file?...",
        "path": "out.mp4",
        "tool_call_id": "call_abc",
        "paused": false,
        "ended": false,
        "current_time": 1.23,
        "duration": 10.0
      }
    ]
  },
  "append_to_session": false
}
```

Notes:
- `query_id` is optional but recommended when responding to an explicit agent request.
- Clients should keep snapshots small and bounded (limit arrays, truncate large strings).

## Request/response pattern (DoD-friendly)

1) Agent emits `ui_action` with `type="request_client_state"` and a `query_id`.
2) Client responds by posting a `client_state` event with the same `query_id`.
3) Agent waits with `client_wait_event(type="client_state", data_match={query_id:"..."})`.

This makes “probe client state” deterministic for the agent within a single run.

## Complement: media telemetry events

For some decisions, snapshots are too heavy and too stale. Clients should also emit event-style telemetry:
- `video_play_started` / `video_play_paused` / `video_play_finished` / `video_play_failed`
- `audio_play_started` / `audio_play_finished` / `audio_play_failed`

These are high-signal, low-volume “facts” that the agent can wait on deterministically (DoD).
