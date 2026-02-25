# Agent Spawn Adapter v0

Date: 2026-02-26  
Status: draft (rolling)

## Goals

1) **Full automation by default**
   - Spawn requests are fulfilled without user intervention.
   - The adapter can run unattended and update broker state.

2) **Low coupling**
   - The broker only stores spawn requests and emits events.
   - Provisioning is handled by an external adapter process.

3) **Auditable**
   - Spawn requests are persisted in the broker DB.
   - Adapter updates are recorded as broker events.

## Non-goals (v0)

- No multi-tenant scheduling or autoscaling.
- No opinionated provisioning backend (VMs, containers, k8s).
- No credential distribution or secret management.

## Inputs (broker APIs)

- `GET /v1/teams`
- `GET /v1/teams/{team_id}/orchestrator/spawn_requests?status=requested`
- `PATCH /v1/teams/{team_id}/orchestrator/spawn_requests/{spawn_request_id}`
  - Optional `expected_status` guard to prevent double-claim races.

Spawn request statuses (convention):
- `requested` → adapter should pick up
- `allocating` → adapter claimed and is provisioning
- `allocated` → members assigned
- `error` → provisioning failed

## Adapter CLI (broker/cmd/agentd-spawn-adapter)

Required:
- `--broker-base` (env: `BROKER_BASE`)
- `--oidc-token` (env: `BROKER_OIDC_TOKEN`, OIDC or client auth token)
- `--command` (env: `SPAWN_COMMAND`)

Optional:
- `--adapter-id` (env: `SPAWN_ADAPTER_ID`)
- `--poll-interval` (default: 3s)
- `--command-timeout` (default: 2m)
- `--limit` (default: 50)
- `--status` (default: requested)
- `--allocator` (env: `SPAWN_ALLOCATOR=1`, use broker runtime member allocator; no command required)
- `--once` (process one poll cycle and exit)
- `--insecure` (env: `BROKER_INSECURE_TLS=1`)

The adapter:
1) Lists teams for the token.
2) Lists spawn requests per team (status filter).
3) Claims the request by setting `status=allocating`, sending `expected_status=requested`, and recording `adapter_id` in `meta`.
   - A `409` response means another adapter claimed it first; treat as non-fatal and move on.
4) Executes the spawn command (or allocator mode).
5) Updates the request with `status`, `assigned_members`, `error`, and merged `meta`.

## Allocator mode (optional)

When `--allocator` is enabled, the adapter calls
`POST /v1/teams/{team_id}/runtime_members/allocate` using the requested role
and updates the spawn request with the allocated runtime members. This mode
is useful for fully-automatic dev stacks where connected agents can be reused
without provisioning new hosts.

Notes:
- `SPAWN_COMMAND` is optional when `--allocator` is enabled.
- The adapter records allocator warnings/missing roles in spawn request meta.
- Client auth tokens require broker `--client-auth-allow-automation`.

## Spawn command contract

The adapter runs the command via `/bin/sh -c` with environment variables:

- `SPAWN_REQUEST_ID`
- `TEAM_ID`
- `ORCHESTRATOR_RUN_ID`
- `ROLE`
- `COUNT`
- `STATUS`
- `REQUIREMENTS_JSON` (JSON map)
- `META_JSON` (JSON map)

The command should emit JSON to stdout:

```
{
  "status": "allocated",
  "assigned_members": [
    {"agent_id": "agent-1", "role": "planner", "deployment_id": "dep-1"}
  ],
  "error": "",
  "meta": {"provisioner": "local"}
}
```

Notes:
- `status` defaults to `allocated` when omitted.
- `assigned_members` is optional; omit to keep existing assignments.
- Any non-empty `error` forces status to `error`.

## Events

The broker emits:
- `orchestrator_spawn_requested`
- `orchestrator_spawn_updated`
- `orchestrator_spawn_status`

These are persisted and available via event replay for reload-safe UIs.
