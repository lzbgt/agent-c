# Policy Hooks v0 (agentd)

Date: 2026-02-19
Status: v0 (implemented; rolling)

This spec defines the **policy hook surface** used by `agentd` to enforce deterministic
guardrails for tool-enabled runs. It focuses on **tool allow/deny** and **budget caps**
and emits audit events (`policy_decision`) for transparency.

---

## Goals

- Deterministic policy decisions for tool calls.
- Minimal, explicit config surface (daemon flags + runtime config).
- Audit events suitable for UI + postmortem reasoning.

## Non-goals (v0)

- Policy scripting/VM execution.
- Per-step or content-based dynamic policies.
- Granular role-based multi-tenant policy overlays.

---

## Policy modes

`policy_mode` governs enforcement:

- **off**: policy hooks are disabled.
- **audit**: emit `policy_decision` events **without** enforcing allow/deny or caps.
- **enforce**: deny disallowed tools and clamp budgets.

---

## Config surface

### Daemon flags

- `--policy-mode off|audit|enforce`
- `--policy-tool-allow <csv>` (tool names; comma-separated; repeatable)
- `--policy-tool-deny <csv>` (tool names; comma-separated; repeatable)
- `--policy-max-steps <n>`
- `--policy-max-tool-calls-total <n>`
- `--policy-max-tool-calls-per-tool <n>`
- `--policy-max-tool-call-args-chars <n>`
- `--policy-max-tool-result-chars <n>`

### Environment variables

- `AGENTD_POLICY_MODE`
- `AGENTD_POLICY_TOOL_ALLOWLIST`
- `AGENTD_POLICY_TOOL_DENYLIST`
- `AGENTD_POLICY_MAX_STEPS`
- `AGENTD_POLICY_MAX_TOOL_CALLS_TOTAL`
- `AGENTD_POLICY_MAX_TOOL_CALLS_PER_TOOL`
- `AGENTD_POLICY_MAX_TOOL_CALL_ARGS_CHARS`
- `AGENTD_POLICY_MAX_TOOL_RESULT_CHARS`

### Runtime config (`/api/v1/config`)

```json
{
  "policy": {
    "mode": "enforce",
    "tool_allowlist": ["memory_write", "text_search"],
    "tool_denylist": ["shell_exec"],
    "max_steps": 32,
    "max_tool_calls_total": 64,
    "max_tool_calls_per_tool": 8,
    "max_tool_call_args_chars": 4000,
    "max_tool_result_chars": 8000
  }
}
```

Notes:
- Tool names are **exact matches**.
- `0` means “unlimited / disabled”.

---

## Enforcement semantics

### Allow/deny

Decision logic (tool name `T`):

1. If `T` is in `tool_denylist`, deny.
2. Else if `tool_allowlist` is non-empty and `T` is **not** present, deny.
3. Else allow.

`audit` mode **does not** block the tool call; it only emits a `policy_decision` event.

### Budget caps

Policy caps clamp the following run limits in **enforce** mode:

- `max_steps`
- `max_tool_calls_total`
- `max_tool_calls_per_tool`
- `max_tool_call_args_chars`
- `max_tool_result_chars`

In **audit** mode, caps are **reported** as `policy_decision` events but the limits are not changed.

---

## Events

Policy hooks emit `policy_decision` events (schema: `run_event_payload_policy_decision_v1`).

Common fields:

- `phase`: `pre_run` | `tool_call` | `post_run`
- `mode`: `off` | `audit` | `enforce`
- `action`: `start` | `cap` | `allow` | `deny` | `complete`
- `enforced`: boolean (for `cap` or `deny`)

Examples:

```json
{
  "type": "policy_decision",
  "schema": "run_event_payload_policy_decision_v1",
  "data": {
    "phase": "pre_run",
    "mode": "enforce",
    "action": "cap",
    "field": "max_steps",
    "requested": 100,
    "effective": 32,
    "enforced": true
  }
}
```

```json
{
  "type": "policy_decision",
  "schema": "run_event_payload_policy_decision_v1",
  "data": {
    "phase": "tool_call",
    "mode": "audit",
    "action": "deny",
    "reason": "tool_denylist",
    "tool_name": "shell_exec",
    "tool_call_id": "call_abc",
    "enforced": false
  }
}
```

```json
{
  "type": "policy_decision",
  "schema": "run_event_payload_policy_decision_v1",
  "data": {
    "phase": "post_run",
    "mode": "enforce",
    "action": "complete",
    "ok": true
  }
}
```

`policy_decision` events appear in both synchronous run responses and async job streams.

---

## Implementation notes

- Policy events are appended after run completion for sync responses.
- Async job streams receive policy events as they occur (best-effort).
- Policy caps are applied before the tool loop begins (deterministic).
