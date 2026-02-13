# Cloud Broker (Secure Relay) — Design

This document defines a **cloud broker** for securely relaying requests between:

- multiple **agentd deployments** (desktop/server “workers” behind NAT, or in private networks)
- multiple **client types** (WebUI, mobile, backend services, CLI)

The broker provides:

1) **Mutual registry**: agents connect outbound and become addressable by `agent_id`
2) **Client authentication + authorization**: OIDC/JWT for users + DB-backed agent membership checks
3) **Management API**: list/create/delete agents, inspect status/metadata, disconnect
4) **Secure relay API**: clients send requests to a specific agentd via broker, without directly reaching the agent network
5) **SSE support**: broker can proxy streaming endpoints (SSE) without fragile “file path” hacks

## Why a broker (vs exposing agentd HTTP directly)

Directly exposing `agentd` is hard to secure and operate at scale:

- many agents are behind NAT / mobile networks
- many clients are browsers (no raw TCP, limited mTLS)
- you want centralized authZ, audit, rate limits, revocation
- you may want to coordinate **multiple agents** for one user task

The broker model reverses the connectivity:

- agents open **outbound** long-lived connections to the broker
- clients connect to the broker and request actions against agents by id

## High-level architecture

**Control plane**
- Broker HTTPS server (public)
- Client API (OIDC/JWT bearer tokens)
- Agent management API (per-user + admin)
- Postgres for durable state/audit

**Data plane**
- Agent → Broker: `wss://` outbound websocket with **mTLS**
- Client → Broker: HTTPS API calls (OIDC bearer token)
- Broker routes messages to the correct connected agent

## Identity / security model

### Agent identity

Agents authenticate with **mTLS**.

Convention (same idea as the urine-monitor hub platform):
- Agent identity is encoded in the client certificate **CN**
- Example CN: `agentd-123`

Broker verifies:
- TLS stack verifies the chain against `--tls-client-ca`
- broker extracts CN and ensures it matches the agent’s declared `agent_id`

This prevents an agent from impersonating another agent id.

### Client identity

Clients authenticate with `Authorization: Bearer <jwt>` (OIDC ID token).

Broker authorizes each request using Postgres:
- user identity is the OIDC `sub` claim
- a user may access an agent if they have a row in `broker_agent_memberships`
- admin subs can be configured with `--admin-subs` (comma-separated `sub` values)

This supports multiple users and prevents “UI says it ran” when it did not:
all authorizations and audits are checked/recorded server-side.

Optional: static client tokens

For non-UI service clients (or environments without OIDC), the broker can accept static bearer tokens from a JSON file:
- `--client-auth-file /path/to/client_auth.json`
- `--client-auth-fallback` to allow tokens when OIDC auth fails
- `--client-auth-reload-ms` to periodically reload the file
- `--client-auth-strict` to fail readiness if reload fails
- `--client-auth-max-age-ms` to fail readiness if last reload is too old
- `--client-auth-event-include-error` to include reload error text in events
- Send `SIGHUP` to reload immediately

Client token auth is intended for **proxy/orchestrate** calls only. Agent list/create remains OIDC-only.

Example file: `broker/client_auth.example.json`.

## Protocol between broker and agent connector

Broker and agent communicate over a single websocket connection (JSON messages).

**Agent connects:**
1) Broker accepts TLS+mTLS websocket connection at `/v1/agent/connect`
2) Agent sends:
   - `{"type":"hello","agent_id":"123","meta":{...}}`
3) Broker replies:
   - `{"type":"hello_ack","ok":true,"agent_id":"123"}`

**Broker forwards a client request to a specific agent:**
- Broker → Agent:
  - `{"type":"http_request","id":"<uuid>","req":{"method":"POST","path":"/api/v1/run","query":"","headers":{...},"body_b64":"..."}}`
- Agent → Broker:
  - `{"type":"http_response","id":"<uuid>","resp":{"status":200,"headers":{...},"body_b64":"..."}}`

Notes:
- `body_b64` allows binary payloads (e.g. wav artifacts) without fragile “file path” hacks.
- Streaming responses (SSE / long responses) are supported with:
  - `http_stream_request` / `http_stream_start` / `http_stream_chunk` / `http_stream_end`
  - broker may send `http_stream_cancel` to stop a long-running stream if the client disconnects

## Broker HTTP API

All endpoints below are served by the broker (not by agents).

### Registry / management

- `GET /v1/agents`
  - lists agents the authenticated user is allowed to access (from Postgres)
  - includes connection status (connected/last_seen) from in-memory registry

- `POST /v1/agents`
  - creates a new agent record owned by the authenticated user (also inserts membership as `owner`)
  - response includes `connector_hint_cn` (the CN you should use in an mTLS client cert)

- `GET /v1/agents/{agent_id}/members`
  - lists agent members (owner/admin only)
- `POST /v1/agents/{agent_id}/members`
  - add/update an agent member (owner/admin only)
- `DELETE /v1/agents/{agent_id}/members/{user_sub}`
  - remove an agent member (owner/admin only; cannot remove owner)
- `GET /v1/agents/{agent_id}/membership_audit`
  - membership audit trail for the agent (owner/admin only)
  - query: `limit` (optional, default `200`, max `500`)

- `POST /v1/agents/{agent_id}/delete` (or `DELETE` to the same path)
  - deletes an agent record (owner or admin)

- `POST /v1/agents/{agent_id}/disconnect`
  - disconnects an agent’s websocket (admin only)

### Relay

- `GET/POST/... /v1/agents/{agent_id}/proxy/<agentd_path>`
  - transparent HTTP proxy to the target agent (path and query preserved)
  - auth: OIDC user must be in `broker_agent_memberships` for that agent

- `GET /v1/agents/{agent_id}/proxy_sse/<agentd_path>`
  - streaming proxy intended for SSE endpoints
  - broker flushes chunks as they arrive from the agent connector

#### Using durable workflows through the broker proxy

The broker proxy can be used as a “virtual base URL” for agentd-to-agentd collaboration tasks:

- In durable workflows, set `agentd_call.base_url` to:
  - `https://<broker>/v1/agents/<agent_id>/proxy`
- Or set `agentd_call.broker_proxy:{broker_base_url,agent_id}` and omit `base_url` (server computes/persists the proxy prefix).
- The caller still uses normal agentd endpoints under the proxy prefix:
  - `POST .../proxy/api/v1/workflow/submit`
  - `GET  .../proxy/api/v1/workflow?workflow_id=...`
- Broker auth is typically an OIDC bearer token; in workflows, use `agentd_call.bearer_env` so the token value is not persisted.

### Orchestration (fan-out)

- `POST /v1/orchestrate`
  - broker-managed fan-out across multiple agents (OIDC required)
  - each task becomes an HTTP request to the target agent (defaults to `POST /api/v1/run`)
  - intended for multi-agent workflows where the client wants one aggregated response

Request (JSON):
- `tasks` (array, required): each task is:
  - `agent_id` (string, required)
  - `task_id` (string, optional)
  - `request` (object, optional): agentd run request body (if omitted, remaining task keys are treated as the request body)
  - `headers` (object, optional): forwarded to agentd (broker auth headers are never forwarded)
  - `method` (string, optional, default `POST`)
  - `path` (string, optional, default `/api/v1/run`)
  - `query` (string, optional, default empty)
- `defaults` (object, optional): merged into each task request (missing keys only)
- `allow_sessions` (bool, optional, default `false`): when `false`, broker forces `no_session=true` and defaults `tools="none"` for safety
- `max_concurrency` (int, optional, default `4`, max `16`)
- `timeout_ms` (int, optional, default `60000`, max `300000`)

Response (JSON):
- `ok` (bool)
- `all_ok` (bool)
- `results` (array): list of `{ task_id, agent_id, ok, http_status?, ms, result?, error? }`

### Broker events (SSE)

- `GET /v1/events`
  - server-sent events for the authenticated user
  - emits JSON `data:` payloads with types like `agent_connected`, `agent_disconnected`, `relay_audit`, `client_auth_reload`, `agent_member_updated`

### Client auth status (admin-only)

- `GET /v1/client_auth/status`
  - reports the last reload status/time for the client auth file
- `POST /v1/client_auth/reload`
  - triggers an immediate reload of the client auth file

### Trace correlation (debugging)

- `GET /v1/trace?trace_id=...`
  - returns broker relay audit rows for the trace id
  - returns persisted broker orchestrate summaries for the trace id (request/response JSON, redacted)
  - returns membership audit rows for the trace id (when membership updates include the same trace_id)
  - membership audit rows are limited to agents the caller can access (admins can see all)
  - best-effort: fans out to referenced agents to query their `GET /api/v1/trace?trace_id=...` endpoint (if supported)

## Multi-agent workflows

The broker supports multi-agent usage by design:
- multiple agents can be connected simultaneously
- client can choose which `agent_id` to send each request to

## WebUI broker console

The WebUI can operate in **broker mode** (OIDC) and now includes a broker console panel:
- list agents and select the active `agent_id`
- manage agent memberships (add/remove roles)
- view membership audit trail (per agent)

Configure in the WebUI Settings:
- Connection mode: `broker`
- Broker base URL + bearer token

This enables “complex tasks” split across specialized agents:
- one agent with full host tools on a workstation
- another agent in a GPU server environment
- another agent attached to lab equipment

Orchestration strategies (future work):
- broker-managed “task sessions” spanning multiple agents
- fan-out calls + gather results
- distributed tool call limits / budgets

## Threat model (MVP)

Assumptions:
- broker public endpoint is reachable by clients and agents
- agent networks are not directly reachable by clients
- trust anchor: broker CA for agent mTLS, plus broker client-token store

Mitigations:
- mTLS for agent channels (identity + encryption)
- DB-backed membership checks for which agents can be controlled
- audit logging in Postgres (`broker_relay_audit`)
- rate limiting (future)

## Local quickstart (dev)

1) Generate local mTLS test certs:

```sh
bash tools/gen_agentd_broker_mtls_test_certs.sh tools/_agentd_broker_mtls_test_certs 1
```

2) Ensure Postgres is reachable and set a DSN:

```sh
export AGENTD_BROKER_DB_DSN='postgres://...'
```

3) Start the broker:

```sh
cd broker
go run ./cmd/agentd-broker \
  --listen :8443 \
  --tls-cert ../tools/_agentd_broker_mtls_test_certs/server.pem \
  --tls-key  ../tools/_agentd_broker_mtls_test_certs/server.key.pem \
  --tls-client-ca ../tools/_agentd_broker_mtls_test_certs/ca.pem \
  --db-dsn "$AGENTD_BROKER_DB_DSN" \
  --oidc-issuer 'https://YOUR_ISSUER' \
  --oidc-audience 'YOUR_CLIENT_ID'
```

4) Register an agent via the broker management API (needs an OIDC bearer token):

```sh
curl -sS -H "Authorization: Bearer $OIDC_JWT" \
  -H 'Content-Type: application/json' \
  -d '{"agent_id":"1"}' \
  https://localhost:8443/v1/agents
```

5) Start `agentd` locally (bind loopback only), then start the connector:

```sh
./build/agentd --host 127.0.0.1 --port 8123
cd broker
go run ./cmd/agentd-connector \
  --broker wss://localhost:8443/v1/agent/connect \
  --local-agentd http://127.0.0.1:8123 \
  --tls-ca   ../tools/_agentd_broker_mtls_test_certs/ca.pem \
  --tls-cert ../tools/_agentd_broker_mtls_test_certs/client_1.pem \
  --tls-key  ../tools/_agentd_broker_mtls_test_certs/client_1.key.pem
```

6) Proxy a request through the broker:

```sh
curl -sS -H "Authorization: Bearer $OIDC_JWT" \
  https://localhost:8443/v1/agents/1/proxy/api/v1/health
```

7) Stream an SSE endpoint through the broker:

```sh
curl -N -H "Authorization: Bearer $OIDC_JWT" \
  https://localhost:8443/v1/agents/1/proxy_sse/api/v1/job/stream
```
