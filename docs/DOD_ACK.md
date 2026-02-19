# Definition of Done (DoD) for UI-visible Effects

Date: 2026-02-19

This document defines a **fundamental stop condition** for “agent did something for the user” workflows in the Web UI.

The core issue:
- A host tool returning `ok:true` proves the host-side action succeeded (file created, tool ran).
- It does **not** prove the user/UI actually **received/observed** the effect (rendered in browser, played, acknowledged).

Therefore, for UI-visible effects (artifacts, notifications, playback), a reliable DoD requires a **handshake**:

1) Agent produces an effect (artifact registered / UI action requested).
2) Client confirms it actually rendered/performed it (client event).
3) Agent waits for that confirmation once (no retry loop).

This is complementary to hard bounds (`max_steps`, tool call limits). Bounds are guardrails; the handshake is the root-cause fix.

## Goals

- Make “done” observable for UI-visible effects so the agent stops repeating work.
- Support deterministic waiting for:
  - one acknowledgement (`ui_wait_event`)
  - multiple acknowledgements (“join”) (`ui_wait_any`, `ui_wait_all`)
- Keep behavior robust when DB is disabled (file-backed client events are canonical).
- Keep everything bounded (time, bytes, recursion depth).

## Scope notes

This document is narrowly about a **deterministic stop condition** (“did the UI/client actually observe the effect?”).

This project explicitly **demands power capabilities** (automation, entity creation/editing/actions, DOM mutation, media control).
The DoD handshake is what makes those capabilities usable in practice: powerful systems that cannot tell when they are done will
retry forever and appear “broken”.

This document does not try to “bypass” browser permission policies (e.g. autoplay gesture requirements). Instead, it specifies the
handshake so agents can act powerfully *and still stop* based on observable facts.

## Protocol primitives

### Effect emission (agent → UI)

- `artifact_register` host tool → derived `artifact` event
- `ui_action` host tool → derived `ui_action` event

### Effect acknowledgement (UI → agentd)

The client posts client events via:
- `POST /api/v1/session/client_event` (preferred; `ui_event` is a legacy alias)

Canonical storage:
- Legacy (file-backed): `<sessions_root>/<session_id>.client_events.jsonl` (current canonical store is the DB)

Recommended event types:
- `artifact_rendered`: UI mounted the artifact renderer for a specific artifact/tool call.
  - payload: `{ path, kind, title?, tool_call_id? }`
- `ui_action_shown`: UI rendered a UI-action card.
  - payload: `{ action_type, title?, tool_call_id? }`
- `notification_ack`: user explicitly acknowledged a notification (manual DoD).

### Waiting / joining acks (agent tool)

Agents should use host tools (session-scoped):
- `ui_wait_event`: wait for one matching event
- `ui_wait_any`: wait until any predicate matches (OR join)
- `ui_wait_all`: wait until all predicates match (AND join)

## Recommended DoD patterns

### “Capture screenshot and present to UI”

1) Produce image file
2) `artifact_register(path=..., kind=image, title=...)`
3) Wait once:
   - `ui_wait_event(type="artifact_rendered", data_match={ tool_call_id: "<tool_call_id>" })`
4) Respond with a short “done” assistant message and stop

### “Present an artifact and stop (generic)”

1) Produce a host file (any format) under the daemon tools root
2) `artifact_register(path=..., kind=..., title=...)`
3) Wait once:
   - `client_wait_event(type="artifact_rendered", data_match={ tool_call_id: "<tool_call_id>" })`

If the task requires active client-side interaction (e.g. playback, DOM edits), request it as a `client_rpc`
(`dom_apply` / `page_eval` / `script_eval` / `media_play`) and wait for `client_rpc_result` instead of assuming it happened.

### Join multiple UI acknowledgements

Example: show notification + play audio, continue after both:
- `ui_wait_all(predicates=[ notification_ack(tool_call_id=...), client_rpc_result(rpc_id=...) ])`

### “Wait for a client-side condition (generic)”

When an agent must decide based on client state (DOM/media/location), use a client RPC:

1) Request RPC:
   - `ui_action(type="client_rpc", rpc_id="<tool_call_id>", rpc={kind:"media_snapshot", args:{...}}, auto_run=true)`
2) Wait for the result:
   - `client_wait_event(type="client_rpc_result", data_match={rpc_id:"<tool_call_id>"})`

For long-running conditions (like “wait until media ended”), prefer observation:
- `ui_action(type="client_rpc", rpc_id="<id>", rpc={kind:"media_observe", side_effects:true, args:{tool_call_id:"<call>"}}, auto_run=true)`
- `client_wait_event(type="client_rpc_progress", data_match={rpc_id:"<id>", name:"ended"})`
- `ui_action(type="client_rpc", rpc_id="<id>", rpc={kind:"media_unobserve", args:{rpc_id:"<id>"}}, auto_run=true)` to detach observers

### “Do side-effecting client automation and stop”

When an agent uses the collaboration surface to cause side effects (DOM edits, entity operations, playback, navigation), the DoD is
still a handshake:

1) Request the side effect as a client RPC:
   - `ui_action(type="client_rpc", rpc_id="<tool_call_id>", rpc={kind:"dom_apply"|"...", side_effects:true, args:{...}}, auto_run=true)`
2) Wait once for the correlated outcome:
   - `client_wait_event(type="client_rpc_result", data_match={rpc_id:"<tool_call_id>", ok:true})`

If the action is long-running, prefer progress events and wait for a phase:
- `client_wait_event(type="client_rpc_progress", data_match={rpc_id:"<tool_call_id>", name:"finished"})`

## Future: concurrent jobs (design sketch)

The daemon already supports concurrent **runs** via `run_async` + job manager.

What we do not yet support is concurrent host tool tasks *within a single tool-loop decision* (e.g. “build + tests in parallel”).

A future-safe design would introduce “tool jobs”:
- `tool_job_start(tool_name, args)` → returns `{ job_handle }`
- `tool_job_poll(job_handle)` → returns `{ status, partial_output? }`
- `tool_job_wait(job_handle, timeout_ms)` → returns `{ final_output }`
- cancellation wired to daemon job cancellation

This enables the agent to reason about multiple ongoing tasks (peek status) and join them deterministically, but is intentionally
deferred until the tool/job API surface is stabilized.
