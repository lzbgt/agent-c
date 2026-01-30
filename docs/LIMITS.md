# Run Limits & Runaway-Loop Guards (Draft)

Date: 2026-01-30

This document specifies how the agent tool loop should behave when configured safety limits are reached.

It exists because:
- In tool mode, the model may keep producing tool calls indefinitely (e.g. repeated camera captures).
- Without explicit limits, the daemon can appear “stuck” and can spam host resources (camera, disk, CPU).
- Limits must fail **loudly** (structured error event + non-OK status) so UIs/operators can diagnose the stop reason.

## Goals

- Provide a bounded, operator-configurable safety envelope for tool loops.
- Ensure hitting a limit:
  - returns a non-OK status (`AGENT_ERR_LIMIT`)
  - includes a human-readable error message
  - emits a structured `error` event with the stop reason and relevant counters
- Preserve explicit opt-out for advanced users (e.g. `max_steps=0` means unlimited).

## Non-goals

- Perfect “task complete” inference. Limits are guardrails, not semantic understanding.
- Provider-specific policy (those belong in hosts or UIs).

## Core semantics

### `max_steps`

- Definition: maximum number of tool-loop *steps* (provider calls), where each step can contain 0+ tool calls.
- `max_steps = 0` means unlimited.
- If the loop would continue producing tool calls beyond the limit, the core must:
  - emit an `error` event with:
    - `reason = "max_steps_exceeded"`
    - `steps_executed`
    - `max_steps`
  - set error message like: `"max steps exceeded"`
  - return `AGENT_ERR_LIMIT`

### `max_repeated_tool_calls`

- Definition: abort if the exact same tool call (same tool name + exact `arguments_json`) repeats too many times consecutively.
- `max_repeated_tool_calls = 0` disables the guard.
- When triggered, the core must:
  - emit an `error` event with:
    - `reason = "repeated_tool_call_guard"`
    - `tool_name`
    - `repeats`
    - `max_repeats`
  - return `AGENT_ERR_LIMIT`

## Host/daemon defaults

### Daemon default `max_steps`

To protect long-running `agentd` instances, the daemon should apply a default `max_steps` for requests that omit it.

- Proposed default: `32` steps.
- The UI should allow:
  - blank / unset `max_steps` → daemon default applies
  - explicit `0` → unlimited (operator accepts risk)

This keeps new users safe while still allowing intentional long runs.

## Related docs

- `docs/PROTOCOL.md` (run request fields)
- `README.md` (operator-facing summary)
