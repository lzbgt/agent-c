# Client RPC (Collaboration Surface) — Draft

Date: 2026-01-30

Agents and humans collaborate through a **client surface** (Web UI, mobile app, Slack bot, etc.).
The client is not the human; it is the **shared stateful environment** where:
- the agent can present meaning (artifacts, UI actions)
- the human can respond (acknowledgements, choices)
- the agent can **observe** and (optionally) **act** to advance tasks

Because the daemon cannot directly inspect or mutate a remote client runtime (DOM, playback, UI tree),
we need a generic RPC channel:

1) Agent requests a client RPC via `ui_action` (`type="client_rpc"`).
2) Client executes an allowlisted handler locally (may be read-only or side-effecting).
3) Client posts back one or more correlated events (`client_rpc_result` and optional `client_rpc_progress`).
4) Agent waits deterministically using `client_wait_event` / `client_wait_any` / `client_wait_all`.

This replaces brittle “hardcoded state” patterns with a universal request/response surface that supports:
- introspection (queries/snapshots)
- controlled side effects (when explicitly enabled; “client YOLO”)
- generic completion criteria (DoD) via correlated result/progress events

## Goals

- Provide a universal agent ↔ client collaboration protocol usable across client types.
- Support both:
  - **read-only** introspection (safe default)
  - **side-effecting** actions (explicitly enabled; local operator consent)
- Make “done” observable:
  - correlate requests with `rpc_id`
  - emit deterministic result/progress events the agent can wait/join on
- Keep implementations SOLID:
  - allowlist of rpc kinds per client
  - bounded inputs/outputs
  - explicit consent gates for side effects

## Non-goals (for now)

- “Run arbitrary code” in the client runtime (too dangerous, too easy to exfiltrate secrets or infinite-loop).
- Full browser automation (navigation/click-recording) as a single generic primitive.
- Guaranteed autoplay / bypassing browser permission policies.

## Safety model (critical)

This protocol intentionally separates:

- **Capability**: client advertises supported rpc kinds (what it can do).
- **Consent**: user/client config decides what may execute automatically.
- **Request**: agent asks for a specific rpc kind + bounded args.

Clients should implement:
- strict allowlists for rpc kinds
- strict bounds (element limits, string length limits, total output size)
- redaction for obviously sensitive fields when returning results

Side effects must be opt-in:
- a client may support “Allow agent client RPCs (read-only)”
- and a separate “Allow agent client RPCs with side effects (YOLO)”

## Agent → client request (via `ui_action`)

Agent emits:

```json
{
  "type": "client_rpc",
  "title": "Observe the video element and wait for it to end",
  "rpc_id": "rpc_123",
  "rpc": {
    "kind": "media_observe",
    "side_effects": true,
    "args": {
      "tool_call_id": "call_abc",
      "events": ["play", "pause", "ended", "error"]
    }
  },
  "auto_run": true
}
```

Fields:
- `rpc_id` (string, required): correlation id for deterministic matching.
  - recommended: set equal to the originating tool call id
- `rpc.kind` (string, required): allowlisted rpc kind for this client
- `rpc.side_effects` (bool, optional): whether this RPC is expected to cause side effects
  - clients must treat this as advisory; actual permission is client-controlled
- `rpc.args` (object, optional): kind-specific bounded arguments
- `auto_run` (bool, optional): request immediate execution when permitted by client settings

## Client → agent response (client_event)

### Event: `client_rpc_result` (final)

Client posts:

```json
{
  "session_id": "...",
  "type": "client_rpc_result",
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": {
    "rpc_id": "rpc_123",
    "request_tool_call_id": "call_abc",
    "rpc_kind": "media_observe",
    "ok": true,
    "elapsed_ms": 12,
    "result": { "observing": 1 }
  },
  "append_to_session": false
}
```

If `ok=false`, include `error` (string) instead of `result`.

### Event: `client_rpc_progress` (optional; streaming facts)

Some RPCs are long-running (e.g. observation/subscriptions). Clients may emit progress events:

```json
{
  "session_id": "...",
  "type": "client_rpc_progress",
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": {
    "rpc_id": "rpc_123",
    "rpc_kind": "media_observe",
    "name": "ended",
    "ts_unix_ms": 0,
    "payload": { "current_time": 9.9 }
  },
  "append_to_session": false
}
```

Agents can deterministically wait for any phase:
- `client_wait_event(type="client_rpc_result", data_match={rpc_id:"rpc_123"})`
- `client_wait_event(type="client_rpc_progress", data_match={rpc_id:"rpc_123", name:"ended"})`

## Capabilities discovery

Clients may periodically announce supported RPC kinds:

- `type="client_capabilities"`
- `data.rpcs=[{kind, side_effects, description?}]` (bounded list)

Agents can inspect the latest capabilities via `client_peek(event_type="client_capabilities")`.

## Recommended rpc kinds (v1)

Clients should start with a small allowlist and grow it intentionally.

This repo’s Web UI client currently supports (initial set):

Read-only:
- `location`: bounded URL/title snapshot (query params redacted when they look sensitive)
- `media_snapshot`: bounded audio/video snapshot (paused/currentTime/duration)
- `dom_query`: bounded DOM query (selector + allowlisted fields)
- `state_snapshot`: convenience combo of `location` + `media_snapshot`

Side-effecting (requires explicit client “side effects” enablement):
- `dom_click`: click by selector
- `dom_set_value`: set input/textarea value by selector (does not echo value back)
- `media_play`: attempt `HTMLMediaElement.play()` by selector (browser policies apply)
- `media_observe`: attach media listeners and emit `client_rpc_progress` events correlated by `rpc_id`

## Relationship to DoD (stop conditions)

This is the root-cause fix for “agent keeps repeating the same UI-visible action”.
The agent should:

1) request an effect or observation (artifact/ui_action/client_rpc)
2) wait once for a correlated acknowledgement/result
3) stop (or move to the next step), rather than retrying in a blind loop
