# Devstack Network + Auth Workflow

This document explains how the devstack wires networking and authentication
for local testing (agentd + broker + WebUI + Keycloak).

## Topology (host + docker)

Devstack runs a mixed topology:
- **Host processes**: agentd, broker, connector, WebUI static server.
- **Docker compose**: Keycloak + Postgres (published to host ports).

Ports are chosen at runtime and written to `out/devstack_state.json`.
The compose project name matches the WebUI port by default
(`agent_devstack_<webui_port>`).

Example (from `out/devstack_state.json`):
- agentd: `http://127.0.0.1:<agentd_port>`
- broker: `https://127.0.0.1:<broker_port>`
- WebUI: `http://127.0.0.1:<webui_port>`
- Keycloak: `http://keycloak.lvh.me:<keycloak_port>` (or `127.0.0.1` / `[::1]`)
- Postgres: `127.0.0.1:<postgres_port>`

```
          (host)
  WebUI -> broker (HTTPS, OIDC bearer)
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

If `keycloak.lvh.me` does not resolve on your host, the helper auto-falls back to
`http://127.0.0.1:<keycloak_port>` or `http://[::1]:<keycloak_port>`.

Example:
```
OIDC_TOKEN="$(tools/devstack_oidc_token.sh --state out/devstack_state.json)"
curl -k -H "Authorization: Bearer ${OIDC_TOKEN}" \
  https://127.0.0.1:<broker_port>/v1/agents
```

### 3) Connector mTLS (agent to broker)

Devstack generates mTLS test certs and the connector uses them to open
`wss://127.0.0.1:<broker_port>/v1/agent/connect`.

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

- **Keycloak not reachable**:
  - Try `http://127.0.0.1:<keycloak_port>` or `http://[::1]:<keycloak_port>`.
  - Ensure docker compose is running (`docker compose ls`).
- **WebUI Unauthorized**:
  - Broker token missing/expired. Regenerate with `tools/devstack_oidc_token.sh`.
- **Broker TLS handshake errors in logs**:
  - Common during probing without proper certs; not fatal.
- **Ports unknown**:
  - Check `out/devstack_state.json`.
