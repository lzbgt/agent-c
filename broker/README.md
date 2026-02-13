# agentd-broker

`agentd-broker` is a small control-plane service that:

- Accepts long-lived **agent connections** over WebSocket (typically `wss://.../v1/agent/connect`), optionally gated by **mTLS**.
- Exposes **OIDC-authenticated** HTTP endpoints for users to:
  - Create/list agents
  - Proxy requests to a connected agent
  - Subscribe to broker events via SSE

This directory also includes `agentd-connector`, a lightweight “bridge” that connects a local `agentd` instance to the broker.

## Endpoints

- `GET /healthz` — liveness (always `ok: true` if the process is serving HTTP)
- `GET /readyz` — readiness (checks Postgres ping and OIDC provider initialization)
- `GET /metrics` — Prometheus metrics (uptime, readiness, connected agents)
  - Includes client auth reload gauges (`broker_client_auth_*`)
- `GET /v1/agent/connect` — agent WebSocket (mTLS recommended)
- `GET /v1/agents` / `POST /v1/agents` — list/create agents (OIDC required)
- `POST /v1/orchestrate` — fan-out `/api/v1/run` calls across multiple agents (OIDC required)
- `GET /v1/agents/{agent_id}/proxy/...` — proxy HTTP request to agent (OIDC required)
- `GET /v1/agents/{agent_id}/proxy_sse/...` — proxy streaming/SSE-like request to agent (OIDC required)
- `GET /v1/events` — Server-Sent Events stream for the authenticated subject (OIDC required)

## Production settings (recommended)

### Broker (`cmd/agentd-broker`)

Key flags:

- TLS / agent auth:
  - `--tls-cert`, `--tls-key` (serve HTTPS)
  - `--tls-client-ca` (enable optional client cert verification)
  - `--require-agent-mtls` (default `true`)
  - `--agent-cn-prefix` (CN prefix used to map client cert CN → agent id)
- OIDC / DB:
  - `--db-dsn` (or `AGENTD_BROKER_DB_DSN` / `DATABASE_URL`)
  - `--oidc-issuer`, `--oidc-audience`
- Optional client token auth:
  - `--client-auth-file` (JSON file with static client tokens; env `AGENTD_BROKER_CLIENT_AUTH_FILE`)
  - `--client-auth-fallback` (allow client tokens when OIDC auth fails; env `AGENTD_BROKER_CLIENT_AUTH_FALLBACK=1`)
  - `--client-auth-reload-ms` (periodically reload client auth file; env `AGENTD_BROKER_CLIENT_AUTH_RELOAD_MS`)
  - Send `SIGHUP` to reload immediately
- Resource limits:
  - `--max-pending-per-agent` (default `256`)
  - `--max-streams-per-agent` (default `64`)
  - `--max-body-bytes` (default `64MiB`)
  - `--max-header-bytes` (default `1MiB`)
- Browser support:
  - `--cors-origins` (comma-separated allowed origins)
  - `--sse-keepalive` (default `15s`)
- Ops:
  - `--read-timeout` (default `0`, disabled; keep `0` for SSE)
  - `--write-timeout` (default `0`, disabled; keep `0` for SSE)
  - `--idle-timeout` (default `120s`)
  - `--read-header-timeout` (default `10s`)
  - `--shutdown-timeout` (default `15s`)
  - `--ready-cache` (default `5s`)

Client auth file format:

```json
{
  "clients": [
    {
      "client_id": "service-a",
      "token": "REDACTED_TOKEN",
      "admin": false,
      "allowed_agents": ["agent1", "agent2"]
    }
  ]
}
```

Notes:
- Client tokens can proxy/orchestrate requests; agent list/create require OIDC.
- If OIDC is not configured, `--client-auth-file` is required.
- Example file: `broker/client_auth.example.json`
- When OIDC is disabled and client auth is configured, `/readyz` only checks DB health.

### Connector (`cmd/agentd-connector`)

The connector:

- Uses a single write-lock to avoid unsafe concurrent WebSocket writes.
- Reconnects with exponential backoff if the broker connection drops.
- Sends periodic WebSocket pings and maintains read deadlines via pong handling.

Typical flags:

- `--broker wss://.../v1/agent/connect`
- `--local-agentd http://127.0.0.1:8123`
- `--tls-ca`, `--tls-cert`, `--tls-key` (for `wss` + mTLS)
- `--agent-id` (optional if derivable from cert CN)

## Reverse proxy notes

- For `GET /v1/events` (SSE), ensure your proxy does **not buffer** responses.
  - The broker also sets `X-Accel-Buffering: no` as a hint for Nginx.
