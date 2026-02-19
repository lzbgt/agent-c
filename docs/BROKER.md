# Cloud Broker (Secure Relay) — Design

Design context: see `DESIGN.md` for the system-wide architecture; this document focuses on broker-specific design and APIs.

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

Optional: cookie-based auth

For browser clients that cannot attach `Authorization` headers directly, the broker can accept a bearer token from a cookie:
- configure `--auth-cookie <name>` (or env `AGENTD_BROKER_AUTH_COOKIE`)
- the cookie value should be a raw JWT (or `Bearer <jwt>`)
- enable CORS credentials (`--cors-allow-credentials`) and **explicit origins** (not `*`)

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
   - Optional `meta.deployment_id` to distinguish multiple deployments for the same `agent_id`
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

### Transport interface (prep for multi-transport)

The broker now uses a transport-agnostic connector interface so new transports can be added without changing relay logic:

- Interface: `broker/internal/transport/conn.go`
- WebSocket adapter: `broker/internal/transport/websocket.go`

This keeps the business logic centered on `transport.Conn` while retaining today’s WebSocket connector path.

## Broker HTTP API

All endpoints below are served by the broker (not by agents).

### Registry / management

- `GET /v1/agents`
  - lists agents the authenticated user is allowed to access (from Postgres)
  - includes connection status (connected/last_seen) from in-memory registry
  - includes connected deployment list per agent when present

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

- `GET /v1/teams`
  - lists teams owned by the authenticated user (admin can list all)
- `POST /v1/teams`
  - creates a new team owned by the authenticated user
- `GET /v1/teams/{team_id}`
  - fetches a team record (owner/admin only)
- `PATCH /v1/teams/{team_id}`
  - updates team metadata (owner/admin only)
- `DELETE /v1/teams/{team_id}`
  - deletes a team (owner/admin only)
- `GET /v1/teams/{team_id}/members`
  - lists team members (owner/admin only)
- `POST /v1/teams/{team_id}/members`
  - adds a team member (owner/admin only)
- `PATCH /v1/teams/{team_id}/members/{member_id}`
  - updates a team member (owner/admin only)
- `DELETE /v1/teams/{team_id}/members/{member_id}`
  - removes a team member (owner/admin only)
- `GET /v1/teams/{team_id}/quorum`
  - lists quorum rules (owner/admin only)
- `POST /v1/teams/{team_id}/quorum`
  - creates a quorum rule (owner/admin only)
- `PATCH /v1/teams/{team_id}/quorum/{rule_id}`
  - updates a quorum rule (owner/admin only)
- `DELETE /v1/teams/{team_id}/quorum/{rule_id}`
  - deletes a quorum rule (owner/admin only)
- `POST /v1/teams/{team_id}/runs`
  - executes a synchronous fan-out run across active team members (role filter optional)
  - supports **quorum gating** for team-run approvals when quorum rules use `action:"team_run"`
    - request payload may include:
      - `team.quorum_policy.mode`: `auto` (default) or `off`
      - `team.approvals`: array of member IDs or objects `{member_id, decision, reason}`
    - `decision` defaults to `approve` and only `approve` counts toward quorum
    - when a strict quorum rule is not satisfied, the broker returns `409` with a `quorum` object describing missing approvals
- `GET /v1/teams/{team_id}/runs/{team_run_id}`
  - returns the stored team run status + current member list
- `GET /v1/teams/{team_id}/runs/{team_run_id}/approvals`
  - lists persisted approvals for a team run (owner/admin only)
- `POST /v1/teams/{team_id}/runs/{team_run_id}/approvals`
  - creates approvals for a team run (owner/admin only)
  - body supports either a single approval object or `{ "approvals": [...] }`

- `POST /v1/agents/{agent_id}/delete` (or `DELETE` to the same path)
  - deletes an agent record (owner or admin)

- `POST /v1/agents/{agent_id}/disconnect`
  - disconnects an agent’s websocket (admin only)
- `GET /v1/agents/{agent_id}/deployments`
  - lists connected deployments for a given `agent_id`
  - includes `default_deployment_id` (the most recent connected deployment)

### Relay

- `GET/POST/... /v1/agents/{agent_id}/proxy/<agentd_path>`
  - transparent HTTP proxy to the target agent (path and query preserved)
  - auth: OIDC user must be in `broker_agent_memberships` for that agent
  - optional header `X-Agentd-Deployment: <deployment_id>` (or query `deployment_id`) to target a specific deployment
  - if no deployment is specified, broker defaults to the most recent connected deployment

- `GET /v1/agents/{agent_id}/proxy_sse/<agentd_path>`
  - streaming proxy intended for SSE endpoints
  - broker flushes chunks as they arrive from the agent connector
  - optional header `X-Agentd-Deployment: <deployment_id>` (or query `deployment_id`) to target a specific deployment

Idempotency (optional):
- send `Idempotency-Key` (or `X-Idempotency-Key`) to safely retry proxied requests
- if the same key is reused with a different request payload, the broker returns `409` (`idempotency_key_conflict`)
- if a request is already in progress for the key, the broker returns `409` (`idempotency_key_in_progress`)
- successful replays return `X-Idempotency-Replay: true`
- large responses are not stored; the broker responds with `X-Idempotency-Disabled: response_too_large`

### Audio signaling (WebRTC relay)

The broker provides a **signaling relay** for audio sessions (media flows directly between WebUI and agentd):

- `POST /v1/audio/sessions` (create a session; returns `session_id`)
- `POST /v1/audio/sessions/{session_id}/signal` (send offer/answer/candidate/bye)
- `GET  /v1/audio/sessions/{session_id}/signal/stream` (SSE stream of signaling events)

Sessions are in-memory and expire after a TTL (default 15 minutes). Configure via:
- `--audio-session-ttl` (duration)
- `AGENTD_BROKER_AUDIO_SESSION_TTL_MS` (milliseconds)

#### Using durable workflows through the broker proxy

The broker proxy can be used as a “virtual base URL” for agentd-to-agentd collaboration tasks:

- In durable workflows, set `agentd_call.base_url` to:
  - `https://<broker>/v1/agents/<agent_id>/proxy`
- Or set `agentd_call.broker_proxy:{broker_base_url,agent_id}` and omit `base_url` (server computes/persists the proxy prefix).
- The caller still uses normal agentd endpoints under the proxy prefix:
  - `POST .../proxy/api/v1/workflow/submit`
  - `GET  .../proxy/api/v1/workflow?workflow_id=...`
- Broker auth is typically an OIDC bearer token; in workflows, use `agentd_call.bearer_env` so the token value is not persisted.

#### Operational control (OTA, maintenance)

Operational endpoints can also be routed through the proxy:

- `POST /v1/agents/{agent_id}/proxy/api/v1/ota/update`
- `GET  /v1/agents/{agent_id}/proxy/api/v1/ota/status`

Use `X-Agentd-Deployment` to target a specific deployment, or omit it to target the broker’s most recent deployment.
`/api/v1/ota/status` surfaces drain hints (`drain_active`, `drain_until_unix_ms`, `drain_reason`) during updates.

Broker bulk OTA fan-out (multi-deployment):

- `POST /v1/agents/{agent_id}/ota/update`
  - body: `{ url, sha256?, version?, reason?, trace_id?, drain_timeout_ms?, deployment_ids?, deployment_id?, deployments? }`
  - if deployment IDs are omitted, broker fans out to all connected deployments for the agent
- `GET /v1/agents/{agent_id}/ota/status`
  - query: `deployment_ids=dep1,dep2` (optional; defaults to all connected deployments)

Both return `{ ok, agent_id, total, ok_count, error_count, results[] }`, where each result is
`{ deployment_id, status, data }` with `data` mirroring the agent’s OTA endpoint JSON.

Broker bulk memory maintenance fan-out (multi-deployment):

- `POST /v1/agents/{agent_id}/memory/retention/enforce`
  - body: memory retention request + optional deployment ids
- `GET /v1/agents/{agent_id}/memory/recaps`
  - query: `limit`, `include_summary`, and optional `deployment_ids=dep1,dep2`
- `POST /v1/agents/{agent_id}/memory/recaps`
  - body: memory recap request + optional deployment ids
- `GET /v1/agents/{agent_id}/memory/salience`
  - query: salience policy params (`include_structured`, `include_daily`, `daily_days`, `max_*`, `half_life_days`, `importance_weight`)
    plus optional `deployment_ids=dep1,dep2`

These also return `{ ok, agent_id, total, ok_count, error_count, results[] }` with `{ deployment_id, status, data }`
per deployment. The `data` field mirrors the underlying agentd memory endpoint response.

### Orchestration (fan-out)

- `POST /v1/orchestrate`
  - broker-managed fan-out across multiple agents (OIDC required)
  - each task becomes an HTTP request to the target agent (defaults to `POST /api/v1/run`)
  - intended for multi-agent workflows where the client wants one aggregated response

Request (JSON):
- `tasks` (array, required): each task is:
  - `agent_id` (string, required)
  - `deployment_id` (string, optional): target a specific deployment for the agent
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

Idempotency (optional):
- send `Idempotency-Key` (or `X-Idempotency-Key`) to safely retry an orchestrate request
- `X-Idempotency-Replay: true` indicates a replayed response
- `409` responses indicate `idempotency_key_in_progress` or `idempotency_key_conflict`

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

## CORS (browser clients)

The broker supports CORS for browser clients with **explicit opt-in**:

- `--cors-origin <origin>` (repeatable) or `--cors-origins <csv>`
  - accepts exact origins, `*`, or `re:<regex>`
- `--cors-allow-headers <csv>` (default includes Authorization + tracing + idempotency headers)
- `--cors-allow-methods <csv>` (default: GET, POST, PUT, PATCH, DELETE, OPTIONS)
- `--cors-allow-credentials` (enable cookie auth; requires explicit origins)
- `--cors-max-age-seconds <n>`

Per-route policies:
- `--cors-route '{"path_prefix":"/v1/agents","origins":["https://ui.example"],"allow_credentials":true}'`
- or env `AGENTD_BROKER_CORS_ROUTES='[{"path_prefix":"/v1/agents","origins":["https://ui.example"]}]'`
- precedence: **longest `path_prefix` match wins**
- origin match precedence: exact > regex > `*`

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

5) Start `agentd` locally (bind loopback only), then start the connector.
   If agentd uses `--auth-token`, pass the same token via `--local-agentd-token`
   or set `AGENTD_AUTH_TOKEN` for the connector process.

```sh
./build/agentd --host 127.0.0.1 --port 8123
cd broker
go run ./cmd/agentd-connector \
  --broker wss://localhost:8443/v1/agent/connect \
  --local-agentd http://127.0.0.1:8123 \
  --local-agentd-token "$AGENTD_AUTH_TOKEN" \
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
