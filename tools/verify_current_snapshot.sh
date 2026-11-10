#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
MODE="${1:-full}"
if [[ "${MODE}" != "full" && "${MODE}" != "--core-only" ]]; then
  echo 'usage: tools/verify_current_snapshot.sh [--core-only]' >&2
  exit 2
fi
if [[ "${MODE}" == "--core-only" ]]; then
  SNAPSHOT_BUILD="${AGENT_SNAPSHOT_BUILD_DIR:-${ROOT}/build-snapshot-core}"
  cmake -S . -B "${SNAPSHOT_BUILD}" -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=OFF
  cmake --build "${SNAPSHOT_BUILD}" --target agent_core_tests -j "${AGENT_SNAPSHOT_JOBS:-4}"
  ctest --test-dir "${SNAPSHOT_BUILD}" -R '^agent_core_tests$' --output-on-failure
  exit 0
fi
pkg-config --exists sqlite3 jsoncpp || { echo 'snapshot host verification requires sqlite3 and jsoncpp development packages' >&2; exit 1; }
SNAPSHOT_BUILD="${AGENT_SNAPSHOT_BUILD_DIR:-${ROOT}/build-snapshot}"
cmake -S . -B "${SNAPSHOT_BUILD}" -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build "${SNAPSHOT_BUILD}" --target agent agentd agent_core_tests work_ledger_tests workflow_sop_tests workflow_effect_tests agent_db_tests host_session_prompts_tests host_toolset_tests docs_sanity_tests -j "${AGENT_SNAPSHOT_JOBS:-4}"
ctest --test-dir "${SNAPSHOT_BUILD}" -R '^(agent_core_tests|work_ledger_tests|workflow_sop_tests|workflow_effect_tests|agent_db_tests|agentd_workflow_effect_recovery_smoke|host_session_prompts_tests|host_toolset_tests|docs_sanity_tests|cli_harness_smoke|cli_machine_output_smoke|agentd_workflow_sop_smoke|agent_local_stream_tool_loop_smoke|agent_local_stream_assistant_smoke|agentd_workflow_idempotency_smoke|agentd_workflow_replay_spec_smoke|agentd_workflow_http_json_smoke|agentd_workflow_http_allowlist_smoke|agentd_workflow_http_dns_pin_smoke|agentd_workflow_http_deny_cidr_smoke)$' --output-on-failure
npm ci --prefix ui --no-audit --no-fund
npm run openapi:types:check --prefix ui
npm run build --prefix ui
(
  cd broker
  go test ./...
)
(
  cd ui
  AGENT_E2E_PREFER_DEVSTACK=0 AGENT_E2E_RECORD="${AGENT_E2E_RECORD:-0}" npx playwright test \
    e2e/workspace_shell.spec.ts e2e/workflow_detail_panel.spec.ts e2e/prompt_bar.spec.ts e2e/scene_view.spec.ts e2e/deployment_auth.spec.ts \
    e2e/workflow_graph_editor.spec.ts e2e/workflow_wait_persisted_ui.spec.ts e2e/workflow_schedules.spec.ts e2e/broker_team_layout.spec.ts \
    --workers=2 --timeout=45000
)
git diff --check
