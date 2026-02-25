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

Spawn request statuses (convention):
- `requested` → adapter should pick up
- `allocating` → adapter claimed and is provisioning
- `allocated` → members assigned
- `error` → provisioning failed

## Adapter CLI (broker/cmd/agentd-spawn-adapter)

Required:
- `--broker-base` (env: `BROKER_BASE`)
- `--oidc-token` (env: `BROKER_OIDC_TOKEN`)
- `--command` (env: `SPAWN_COMMAND`)

Optional:
- `--adapter-id` (env: `SPAWN_ADAPTER_ID`)
- `--poll-interval` (default: 3s)
- `--command-timeout` (default: 2m)
- `--limit` (default: 50)
- `--status` (default: requested)
- `--once` (process one poll cycle and exit)
- `--insecure` (env: `BROKER_INSECURE_TLS=1`)

The adapter:
1) Lists teams for the token.
2) Lists spawn requests per team (status filter).
3) Claims the request by setting `status=allocating` and recording `adapter_id` in `meta`.
4) Executes the spawn command.
5) Updates the request with `status`, `assigned_members`, `error`, and merged `meta`.

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
