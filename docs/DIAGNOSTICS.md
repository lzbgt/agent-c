# Diagnostics Endpoints (Draft)

Date: 2026-02-13

This document describes the lightweight diagnostics endpoints exposed by `agentd`.

Goals:
- Give operators a **fast health snapshot** beyond `/api/v1/health`.
- Surface provider key presence **without leaking secrets**.
- Provide a small provider smoke test endpoint for quick verification.

Additional goals:
- Deliver full observability by unifying traces, audit logs, and DB query endpoints.
- Provide long-running benchmark and soak runs via dedicated load-test harnesses.

## Auth

All diagnostics endpoints require Bearer auth when `agentd` is started with `--auth-token`.

## Endpoint: `/api/v1/diagnostics`

Returns a compact snapshot with:
- readiness (`ready`)
- DB metadata (`db.path`, `db.size_bytes`)
- DB table counts (`db.tables.*`)
- job status counts (`jobs.by_status`)
- workflow scheduler stats (`workflows.*`)

Example:

```json
{
  "ok": true,
  "service": "agentd",
  "version": "0.1",
  "now_unix_ms": 1739490000000,
  "uptime_ms": 123456,
  "ready": true,
  "checks": { "db_open": true },
  "db": {
    "path": "/path/to/agentd.db",
    "size_bytes": 1234567,
    "tables": {
      "sessions": 12,
      "messages": 345,
      "runs": 78,
      "events": 910,
      "artifacts": 5,
      "ui_actions": 2,
      "client_events": 0,
      "audit_records": 80,
      "jobs": 3,
      "workflows": 7,
      "workflow_tasks": 22,
      "workflow_events": 41,
      "edge_nodes": 0,
      "edge_tasks": 0,
      "edge_workflows": 0
    }
  },
  "jobs": {
    "total": 3,
    "by_status": { "queued": 1, "running": 0, "done": 2 }
  },
  "workflows": {
    "workflows_by_status": { "queued": 1, "running": 0, "done": 6 },
    "tasks_by_status": { "queued": 2, "running": 0, "done": 20 },
    "tasks_queued_ready": 1,
    "tasks_queued_not_ready": 1
  }
}
```

If any counters fail to load, the response includes a `warnings[]` array.

## Endpoint: `/api/v1/diagnostics/providers`

Returns **provider key presence** and best-effort source (config/env/file), plus base URL and model defaults.

Example:

```json
{
  "ok": true,
  "service": "agentd",
  "version": "0.1",
  "now_unix_ms": 1739490000000,
  "uptime_ms": 123456,
  "providers": {
    "deepseek": {
      "key_present": true,
      "source": { "kind": "env", "label": "DEEPSEEK_API_KEY" },
      "base_url": "https://api.deepseek.com",
      "model_default": "deepseek-reasoner"
    },
    "moonshot": {
      "key_present": true,
      "source": { "kind": "file", "label": "~/.env" },
      "base_url": "https://api.moonshot.cn/v1",
      "model_default": "kimi-k2.5"
    }
  }
}
```

Notes:
- `key_present` is **boolean only**. Secret values are never returned.
- `source.kind` is one of: `config`, `env`, or `file`.
- `source.label` is a best-effort descriptor (e.g., `provider_keys`, `api_key`, `.not_in_repo`, `project.local.md`, `~/.env`).

## Endpoint: `/api/v1/diagnostics/provider_test`

Runs a small provider test without creating a session.

Request fields:
- `provider` (required): `deepseek`, `moonshot`, `openrouter`, `openai`
- `base_url` (optional): override provider base URL
- `model` (optional): override model
- `prompt` / `expect` (optional): basic expectation matcher (`assistant_text` must match `expect`)
- `tools` (optional): `none|basic|host`
- `require_tool_call` (optional): require at least one tool call
- `timeout_ms` (optional)
- Tool-loop limits (optional): `max_steps`, `max_tool_calls_total`, `max_tool_calls_per_tool`,
  `max_tool_call_args_chars`, `max_repeated_tool_calls`
- `include_run` (optional): echo the full run response in `run`

Example (DeepSeek reasoner + tool call):

```bash
curl -sS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{
    "provider": "deepseek",
    "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
    "expect": "40",
    "tools": "basic",
    "require_tool_call": true,
    "max_steps": 6
  }' \
  http://127.0.0.1:8123/api/v1/diagnostics/provider_test
```

Example (Moonshot/Kimi tool call):

```bash
curl -sS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{
    "provider": "moonshot",
    "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
    "expect": "40",
    "tools": "basic",
    "require_tool_call": true,
    "max_steps": 6
  }' \
  http://127.0.0.1:8123/api/v1/diagnostics/provider_test
```

Response fields:
- `ok` (bool)
- `provider`, `base_url`, `model`
- `duration_ms`
- `assistant_text` (truncated at 2048 chars)
- `error` (when non-OK)
- `http_status` (best-effort provider HTTP status)
- `run` (optional, when `include_run=true`)

Notes:
- Provider tests use the same API key resolution as normal runs (config, env, repo-local secrets).
- No session is created (`no_session=true`).
- Tests respect daemon limits and defaults.
