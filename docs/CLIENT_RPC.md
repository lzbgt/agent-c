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

## Entities (client-agnostic collaboration surface)

DOM is Web-specific. The client-agnostic abstraction is **entities**:
- query / create / update / action / delete

See: `docs/CLIENT_ENTITIES.md`.

## Web UI: Canvas2D scripts (power mode)

For the Web UI client (`client.kind="webui"`), the recommended “no hardcoded ops” approach for drawing is:

- Create a `canvas2d` entity via `entity_apply` with explicit `props.width`/`props.height`
- Provide arbitrary drawing logic as `props.script` (JavaScript)

The script is executed in the browser with:

- `ctx`: a 2D canvas rendering context
- `canvas`: the canvas element
- `width`, `height`: canvas dimensions (numbers)
- `props`: the entity props object
- `args`: `props.script_args` (or `props.args`)

This keeps user prompts succinct (“draw a sine plot”), while the client profile injected by `agentd` provides the operational defaults.
See `docs/CLIENT_PROFILES.md`.

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

## Power features (staged, but MUST-have)

This project is explicitly about agents and humans collaborating through a client surface.
That implies the system must support progressively more powerful capabilities over time:

1) **Scriptable probing (remote “probe code”)**
   - The agent can send runnable code that performs *task-specific* inspection and reasoning, rather than relying on fixed snapshots.
   - This is the general solution to “the agent should probe only what it cares about”.

2) **Scriptable side effects (client YOLO)**
   - In “client YOLO” mode, the agent can cause client-side changes (clicks, typing, observers/subscriptions, navigation).
   - This is core to real autonomy: the agent must be able to act on the collaboration surface.

3) **Full browser automation**
   - Long-term, higher-level automation (navigation flows, record/replay) should be expressible either as:
     - a set of primitives (click/type/navigate/wait/assert), and/or
     - a script engine that composes primitives into workflows.

4) **Media playback policies (autoplay)**
   - Browsers enforce user-gesture policies. A client can:
     - attempt autoplay
     - report whether it succeeded
     - fall back to a deterministic “gesture handshake” (ask user to click play, then ack).
   - “Guaranteed autoplay bypass” is not generally achievable in standards-compliant browsers, but deterministic UX is achievable.

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

Important engineering reality:
- Some classes of “arbitrary code” are not safely preemptible in the browser main thread (an infinite loop can block timers).
- Therefore, **killability** matters:
  - prefer script engines that can be terminated (e.g. Web Worker execution) and that interact with DOM via an API bridge.
  - for truly unsafe “page eval”, require explicit user enablement and accept that the page may need a reload as the kill switch.

## Agent → client request (via `ui_action`)

Agent emits a **tool call** to `ui_action` whose arguments JSON contains a structured action payload.

Canonical arguments shape (exact field names):

```json
{
  "type": "client_rpc",
  "title": "Optional short title for UI",
  "rpc_id": "rpc_123",
  "rpc": {
    "kind": "dom_query",
    "args": { "selector": "#app", "fields": ["tag", "id"] }
  },
  "auto_run": true,
  "side_effects": false
}
```

Notes:
- `type` must be `"client_rpc"` for this collaboration protocol.
- `rpc_id` is the deterministic correlation key. Recommended: set it equal to the originating `tool_call_id`.
- `rpc.kind` is the allowlisted RPC kind for the client.
- `rpc.args` is a kind-specific object (bounded).
- `auto_run` requests that the client executes immediately when settings allow.
- `side_effects` is advisory; clients can also infer side-effect expectations by `rpc.kind`.

Equivalent legacy shapes are still accepted by the Web UI (for backwards compatibility):
- `rpc_id` may be omitted if `tool_call_id` exists (client will correlate by `tool_call_id`)
- `rpc.args` may be omitted and the client will treat `rpc` as the args object
- `side_effects` may also be provided as `rpc.side_effects`

Example:

```json
{
  "type": "client_rpc",
  "title": "Observe the video element and wait for it to end",
  "rpc_id": "rpc_123",
  "rpc": {
    "kind": "media_observe",
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
- `artifact_url`: resolve an artifact path to a **browser-usable URL** (typically a `blob:` URL) so the agent can embed it via DOM ops
- `state_snapshot`: convenience combo of `location` + `media_snapshot`

Side-effecting (requires explicit client “side effects” enablement):
- `dom_click`: click by selector
- `dom_set_value`: set input/textarea value by selector (does not echo value back)
- `dom_apply`: apply a DOM patch (create/edit/delete/dispatch). This is the “native surface” primitive for collaboration UI changes.
- `media_play`: attempt to play media (browser policies apply)
- `media_observe`: attach media listeners and emit `client_rpc_progress` events correlated by `rpc_id`
  - WebUI bounds observers to avoid memory leaks; idle observers are auto-evicted after ~15 minutes or when exceeding ~32 active.
- `media_unobserve`: detach listeners created by `media_observe` (by `rpc_id` or `all:true`)
- `navigate`: navigate to a same-origin URL (likely reloads the client)
- `open_url`: open an external URL in a new tab (requires explicit user confirmation)

Scriptable (must-have power primitive):
- `script_eval`: run agent-provided script code (prefer killable engines like Web Workers) and expose DOM/media/location via an API bridge.
  - scripts can probe only what they care about, and can implement their own “wait/act” logic using client RPC progress events.

Unsafe (main-thread; optional but powerful):
- `page_eval`: run agent-provided JS on the browser main thread with access to the same API bridge.
  - Not safely preemptible if the script blocks the event loop; treat “reload the page” as the kill switch.

### Full-power DOM note (Web UI)

If you need **unbounded DOM power** (create arbitrary elements, attach handlers, query state, do complex presentation),
use `page_eval`. This is the generic solution for **interactive media presentation tasks** where “download/open link” is not sufficient:
- It runs in the browser main thread.
- It can access `window` / `document` directly (it is not limited to the API bridge).
- Use it when `dom_apply` is too limiting.

Because main-thread JS cannot be safely preempted, keep code short and use async/await (avoid infinite loops).

### `dom_apply` schema (v1, Web UI client)

`rpc.kind="dom_apply"` with:

```json
{
  "ops": [
    {"op":"create","tag":"div","parent_selector":"#root","attrs":{"data-x":"1"},"text":"hello"},
    {"op":"set_attr","selector":"#root","name":"data-ready","value":"1"},
    {"op":"append_html","selector":"#root","html":"<button id=\\"b\\">Click</button>"},
    {"op":"dispatch","selector":"#b","event":"click"}
  ]
}
```

Supported ops (bounded):
- `create`: `{tag, parent_selector?, insert?, attrs?, text?, html?}`
- `remove`: `{selector, limit?}`
- `set_attr`: `{selector, name, value, limit?}`
- `remove_attr`: `{selector, name, limit?}`
- `set_text`: `{selector, text, limit?}`
- `set_html` / `append_html`: `{selector, html, limit?}`
- `dispatch`: `{selector, event, event_init?, limit?}`

Return value includes `{applied,total,ops:[...]}` with per-op success/errors.

### `media_play` schema (v1, Web UI client)

`rpc.kind="media_play"` is side-effecting. It supports:

1) **Play an existing element** (recommended):

```json
{ "selector": "#player" }
```

2) **Convenience: create/reuse an element and play a URL or artifact path**:

```json
{ "id": "player", "url": "blob:..." }
```

or (artifact paths; the client resolves to a `blob:` URL internally, including daemon auth headers when needed):

```json
{ "id": "player", "path": "out/hello.mp3", "resolved_path": "/abs/.../out/hello.mp3", "yolo": true }
```

Notes:
- Autoplay may be blocked without a user gesture; the result will include an error string in that case.
- Prefer explicitly creating a UI player (controls) via `page_eval`/`dom_apply` and using `media_observe` to verify a `play` event.

### Media presentation note

This repo intentionally does **not** expose a dedicated `artifact_play` RPC in the Web UI client.
Instead, agents should:
- register files via `artifact_register` and rely on `/api/v1/file` for download/preview, and/or
- use `dom_apply` / `page_eval` / `script_eval` to create whatever UI elements are appropriate for the client surface.

### `artifact_url` schema (v1, Web UI client)

`rpc.kind="artifact_url"` with:

```json
{
  "path": "out/hello.wav",
  "resolved_path": "/abs/path/to/out/hello.wav",
  "yolo": true
}
```

Behavior:
- The client fetches bytes from `GET /api/v1/file?path=...&yolo=...` (including `Authorization` if the daemon requires it),
  then returns a `blob:` URL that can be used in DOM (`<audio src>`, `<video src>`, `<img src>`, download links, etc.).
- In YOLO mode, the client may fall back from `path` to `resolved_path` (absolute) if the daemon tools root differs from the run tools root.

Return example:

```json
{
  "kind": "artifact_url",
  "ok": true,
  "url": "blob:https://.../....",
  "source_path": "out/hello.wav",
  "content_type": "audio/wav",
  "size_bytes": 12345
}
```

### `script_eval` schema (killable worker; MUST-have)

`rpc.kind="script_eval"` runs agent-provided code in a **killable Web Worker** with an API bridge:

```json
{
  "code": "await api.progress('begin'); const s = await api.location.get(); return { href: s.href };",
  "args": { "note": "optional user args" },
  "timeout_ms": 8000
}
```

Notes:
- The worker is terminated on timeout or completion.
- Side-effecting bridge methods are rejected unless client “side effects” is enabled.

### `page_eval` schema (unsafe main-thread; power option)

`rpc.kind="page_eval"` runs agent-provided JS on the browser main thread with the same API bridge shape.
This is intentionally powerful and should be used for things a worker cannot do directly.

```json
{
  "code": "await api.dom.apply({ ops: [{ op:'set_attr', selector:'body', name:'data-agent', value:'1' }] }); return 'ok';",
  "args": {},
  "timeout_ms": 8000
}
```

Important:
- It is **not** safely preemptible if the script blocks the event loop.
- Treat “reload the page” as the kill switch.

## Relationship to DoD (stop conditions)

This is the root-cause fix for “agent keeps repeating the same UI-visible action”.
The agent should:

1) request an effect or observation (artifact/ui_action/client_rpc)
2) wait once for a correlated acknowledgement/result
3) stop (or move to the next step), rather than retrying in a blind loop
