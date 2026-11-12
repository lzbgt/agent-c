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
cmake --build "${SNAPSHOT_BUILD}" --target agent agentd agentd_tool_plugin_echo agent_core_tests work_ledger_tests workflow_sop_tests workflow_sop_registry_tests workflow_decision_tests workflow_model_effect_tests workflow_host_tool_tests workflow_effect_tests workflow_budget_tests device_assignment_tests agent_db_tests host_session_prompts_tests host_toolset_tests docs_sanity_tests -j "${AGENT_SNAPSHOT_JOBS:-4}"
ctest --test-dir "${SNAPSHOT_BUILD}" -R '^(agent_core_tests|work_ledger_tests|workflow_sop_tests|workflow_sop_registry_tests|agentd_workflow_sop_registry_smoke|workflow_decision_tests|agentd_workflow_decision_smoke|workflow_model_effect_tests|agentd_workflow_model_effect_recovery_smoke|workflow_host_tool_tests|agentd_workflow_host_tool_recovery_smoke|workflow_effect_tests|workflow_budget_tests|workflow_budget_parallel_smoke|device_assignment_tests|agent_db_tests|agentd_workflow_effect_recovery_smoke|host_session_prompts_tests|host_toolset_tests|docs_sanity_tests|cli_harness_smoke|cli_machine_output_smoke|agentd_workflow_sop_smoke|agent_local_stream_tool_loop_smoke|agent_local_stream_assistant_smoke|agentd_workflow_idempotency_smoke|agentd_workflow_replay_spec_smoke|agentd_workflow_http_json_smoke|agentd_workflow_http_allowlist_smoke|agentd_workflow_http_dns_pin_smoke|agentd_workflow_http_deny_cidr_smoke)$' --output-on-failure
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
    e2e/workspace_shell.spec.ts e2e/workflow_sop_library.spec.ts e2e/workflow_decisions.spec.ts e2e/workflow_detail_panel.spec.ts e2e/prompt_bar.spec.ts e2e/scene_view.spec.ts e2e/scene_scope.spec.ts e2e/deployment_auth.spec.ts \
    e2e/workflow_graph_editor.spec.ts e2e/workflow_wait_persisted_ui.spec.ts e2e/workflow_schedules.spec.ts e2e/broker_team_layout.spec.ts \
    e2e/memory_panel.spec.ts e2e/tool_focus.spec.ts e2e/voice_panel.spec.ts e2e/run_diff_panel.spec.ts e2e/approval_queue_panel.spec.ts e2e/broker_trace_lookup.spec.ts \
    e2e/chat_focus.spec.ts e2e/conversation_client_rpc.spec.ts e2e/conversation_ui_action_ack.spec.ts e2e/conversation_rpc_script_eval.spec.ts \
    e2e/broker_codexw_session_lease.spec.ts e2e/broker_codexw_session_events_resume.spec.ts e2e/broker_codexw_session_operators.spec.ts e2e/broker_codexw_artifact_boundary.spec.ts \
    --workers=2 --timeout=45000
)
git diff --check
