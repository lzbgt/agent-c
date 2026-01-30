# UI Actions (Draft)

Date: 2026-01-30

This document defines a **safe, explicit** mechanism for the agent to request **UI-side actions** (agentd → Web UI).

It complements `docs/PROTOCOL.md` and `docs/LIMITS.md`:
- `artifact` events solve *media presentation* (render files like images/audio/video).
- `ui_action` events solve *UI intent* (ask the UI to perform a user-facing action like showing a notification, or preparing
  an audio player with “play N times” controls).

## Goals

- Provide a **typed** UI-control surface that is not regex-based.
- Keep the host tool loop **side-effect free** for UI actions (no shell execution).
- Make UI actions **safe by default**:
  - allowlist of supported action types
  - user consent gates for sensitive actions (especially audio autoplay)
- Make debugging easy by ensuring actions are:
  - emitted as structured `events`
  - mirrored into the SQLite troubleshooting DB (via the existing `events` mirror)

## Non-goals

- Bypassing browser autoplay restrictions.
- A long-term stable public API (rolling project).
- Remote/3rd-party URL opens without explicit operator consent.

## Event: `ui_action`

`ui_action` is a normal event in the `events` array:

- `type`: `"ui_action"`
- `data`: object
  - `step` (number, optional): tool-loop step the action originated from
  - `tool_call_id` (string, optional)
  - `tool_name` (string, optional; usually `"ui_action"`)
  - `action` (object):
    - `type` (string): action type (allowlisted)
    - `title` (string, optional): UI label
    - `message` (string, optional): notification text
    - `path` (string, optional): file path for media actions (resolved via `/api/v1/file`)
    - `mime` (string, optional)
    - `repeat` (int, optional): e.g. play audio N times (clamped by UI)
    - `autoplay` (bool, optional): request autoplay (UI may require user opt-in)

## Host tool: `ui_action` (tools=host)

The agent can request a UI action by calling:

- Tool name: `ui_action`
- Arguments (JSON):
  - `type` (string, required): action type
  - `title` (string, optional)
  - `message` (string, optional)
  - `path` (string, optional)
  - `mime` (string, optional)
  - `repeat` (int, optional)
  - `autoplay` (bool, optional)

Return (tool output string; JSON envelope):
- `ok` (bool)
- `error` (string, optional)
- `data` (object)
  - `tool`: `"ui_action"`
  - `action`: object (normalized action payload echoed back)

## Safety

### Allowlist

The UI must only implement a small allowlist of actions. Unknown `action.type` values are rendered as a safe debug card.

Initial allowlist (v1):
- `notify`: show a UI notification card (no side effects)
- `play_audio`: render an audio player for `path` and optionally attempt autoplay (requires UI opt-in)

### Consent

- `autoplay=true` is only honored when the UI setting “Allow agent-requested audio autoplay” is enabled.
- Even then, browsers may still require a user gesture; UI should fall back to a “Play” button.

## Relationship to loop prevention

One of the reasons models repeat camera/audio captures is that the model cannot reliably infer whether the UI received
an artifact. UI actions help reduce this pressure (the model can request UI playback directly), but **hard stop guards**
(`max_steps`, `max_tool_calls_total`, `tool_call_limits`) are still required because:
- models can ignore instructions
- providers may return tool calls even when the task is conceptually complete
