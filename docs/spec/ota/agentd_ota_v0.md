# Agentd OTA Update (v0)

Date: 2026-02-19
Status: v0 (implemented in this repo; rolling)

## Goals

- Remote OTA update for **agentd service instances** via the platform (WebUI + broker).
- Support **multiple deployments** per agent and update all or a selected subset.
- Preserve **task continuity** after OTA (durable jobs/workflows resume after restart).
- Provide a **safe, explicit** operator-controlled update mechanism with auditability.
- Enable **staging + rollback-friendly** workflows (explicit plan files, version labels, and operator-driven reapply).
- Surface **observability** signals (per-deployment status, drain state, errors) in the WebUI.

All formerly deferred OTA capabilities are explicit goals and tracked in `TODOS.md`.

## Background / Constraints

- `agentd` is frequently installed as a **system service** (systemd/launchd/Windows service).
- Replacing the running binary typically requires a privileged external step and/or a service manager restart.
- Task continuity is already supported for **durable jobs** and **workflows**:
  - `run_async` jobs are persisted and resumed by the JobEngine after restart.
  - Workflows are persisted and resumed by the WorkflowEngine after restart.
- OTA must therefore integrate with **service managers** and provide a controlled handoff.

## Architecture Overview

The OTA flow is split into two concerns:

1) **Control plane (agentd HTTP API)** — accepts OTA requests, validates them, writes a plan file, and invokes an
   operator-provided command.
2) **Execution plane (ota command/script)** — downloads/verifies the update, swaps binaries, and restarts the service.

This keeps agentd **portable** and avoids embedding platform-specific update logic in C++.

## API (agentd)

### POST /api/v1/ota/update

Request body (JSON):

- `version` (string, optional): version label for auditing.
- `url` (string, required): artifact URL (https/file) for the new agentd binary or package.
- `sha256` (string, optional): expected sha256 of the artifact.
- `reason` (string, optional): human-readable reason.
- `drain_timeout_ms` (int, optional): grace period before requesting restart (best-effort).
- `trace_id` (string, optional): audit correlation.

Response (JSON):

- `ok` (bool)
- `ota_id` (string)
- `status` (string): `queued` | `running` | `error`
- `error` (string, optional)

### GET /api/v1/ota/status

Returns:

- `ok` (bool)
- `ota_id` (string, optional)
- `status` (string)
- `updated_unix_ms` (int)
- `last_error` (string, optional)
- `plan_path` (string, optional)

## Agentd Configuration

OTA is **disabled by default**. Operators must enable it explicitly.

New configuration knobs:

- `--ota-enable` (env `AGENTD_OTA_ENABLE=1`)
- `--ota-command <path>` (env `AGENTD_OTA_COMMAND`)
- `--ota-command-timeout-ms <n>` (env `AGENTD_OTA_COMMAND_TIMEOUT_MS`)
- `--ota-drain-timeout-ms <n>` (env `AGENTD_OTA_DRAIN_TIMEOUT_MS`)
- `AGENTD_OTA_RESTART=systemd|launchd|signal` (default `signal`)
- `AGENTD_OTA_SERVICE=<service>` for systemd/launchd restart ownership
- `AGENTD_OTA_RESTART_DRY_RUN=1` for restart smokes/proofs only

If OTA is not enabled or the command is missing, the update endpoint returns an
error. `POST /api/v1/ota/restart` does not require an update command, but it
still requires OTA to be enabled and requires a supervisor restart mode of
`systemd` or `launchd` with a safe `AGENTD_OTA_SERVICE`.

## OTA Plan File

Agentd writes a plan file under the state dir so that the update can be audited or re-run if needed:

`<state_dir>/ota/pending.json`

The plan includes:

- `ota_id`, `operation`, `version`, `url`, `sha256`, `reason`,
  `requested_unix_ms`, `trace_id`, `idempotency_key`
- `agentd_pid`, `state_dir`, `db_path`
- `status` (queued/running/error/done)

## Execution Command Contract

The OTA command is invoked by agentd with **environment variables**:

- `AGENTD_OTA_PLAN_PATH` — absolute path to the plan JSON.
- `AGENTD_OTA_VERSION`, `AGENTD_OTA_URL`, `AGENTD_OTA_SHA256`
- `AGENTD_OTA_STATE_DIR`, `AGENTD_OTA_DB_PATH`
- `AGENTD_OTA_PID` — current agentd PID (for graceful stop if needed).

A reference implementation is provided at `tools/agentd_ota_apply.sh`.

## WebUI / Broker Flow

The WebUI uses broker **deployments + bulk fan-out** to update multiple deployments in one action:

- `GET /v1/agents/{agent_id}/deployments` to list connected deployments.
- `POST /v1/agents/{agent_id}/ota/update` with `deployment_ids=[...]` to trigger OTA across selected deployments.
- `GET /v1/agents/{agent_id}/ota/status` with optional `deployment_ids=...` to watch drain/rollout state.
- Proxy fallback per deployment remains supported:
  - `POST /v1/agents/{agent_id}/proxy/api/v1/ota/update` with `X-Agentd-Deployment`.

The local daemon also exposes `POST /api/v1/ota/restart` for broker
`runtime.restart`. That endpoint writes a restart plan, enters the same drain
boundary as update, waits for in-flight work up to `drain_timeout_ms`, and then
asks the configured supervisor to restart the service. The codexw connector may
advertise `runtime.restart` only when `/api/v1/ota/status` reports
`restart.enabled=true` and safe boundary `agentd_supervisor_restart_drain`.

This leverages broker auth + audit trail and keeps multi-deployment updates consistent.

## Task Continuity After OTA

- **Async jobs** (`/api/v1/run_async`) are persisted and resumed by `JobEngine` after restart.
- **Workflows** are persisted and resumed by `WorkflowEngine` after restart.
- During OTA, agentd enters **drain mode**:
  - New run/workflow submissions are rejected with `HTTP 503` + `drain_*` hints.
  - Job/workflow schedulers pause claiming new tasks.
- OTA uses a **grace period** (drain timeout) and **waits for running jobs/workflow tasks** to complete before restart
  (best-effort, bounded by `drain_timeout_ms`). Any remaining in-flight work is replayed by the engines after restart.

## Observability

- OTA requests are logged with `trace_id` when provided.
- Plan/status files are stored under `state_dir/ota/` for audit/debug.
- Broker audit trail captures OTA proxy requests.
- `GET /api/v1/ota/status` returns best-effort inflight counts when DB is available
  (`jobs_running`, `jobs_queued`, `workflow_tasks_running`, `workflow_tasks_queued`, `workflows_running`).
- `GET /api/v1/ota/status` returns a `restart` object describing whether
  supervisor restart is available, enabled, dry-run, and bound to the safe
  `agentd_supervisor_restart_drain` policy.
