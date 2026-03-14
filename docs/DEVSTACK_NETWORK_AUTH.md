# Devstack Network + Auth Workflow

This document explains how the devstack wires networking and authentication
for local testing (agentd + broker + WebUI + Keycloak).

## Topology (host + docker)

Devstack runs a mixed topology:
- **Host processes**: agentd, broker, connector, WebUI static server.
- **Docker compose**: Keycloak + Postgres (published to host ports).

By default the broker runs over plain HTTP to avoid local TLS trust issues.
Enable HTTPS + mTLS with `tools/devstack_agent.sh --broker-tls`.

Ports are chosen at runtime and written to `out/devstack_state.json`.
The compose project name matches the WebUI port by default
(`agent_devstack_<webui_port>`).
`tools/devstack_agent.sh` now treats that state file as the canonical singleton
stack: if the recorded broker/WebUI are still alive it reuses them instead of
starting a second stack, and `--restart` replaces the live stack explicitly.

Example (from `out/devstack_state.json`):
- agentd: `http://127.0.0.1:<agentd_port>`
- broker: `http://127.0.0.1:<broker_port>` (default; `https://` when `--broker-tls`)
- WebUI: `http://127.0.0.1:<webui_port>`
- Keycloak: `http://keycloak.lvh.me:<keycloak_port>` (or `127.0.0.1` / `[::1]`)
- Postgres: `127.0.0.1:<postgres_port>`

```
          (host)
  WebUI -> broker (HTTP by default, OIDC bearer)
              ^        ^
              |        |
          connector   Keycloak (OIDC issuer)
              |        |
           agentd      Postgres
```

## Auth flows

### 1) Agentd auth (daemon token)

- Devstack sets `AGENTD_AUTH_TOKEN=dev-agentd-token`.
- Most `/api/v1/*` endpoints require:
  `Authorization: Bearer dev-agentd-token`.
- Health/ready endpoints are unauthenticated.

Example:
```
curl -H "Authorization: Bearer dev-agentd-token" \
  http://127.0.0.1:<agentd_port>/api/v1/caps
```

### 2) Broker auth (OIDC bearer)

Broker uses Keycloak as its OIDC issuer:
- Realm: `agentd`
- Client ID: `agentd-broker-dev`
- User: `test`
- Password: `test`

Token helper:
```
tools/devstack_oidc_token.sh --state out/devstack_state.json
```

If `keycloak.lvh.me` is not directly reachable on your host, the helper will try
`127.0.0.1` / `[::1]` transport targets while preserving the original `Host`
header so Keycloak still mints a token with the broker-expected issuer.

Tokens expire. The dev realm sets `accessTokenLifespan` to 12 hours for smoother
testing; if you still hit 401s, regenerate a token or restart Keycloak to pick
up realm changes.

Example:
```
OIDC_TOKEN="$(tools/devstack_oidc_token.sh --state out/devstack_state.json)"
curl -H "Authorization: Bearer ${OIDC_TOKEN}" \
  http://127.0.0.1:<broker_port>/v1/agents
```

For broker proxy or broker session-alias calls that target a native `agentd`,
also pass the daemon bearer through `X-Agentd-Authorization`:

```
curl -H "Authorization: Bearer ${OIDC_TOKEN}" \
  -H "X-Agentd-Authorization: Bearer dev-agentd-token" \
  http://127.0.0.1:<broker_port>/v1/agents/<agent_id>/proxy/api/v1/health
```

### 3) Connector mTLS (agent to broker, optional)

When `--broker-tls` is enabled, devstack generates mTLS test certs and the
connector uses them to open `wss://127.0.0.1:<broker_port>/v1/agent/connect`.
Without `--broker-tls`, the connector uses plain `ws://` and no mTLS.

This connection is independent from the OIDC bearer token used by WebUI and
clients; it is a mutual TLS channel for agent traffic.

## WebUI behavior

Devstack writes the broker token into `ui/dist/agentui-config.js` on startup:
```
brokerAuthToken: "<OIDC token>"
```

If the WebUI shows:
> Unauthorized: the broker requires an OIDC bearer token.

Do one of the following:
1) Refresh the WebUI after devstack starts (token is embedded at startup).
2) Generate a token with `tools/devstack_oidc_token.sh` and paste it into
   Settings → Connection → Broker auth token.
3) If you refreshed tokens, refresh the page so the WebUI reloads config.

## Troubleshooting

- **Need to see whether the canonical stack is still alive**:
- Run `tools/devstack_status.sh` for a concise stack summary.
  - `status=live`: broker/WebUI processes are alive and `agentd /health`, broker `/healthz`, and broker `/readyz` are all returning `200`.
  - `status=degraded`: the browser-facing processes are alive but one of those readiness probes is failing.
  - `tools/devstack_status.sh --require-live` only requires the canonical broker/WebUI processes to exist.
  - `tools/devstack_status.sh --require-ready` requires the full stack to be ready for broker-backed testing.
- **Need to replace the current stack intentionally**:
  - Run `tools/devstack_agent.sh --restart` or stop it first with
    `tools/devstack_agent_down.sh --state out/devstack_state.json`.

- **Keycloak not reachable**:
  - Try `http://127.0.0.1:<keycloak_port>` or `http://[::1]:<keycloak_port>`.
  - Ensure docker compose is running (`docker compose ls`).
- **WebUI Unauthorized**:
  - Broker token missing/expired. Regenerate with `tools/devstack_oidc_token.sh`.
- **Browser tries `keycloak.lvh.me:<port>/v1/agents/...`**:
  - Your Broker base URL is misconfigured (Keycloak is the issuer, not the broker).
  - Reset WebUI Settings → Connection to `http://127.0.0.1:<broker_port>` (or `https://` if using `--broker-tls`) and paste a fresh broker token.
- **Browser cannot reach `http://127.0.0.1:<broker_port>/v1/agents/<id>/proxy`**:
  - Ensure the WebUI origin is `http://127.0.0.1:<webui_port>` or `http://localhost:<webui_port>` (broker CORS allowlist).
  - If you enabled `--broker-tls`, accept the self-signed cert by visiting `https://127.0.0.1:<broker_port>` directly in the browser.
- **Broker TLS handshake errors in logs**:
  - Common during probing without proper certs when `--broker-tls` is enabled; not fatal.
- **Ports unknown**:
  - Check `out/devstack_state.json`.
