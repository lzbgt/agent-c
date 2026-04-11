#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

WORKFLOW_DRR_COST_MODEL=budget_pressure_v1 \
WORKFLOW_DRR_SMOKE_NAME=agentd_workflow_drr_budget_pressure_smoke \
  exec bash "${SCRIPT_DIR}/agentd_workflow_drr_cost_telemetry_smoke.sh" "$@"
