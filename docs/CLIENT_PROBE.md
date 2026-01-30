# Client Probe RPC (Draft)

Date: 2026-01-30

Agents need **world state** to make autonomous decisions. For remote clients (browser, mobile app, Slack),
the daemon cannot directly inspect the client runtime (DOM, playback, UI tree) due to sandbox/security boundaries.

The fundamental solution is a **client probe RPC**:

1) Agent requests a bounded, read-only probe via `ui_action` (`type="client_probe"`).
2) Client executes an allowlisted probe implementation locally (DOM/media/UI introspection).
3) Client posts the result back as a `client_event` (`type="client_probe_result"`).
4) Agent waits deterministically using `client_wait_event` / `client_wait_any` / `client_wait_all`.

This avoids brittle hard-coded state transitions while remaining safe and extensible across many client types.

## Threat model / safety

This protocol is intentionally **not** “run arbitrary JS from the agent”.

Constraints:
- Probes are **read-only** (no DOM mutation, no clicks, no navigation, no network fetches).
- Probes are **bounded** (limits on elements, string lengths, total output size).
- Clients implement an **allowlist** of probe kinds; unknown kinds are rejected safely.
- Clients may require explicit user opt-in (“Allow agent probes”) before executing probes automatically.
- Clients should apply **defense-in-depth redaction** for obviously sensitive fields when practical (for example,
  DOM input values for password fields, and attribute/dataset keys that look like secrets/tokens).

## Agent → client request (via ui_action)

Agent emits an action:

```json
{
  "type": "client_probe",
  "title": "Check whether video is playing",
  "probe_id": "p123",
  "probe": {
    "kind": "dom_query",
    "selector": "video",
    "limit": 5,
    "fields": ["tag", "dataset", "currentSrc", "paused", "ended", "currentTime", "duration"]
  }
}
```

Fields:
- `probe_id` (string, required): stable id for correlation. Recommended: use the tool_call_id.
- `probe.kind` (string, required): allowlisted probe kind.
- `probe.*`: kind-specific arguments.

### Consent / execution model

Clients may support two execution modes:

- **Manual** (default): render the probe request in the UI (a “Run probe” button) and only execute after a user action.
- **Auto-run** (opt-in): if the user has explicitly enabled “Allow agent-requested client probes”, the client may execute
  immediately when the request includes `auto_run=true` (or `auto=true`).

Agents must treat probes as **best-effort**:
- If a probe result does not arrive before timeout, do not blindly retry in a loop.
- Instead, ask the user to enable probes (or use a different DoD handshake such as `artifact_rendered` / `ui_action_shown`).

## Client → agent response (client_event)

Client posts:

```json
{
  "session_id": "...",
  "type": "client_probe_result",
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": {
    "probe_id": "p123",
    "request_tool_call_id": "call_abc",
    "ok": true,
    "result": { "items": [ ... ] }
  },
  "append_to_session": false
}
```

## Waiting / joining

Agent waits:

- single:
  - `client_wait_event(type="client_probe_result", data_match={probe_id:"p123"})`
- join:
  - `client_wait_all(predicates=[{type:"client_probe_result",data_match:{probe_id:"p1"}}, ...])`

## Capabilities discovery

Clients may periodically announce supported probes:

`type="client_capabilities"` with `data.probes=[{kind, schema?}]`

Agents can also inspect the last capability event via `client_peek(event_type="client_capabilities")`.

## Recommended probe kinds (v1)

This repo currently uses a small initial allowlist (browser/Web UI client):

- `location`
  - Intended for lightweight context (URL/title). Clients should consider redacting query parameters that look sensitive.
- `media_snapshot`
  - Intended for “is audio/video playing?” checks without hardcoding action-specific events.
- `dom_query`
  - Intended for bounded DOM inspection (selector + allowlisted fields).
  - Clients should treat password field values and key-like attributes/datasets as sensitive and redact them.
