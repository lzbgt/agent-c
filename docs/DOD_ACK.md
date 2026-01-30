# Definition of Done (DoD) for UI-visible Effects (Draft)

Date: 2026-01-30

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

## Non-goals (for now)

- True concurrent tool execution within a single tool-loop step.
- Complex boolean logic DSL for matching.
- Browser autoplay bypass (user gesture policies still apply).

## Protocol primitives

### Effect emission (agent → UI)

- `artifact_register` host tool → derived `artifact` event
- `ui_action` host tool → derived `ui_action` event
- `camera_capture` host tool → derived `artifact` and/or `ui_action`

### Effect acknowledgement (UI → agentd)

The client posts client events via:
- `POST /api/v1/session/client_event` (preferred; `ui_event` is a legacy alias)

Canonical storage:
- `<sessions_root>/<session_id>.client_events.jsonl`

Recommended event types:
- `artifact_rendered`: UI mounted the artifact renderer for a specific artifact/tool call.
  - payload: `{ path, kind, title?, tool_call_id? }`
- `ui_action_shown`: UI rendered a UI-action card.
  - payload: `{ action_type, title?, tool_call_id? }`
- `audio_play_started` / `audio_play_finished` / `audio_play_failed`: already supported for audio playback.
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

### “Play audio and continue after finished”

1) `artifact_register(kind=audio, autoplay=true, repeat=N)` or `ui_action(type=play_audio, ...)`
2) Wait:
   - `ui_wait_event(type="audio_play_finished", data_match={ tool_call_id: "<tool_call_id>" })`

### Join multiple UI acknowledgements

Example: show notification + play audio, continue after both:
- `ui_wait_all(predicates=[ notification_ack(tool_call_id=...), audio_play_finished(tool_call_id=...) ])`

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
