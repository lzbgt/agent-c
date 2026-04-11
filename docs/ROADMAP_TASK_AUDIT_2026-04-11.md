# Agent Project Task Audit - 2026-04-11

Scope: local repository at `/Users/zongbaolu/work/agent` on macOS M2.

Repository state checked:
- Branch: `master`
- Baseline HEAD before the follow-up compatibility slice: `aa5456b5 docs: narrow active roadmap tasks`
- Existing untracked paths before this audit included `.codex-autopilot/`, `.config/`, `Professional_Resume_Lu_Zongbao_Chinese.md`, `external/`, `image.png`, and `refs/`.

Verification performed:
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/task_audit_build_20260411.log 2>&1`
  - Result: `build_rc=0`
- `ctest --test-dir build -N > build/task_audit_ctest_inventory_20260411.log 2>&1`
  - Result: `ctest_inventory_rc=0`
  - Inventory: 312 tests
- `tools/verify_repo_guards.sh > build/task_audit_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`

Follow-up compatibility slice verification:
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/audio_compat_build_20260411.log 2>&1`
  - Result: `build_rc=0`
- `ctest --test-dir build -R 'session_voice_(builtin_sdp_answer|sdp_candidate|signal_session|audio_decode|audio_encode)_tests' --output-on-failure > build/audio_compat_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; five targeted session-voice tests passed.
- `tools/verify_repo_guards.sh > build/audio_compat_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`
- `git diff --check`
  - Result: clean.

Follow-up provider-surface slice verification:
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/audio_payload_helper_build_20260411.log 2>&1`
  - Result: `build_rc=0`
- `ctest --test-dir build -R 'session_voice_(builtin_audio_payload|builtin_embedded_transport_provider|audio_encode|audio_decode)_tests' --output-on-failure > build/audio_payload_helper_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; four targeted session-voice tests passed, including the embedded transport provider loader/answer coverage.
- `tools/verify_repo_guards.sh > build/audio_payload_helper_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`
- `git diff --check`
  - Result: clean.

Follow-up embedded-status slice verification:
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/embedded_status_helper_build_20260411.log 2>&1`
  - Result: `build_rc=0`
- `ctest --test-dir build -R 'session_voice_(builtin_embedded_status|builtin_embedded_transport_provider|builtin_audio_payload)_tests' --output-on-failure > build/embedded_status_helper_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; three targeted session-voice tests passed, including embedded status formatting and embedded transport provider coverage.
- `tools/verify_repo_guards.sh > build/embedded_status_helper_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`
- `git diff --check`
  - Result: clean.

Follow-up packet-accounting slice verification:
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/packet_accounting_build_20260411.log 2>&1`
  - Result: `build_rc=0`
- `ctest --test-dir build -R 'session_voice_(builtin_packet_accounting|builtin_embedded_status|builtin_embedded_transport_provider|builtin_audio_payload)_tests' --output-on-failure > build/packet_accounting_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; four targeted session-voice tests passed, including packet accounting, embedded status formatting, payload negotiation, and embedded provider load/status coverage.

Follow-up native-plugin graduation gate verification:
- `python3 tools/check_voice_native_media_stack.py --pretty > build/native_plugin_graduation_stack_20260411.json 2> build/native_plugin_graduation_stack_20260411.err`
  - Result: `ok=true`; stderr empty.
- `python3 tools/inspect_voice_media_provider.py build/libagentd_voice_builtin_media_engine_embedded_transport.so --pretty > build/native_plugin_graduation_provider_20260411.json 2> build/native_plugin_graduation_provider_20260411.err`
  - Result: `ok=true`; ABI v5; provider `agentd_builtin_embedded_transport_provider` version `0.13.0`; required callbacks present; `native_media_supported=true`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/native_plugin_graduation_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'session_voice_(builtin_packet_accounting|builtin_embedded_status|builtin_embedded_transport_provider|builtin_audio_payload|builtin_sdp_answer|sdp_candidate|audio_decode|audio_encode)_tests' --output-on-failure > build/native_plugin_graduation_unit_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; eight targeted native-plugin unit/provider tests passed.
- `ctest --test-dir build -R 'session_voice_builtin_dtls_transport_tests|session_voice_rtcp_report_tests' --output-on-failure > build/native_plugin_graduation_transport_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; DTLS/SRTP transport and RTCP report tests passed.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_builtin_native_plugin_smoke|agentd_session_voice_webrtc_peer_builtin_default_native_plugin_smoke|agentd_session_voice_webrtc_peer_runtime_smoke|agentd_audio_webrtc_peer_smoke' --output-on-failure > build/native_plugin_graduation_smoke_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; four runtime/browser smokes passed with no skips.

Follow-up firmware-consensus adoption verification:
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/firmware_consensus_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agent_core_edge_consensus_firmware_loop_tests|agent_core_tests|edge_node_consensus_loop_tests|edge_consensus_runtime_loop_adapter_tests|edge_node_consensus_tests' --output-on-failure > build/firmware_consensus_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; five portable-core and host-loop consensus tests passed, including the new firmware-style fixture.

Follow-up WebRTC runtime smoke maintainability slice:
- Extracted the embedded Playwright receiver peer from `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` into `tests/lib/agentd_voice_webrtc_peer_receiver.js`.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_receiver_extract_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_receiver_extract_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after the extraction.

Follow-up WebRTC runtime smoke fixture-record slice:
- Extracted stale/corrupt/planned runtime record seeding from `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` into `tests/lib/agentd_voice_webrtc_runtime_record.py`.
- Removed 172 generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` after CTest could not write `LastTestsDisabled.log` with the volume at 125 MiB free.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_record_helper_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_record_helper_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after pruning generated build artifacts and rerunning.

Follow-up WebRTC runtime smoke assertion-helper slice:
- Moved repeated stale/corrupt runtime self-heal DB/artifact assertions into `tests/lib/agentd_voice_webrtc_runtime_record.py` via `assert-cleared`.
- Added a shell-local `url_quote` helper so the smoke no longer repeats inline `urllib.parse.quote` one-liners at each session-status/delete call site.
- Removed 7 generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` before the focused smoke, then removed the 7 files regenerated by that smoke after verification.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_assert_helper_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_assert_helper_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after the assertion-helper extraction.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_assert_helper_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke session-create helper slice:
- Moved the repeated authenticated `POST /api/v1/session/new` request shape into a shell-local `create_session` helper and rewired 30 exact call sites.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_create_session_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_create_session_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after the session-create helper extraction.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_create_session_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke API-helper slice:
- Moved repeated daemon API request shapes into shell-local `delete_session`, `delete_session_quiet`, `voice_peer_status`, `voice_peer_request`, `voice_peer_request_status`, and `config_update` helpers.
- Removed 13 generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` before the final focused smoke.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_api_helpers_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_api_helpers_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after the daemon API helper extraction.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_api_helpers_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke config-helper slice:
- Moved repeated daemon config GET/update request shapes into shell-local `config_get` and `config_update` helpers.
- Removed 7 generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` before the focused smoke.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_config_helpers_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_config_helpers_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after the daemon config helper extraction.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_config_helpers_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke broker-client helper slice:
- Moved direct audio-broker session create/get/status/delete/signal request shapes into `tests/lib/agentd_voice_webrtc_broker_client.sh` and rewired the runtime smoke to source it.
- Removed generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` before the focused smoke reruns.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && bash -n tests/lib/agentd_voice_webrtc_broker_client.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_broker_client_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_broker_client_ctest_retry_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed after pruning generated build artifacts and rerunning.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_broker_client_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke daemon-client helper slice:
- Moved daemon session/config/voice peer request helpers into `tests/lib/agentd_voice_webrtc_daemon_client.sh` and rewired the runtime smoke to source it.
- Removed generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` before and after the focused smoke rerun.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && bash -n tests/lib/agentd_voice_webrtc_broker_client.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_client.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_daemon_client_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_daemon_client_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed in 48.01 seconds after the daemon-client helper extraction.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_daemon_client_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke lifecycle-helper and receiver-shutdown slice:
- Moved daemon start/restart/readiness helpers into `tests/lib/agentd_voice_webrtc_daemon_lifecycle.sh` and rewired the runtime smoke to source it.
- Fixed the Playwright receiver helper to wait for the aborted broker signal stream before closing Chromium, avoiding a queued `page.evaluate` call against a closed page during successful shutdown.
- Removed generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` after the focused smoke rerun.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && bash -n tests/lib/agentd_voice_webrtc_broker_client.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_client.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_lifecycle.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_lifecycle_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_lifecycle_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed in 28.90 seconds after the lifecycle-helper extraction and receiver shutdown-order fix.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_lifecycle_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke runtime-helper slice:
- Moved peer-ready polling, broker-session deletion polling, stale runtime record seeding, and Playwright receiver launch helpers into `tests/lib/agentd_voice_webrtc_runtime_helpers.sh`.
- Removed generated smoke-test SQLite/WAL/SHM files and broker binaries from `build/` before the focused smoke retry.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && bash -n tests/lib/agentd_voice_webrtc_broker_client.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_client.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_lifecycle.sh && bash -n tests/lib/agentd_voice_webrtc_runtime_helpers.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_helpers_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_helpers_ctest_retry_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed in 28.75 seconds on the clean retry after pruning generated smoke artifacts.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_helpers_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.

Follow-up WebRTC runtime smoke fixture/assertion helper slice:
- Moved Postgres + audio-broker fixture setup/cleanup into `tests/lib/agentd_voice_webrtc_broker_fixture.sh`.
- Moved per-run broker/response logs under `build/agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}/`, reused the daemon-client status-body helper for the remaining status-code `voice_webrtc_peer` request paths, and explicitly truncates status body files before each `curl -o` write to avoid stale suffix bytes in reused smoke artifacts.
- Moved the large builtin contract, borrowed-broker contract, start-error, and no-runtime assertions into `tests/lib/agentd_voice_webrtc_runtime_assertions.py`.
- Removed generated smoke-test SQLite/WAL/SHM files, broker binaries, stale response-body files, and Python `__pycache__` files during verification cleanup.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && bash -n tests/lib/agentd_voice_webrtc_broker_fixture.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_client.sh && bash -n tests/lib/agentd_voice_webrtc_broker_client.sh && bash -n tests/lib/agentd_voice_webrtc_daemon_lifecycle.sh && bash -n tests/lib/agentd_voice_webrtc_runtime_helpers.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_record.py tests/lib/agentd_voice_webrtc_runtime_assertions.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_fixture_assertions_build_retry_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_fixture_assertions_ctest_retry2_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed in 26.09 seconds in a fresh log after pruning generated response/body artifacts.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_fixture_assertions_repo_guards_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.
- `ctest --test-dir build -N > build/webrtc_runtime_fixture_assertions_ctest_inventory_20260411.log 2>&1`
  - Result: `ctest_inventory_rc=0`; the test inventory still contains 316 tests.

Follow-up WebRTC runtime smoke JSON assertion helper slice:
- Added reusable `print-field`, `assert-fields`, `assert-started`, and `assert-stopped` helpers to `tests/lib/agentd_voice_webrtc_runtime_assertions.py`, with shell wrappers in `tests/lib/agentd_voice_webrtc_runtime_helpers.sh`.
- Rewired repeated broker-session/stdout/pid extraction, start-response assertions, stop-response assertions, and JSON field/status/body assertions in `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` through those helpers. The remaining inline Python blocks now stay limited to custom scenario logic, SQLite mutation, or filesystem artifact checks.
- Removed generated smoke-test SQLite/WAL/SHM files and per-run response directories before the authoritative focused smoke run.
- `bash -n tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh && bash -n tests/lib/agentd_voice_webrtc_runtime_helpers.sh && python3 -m py_compile tests/lib/agentd_voice_webrtc_runtime_assertions.py tests/lib/agentd_voice_webrtc_runtime_record.py && node --check tests/lib/agentd_voice_webrtc_peer_receiver.js`
  - Result: `syntax_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/webrtc_runtime_json_helpers_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_runtime_smoke' --output-on-failure > build/webrtc_runtime_json_helpers_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; the affected runtime smoke passed in 36.67 seconds after replacing the repeated response/extraction assertions.
- `tools/verify_repo_guards.sh > build/webrtc_runtime_json_helpers_repo_guards_final_20260411.log 2>&1`
  - Result: `repo_guards_rc=0`.
- `ctest --test-dir build -N > build/webrtc_runtime_json_helpers_ctest_inventory_20260411.log 2>&1`
  - Result: `ctest_inventory_rc=0`; the test inventory still contains 316 tests.

Follow-up roadmap pruning and DRR budget-pressure slice:
- Pruned stale P0/P1 `Next` / `Remaining` notes that duplicated later shipped roadmap items, and narrowed the next unblocked scheduler work to `budget_pressure_v1` follow-up proof.
- Added `--workflow-drr-cost-model budget_pressure_v1`, which combines telemetry/request DRR estimates with bounded pressure bumps from workflow limits and retry-safe usage totals.
- `bash -n tests/agentd_workflow_drr_cost_telemetry_smoke.sh && bash -n tests/agentd_workflow_drr_budget_pressure_smoke.sh && git diff --check`
  - Result: `syntax_and_diff_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/drr_budget_pressure_final_build_20260412.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'workflow_fairq_cost_tests|agentd_workflow_drr_(cost_telemetry|budget_pressure)_smoke|agentd_workflow_stats_budget_pressure_smoke' --output-on-failure > build/drr_budget_pressure_final_ctest_20260412.log 2>&1`
  - Result: `ctest_rc=0`; four targeted fair-queue / budget-pressure tests passed.
- `tools/verify_repo_guards.sh > build/drr_budget_pressure_final_repo_guards_20260412.log 2>&1`
  - Result: `repo_guards_rc=0`.
- `ctest --test-dir build -N > build/drr_budget_pressure_final_ctest_inventory_20260412.log 2>&1`
  - Result: `ctest_inventory_rc=0`; the test inventory now contains 317 tests.

Follow-up DRR budget-pressure mixed-fairness smoke:
- Added a live `budget_pressure_v1` CTest smoke that submits two DRR sessions against a local OpenAI-compatible stub and proves interleaving across deterministic host work, LLM-like work, streaming-like work, and an edge sensor poll loop after the pressure workflow has consumed most of its token budget.
- `bash -n tests/agentd_workflow_drr_budget_pressure_mixed_fairness_smoke.sh && git diff --check`
  - Result: `syntax_and_diff_rc=0`.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/workflow_mixed_fairness_smoke_build_20260412.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'workflow_fairq_cost_tests|agentd_workflow_drr_cost_telemetry_smoke|agentd_workflow_drr_budget_pressure_smoke|agentd_workflow_drr_budget_pressure_mixed_fairness_smoke|agentd_workflow_stats_budget_pressure_smoke|agentd_workflow_budget_host_tool_memory_put_smoke' --output-on-failure > build/workflow_mixed_fairness_smoke_ctest_final_20260412.log 2>&1`
  - Result: `ctest_rc=0`; six targeted fair-queue / budget-pressure tests passed.
- `tools/verify_repo_guards.sh > build/workflow_mixed_fairness_smoke_repo_guards_20260412.log 2>&1`
  - Result: `repo_guards_rc=0`.
- `ctest --test-dir build -N > build/workflow_mixed_fairness_smoke_ctest_inventory_20260412.log 2>&1`
  - Result: `ctest_inventory_rc=0`; the test inventory now contains 318 tests.

Follow-up provider-backed streaming token budget slice:
- Added `workflow_run_budget_clamp.*` so workflow remaining budgets are applied consistently to direct run tasks and delegate attempts.
- Workflow `max_total_tokens` now clamps provider requests with `max_completion_tokens` when no per-run cap exists, clamps existing `max_completion_tokens` downward, and preserves/clamps legacy `max_tokens` when that was the caller's existing field.
- Added request-builder support for the caps across the high-level OpenAI client, text-only provider, multimodal raw provider, streaming tool provider, and direct `tools:"none"` streaming path.
- The direct `tools:"none"` streaming path now requests `stream_options.include_usage=true` and retries once without `stream_options` on compatible "unknown/unrecognized field" errors, matching the tool-provider path.
- Successful streams without provider usage now add an estimated `llm_usage` event so retry-safe workflow budget counters still advance; this is a guardrail, not billing-grade token accounting.
- Usage aggregation now reconciles fallback estimates against later authoritative provider usage for the same tool-loop step/epoch, direct-run attempt, or legacy run-level bucket, avoiding double-charge if a provider exposes delayed/out-of-band usage.
- Added `agentd_workflow_budget_tokens_stream_fallback_smoke` to force `stream_options` rejection, a transient 429 retry, missing provider usage, and `max_completion_tokens` enforcement on the fallback/retry request.
- Verification:
  - `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/workflow_stream_fallback_build_retry_20260412.log 2>&1` passed.
  - `ctest --test-dir build -R 'workflow_run_budget_clamp_tests|agentd_workflow_budget_tokens_stream_fallback_smoke|agentd_workflow_budget_tokens_stream_smoke|agentd_workflow_budget_tokens_smoke|agentd_workflow_budget_retry_charges_smoke|openai_tool_provider_stream_tests|openai_provider_tests' --output-on-failure > build/workflow_stream_fallback_ctest_retry_20260412.log 2>&1` passed, 7/7.
  - `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/llm_usage_reconcile_build_20260412.log 2>&1` passed.
  - `ctest --test-dir build -R 'workflow_run_budget_clamp_tests|agentd_workflow_budget_tokens_stream(_fallback)?_smoke|agentd_workflow_budget_tokens_smoke|agentd_workflow_budget_retry_charges_smoke|openai_tool_provider_stream_tests' --output-on-failure > build/llm_usage_reconcile_ctest_20260412.log 2>&1` passed, 6/6.
  - `tools/verify_repo_guards.sh > build/workflow_stream_fallback_repo_guards_20260412.log 2>&1` passed.
  - `ctest --test-dir build -N > build/workflow_stream_fallback_ctest_inventory_20260412.log 2>&1` passed; inventory is now 320 tests.

Follow-up agent collaboration routing/memory slice:
- Added broker deployment routing metadata to `agentd_call.broker_proxy` and `agentd_parallel.targets[].broker_proxy`; `deployment_id` now injects `X-Agentd-Deployment` and is persisted as `agentd.broker_deployment_id`.
- Added stable `agentd.target_identity` result metadata and changed `agentd_parallel` quorum-hash default node identity from `/agentd/base_url` to `/agentd/target_identity`, so direct URL targets and broker agent/deployment targets share one correctness surface.
- Added `agentd_parallel.routing_policy.require_distinct_targets` to reject duplicate derived target identities at submit time.
- Added `agentd_parallel.memory_scope` to inject per-target or shared remote workflow `memory_scope_id` / `memory_scope_mode`, plus target identity inputs, before the derived remote workflow is submitted.
- Added `agentd_workflow_agentd_parallel_broker_routing_memory_scope_smoke` to exercise duplicate broker-target rejection, broker deployment header routing, target metadata persistence, and per-target remote memory-scope injection.
- Added `agentd_parallel.targets_from_broker_registry` to snapshot `GET /v1/agents` at submit time under the workflow HTTP outbound policy and expand connected/enabled broker agents or deployments into concrete broker-proxy targets.
- Verification:
  - `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/agentd_parallel_target_identity_build_20260412.log 2>&1` passed.
  - `ctest --test-dir build -R 'agentd_workflow_agentd_parallel_(macro|quorum_hashes|quorum_hashes_default_pointers|distinct_nodes|broker_routing_memory_scope)_smoke' --output-on-failure > build/agentd_parallel_target_identity_ctest_20260412.log 2>&1` passed, 5/5.
  - `tools/verify_repo_guards.sh > build/agentd_parallel_target_identity_repo_guards_20260412.log 2>&1` passed.
  - `ctest --test-dir build -N > build/agentd_parallel_target_identity_ctest_inventory_20260412.log 2>&1` passed; inventory is now 321 tests.
  - `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/agentd_parallel_broker_registry_build_20260412.log 2>&1` passed.
  - `ctest --test-dir build -R 'agentd_workflow_agentd_parallel_(broker_routing_memory_scope|quorum_hashes_default_pointers|distinct_nodes|macro|quorum_hashes)_smoke|agentd_workflow_agentd_call_broker_proxy_smoke' --output-on-failure > build/agentd_parallel_broker_registry_ctest_20260412.log 2>&1` passed, 6/6.
  - `tools/verify_repo_guards.sh > build/agentd_parallel_broker_registry_repo_guards_20260412.log 2>&1` passed.
  - `ctest --test-dir build -N > build/agentd_parallel_broker_registry_ctest_inventory_20260412.log 2>&1` passed; inventory is still 321 tests.

Follow-up memory ranking/conflict proof slice:
- Added deterministic host-tool coverage for FTS5-ranked `memory_search` order across core/session candidates.
- Added structured memory current-view query coverage for latest-value conflict resolution, current evidence, and retained superseded version evidence.
- Verification:
  - `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/memory_rank_conflict_tests_build_20260412.log 2>&1` passed.
  - `ctest --test-dir build -R 'host_toolset_tests|agentd_workflow_memory_(put|search|query|structured_query)_smoke|agentd_workflow_agentd_parallel_(macro|quorum_hashes|quorum_hashes_default_pointers|distinct_nodes|broker_routing_memory_scope)_smoke' --output-on-failure > build/memory_rank_conflict_tests_ctest_20260412.log 2>&1` passed, 10/10.
  - `ctest --test-dir build -R 'openapi_sanity_tests|docs_sanity_tests' --output-on-failure > build/broker_registry_openapi_docs_ctest_20260412.log 2>&1` passed, 2/2.
  - `tools/verify_repo_guards.sh > build/memory_rank_conflict_tests_repo_guards_20260412.log 2>&1` passed.
  - `ctest --test-dir build -N > build/memory_rank_conflict_tests_ctest_inventory_20260412.log 2>&1` passed; inventory is still 321 tests.

Follow-up workflow expectation schema slice:
- Extracted the edge/tool-parameter JSON Schema subset validator into `json_schema_subset.*` and reused it for workflow `expect.json_pointer_schema`.
- Extended `agentd_workflow_expect_extended_smoke` so one task passes root and pointer-level schema checks while an `allow_error` branch proves deterministic schema mismatch handling.
- Reconciled the stale AVM audit note: `agentd_workflow_aggregate_quorum_smoke` already proves AVM `quorum_hashes` defaults `node_pointer` to `/avm/attest/node_id` and preserves `attestations_by_task_id`.

## Directly Open Roadmap Items

`TODOS.md` now has two open roadmap workstreams with concrete unchecked subitems, plus one newly closed roadmap workstream:

1. `TODOS.md:516` - Streaming stability umbrella.
   - Concrete remaining subitems:
   - `TODOS.md:524` - Populate OpenRouter streaming pins with a verified key; the currently available key is documented as returning `401 User not found` during chat preflight.
   - `TODOS.md:525` - Re-verify live-provider usage/variant coverage after a working OpenRouter key exists, then commit the resulting pins file.
   - Priority: high, but blocked on valid OpenRouter credentials unless a new provider/pin source is supplied.

2. `TODOS.md:547` - Audio streaming hardening after the shipped agentd-native WebRTC full-duplex native-plugin proof.
   - The roadmap now treats native media as shipped beyond signaling/control glue and narrows the remaining work to production hardening.
   - 2026-04-11 follow-up: the compatibility-coverage subitem is now covered with deterministic fixtures for unsupported Opus channel/rate variants, unmapped dynamic RTP payloads, codec-name case normalization, and ICE relay-versus-srflx/prflx candidate handling. The outbound RTP encoder now normalizes selected codec names before storing them.
   - 2026-04-11 follow-up: outbound audio payload negotiation is now extracted from the embedded provider into `session_voice_builtin_audio_payload.*` with unit coverage; the provider still loads through the same `native_plugin` ABI.
   - 2026-04-11 follow-up: embedded progress/status JSON construction is now extracted into `session_voice_builtin_embedded_status.*` with unit coverage; the provider file is down to 1725 lines while preserving the same `native_plugin` ABI.
   - 2026-04-11 follow-up: RTP/RTCP packet accounting and RTCP sender/receiver-report cadence decisions are now extracted into `session_voice_builtin_packet_accounting.*` with unit coverage; the provider file is down to 1620 lines while preserving embedded provider load/status coverage.
   - 2026-04-11 follow-up: production graduation criteria are now documented in `docs/VOICE_NATIVE_PLUGIN_GRADUATION.md`, including operator default levels, dependency probes, verification gates, fail-closed behavior, and bundled rollback rules.
   - 2026-04-11 follow-up: the documented native-plugin graduation gate passed on this macOS M2 workspace with native stack/provider probes, full build, native-plugin unit/provider tests, DTLS/SRTP + RTCP tests, and four non-skipped runtime/browser smokes. This makes builtin lab-default-eligible on this host; it does not by itself change global production defaults.
   - Concrete remaining subitem: execute the same non-skipped gate on any additional target release platforms before promoting builtin under auto/default policy outside this macOS M2 host.

3. `TODOS.md:700` - Node consensus firmware-native adoption on top of portable `agent_core` consensus rules and managed runtime.
   - Status: closed on 2026-04-11 by `agent_core_edge_consensus_firmware_loop_tests` and `docs/EDGE_CONSENSUS_FIRMWARE_ADOPTION.md`.
   - The fixture links only `agent_core`, includes only `agent/edge_interop.h` for production consensus rules, and avoids the host C++ replica implementation while proving duplicate/drop/reorder behavior for consensus frames and membership bundles.

## Secondary Backlog Signals

The older P0/P1 section was pruned on 2026-04-12 so `Next` / `Remaining` notes no longer restate work already marked shipped in the same roadmap:

- Budgets / scheduling:
  - Host-tool memory budget charging and workflow budget-pressure stats are already shipped.
  - 2026-04-12 follow-up: `budget_pressure_v1` DRR charging now combines telemetry/request estimates with bounded pressure bumps from workflow limits and retry-safe usage totals.
  - 2026-04-12 follow-up: the deterministic mixed-workload proof now covers host tasks, LLM-like tasks, streaming-like tasks, and edge poll loops in `workflow_fairq_cost_tests` and the live `agentd_workflow_drr_budget_pressure_mixed_fairness_smoke`.
  - 2026-04-12 follow-up: provider-backed streaming token budget enforcement now covers missing provider usage, `stream_options` compatibility fallback, transient provider retry, and delayed/out-of-band authoritative usage reconciliation.
  - Current remaining: edge-poll cost refinements only if larger trace replays show tail-latency gaps.

- Agent collaboration:
  - `strict_all_ok` and node-identity-aware quorum proof are already shipped in later collaboration slices.
  - 2026-04-12 follow-up: broker-routed deployment routing policy and identity-scoped remote memory injection are now covered by a live `agentd_parallel` CTest smoke.
  - 2026-04-12 follow-up: broker-routed target discovery from broker registry/list APIs is now covered by `agentd_parallel.targets_from_broker_registry`, which snapshots `GET /v1/agents` at submit time under the workflow HTTP outbound policy and expands matching connected/enabled agents or deployments into concrete broker-proxy targets.
  - Current remaining: no unblocked local collaboration macro gap; real-fleet broker validation depends on target broker credentials/environments.

- Memory:
  - Structured current-view key-prefix queries are already shipped through the memory query API and deterministic workflow `memory_query`.
  - 2026-04-12 follow-up: `host_toolset_tests` now covers deterministic FTS5-ranked `memory_search` order for index mode and structured conflict-resolution current-view queries with current evidence plus retained superseded versions/evidence.
  - Current remaining: all-layer time/size consolidation, versioned facts with explicit `supersedes` / `observed_utc` / `valid_from` metadata, and a durable correlation graph linked to trace/workflow/job IDs and source excerpts.

- Workflow / correctness:
  - 2026-04-12 follow-up: workflow `expect` now covers JSON pointer checks, regex, numeric bounds, schema subset checks, and tool-call constraints through shared deterministic validators.
  - Remaining notes mention workflow timeline UI filters, richer aggregation strategies, and replay-mode workflow validation under stub providers.

- Tool servers:
  - Remaining P1 note: reference tool server for ESP32 serial/MQTT bridges that speaks the strict stdio protocol and advertises UM-ACDS tool schemas.

- MCP:
  - Not an open product-extension roadmap item. Runtime extensibility remains centered on built-in tools, local plugins, and strict JSON-lines tool servers; any future MCP work should be a concrete edge adapter only, not a parallel plugin system.

## Maintainability Signals

Line-count scan excluding `build/`, `ui/node_modules/`, `external/`, `ref/`, `refs/`, `docs/research/`, and generated OpenAPI types no longer finds a non-generated file over 2000 lines.

- Former largest maintainability signal: `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` is now 1952 lines after the receiver-peer extraction plus runtime-record fixture/assertion, daemon client, daemon lifecycle, broker-client, runtime-helper, broker-fixture, runtime-assertion, and JSON assertion helper extractions. Extracted helpers:
  `tests/lib/agentd_voice_webrtc_runtime_assertions.py` at 629 lines, `tests/lib/agentd_voice_webrtc_peer_receiver.js` at 219 lines, `tests/lib/agentd_voice_webrtc_runtime_record.py` at 196 lines, `tests/lib/agentd_voice_webrtc_runtime_helpers.sh` at 168 lines, `tests/lib/agentd_voice_webrtc_broker_fixture.sh` at 149 lines, `tests/lib/agentd_voice_webrtc_daemon_lifecycle.sh` at 101 lines, `tests/lib/agentd_voice_webrtc_daemon_client.sh` at 78 lines, and `tests/lib/agentd_voice_webrtc_broker_client.sh` at 43 lines.

The scan did not identify a production implementation file over the 2000-line threshold after excluding generated OpenAPI typings.

## Recommended Next Work

Highest leverage sequence:

1. Fix or unblock OpenRouter credentials, run the OpenRouter streaming pin workflow, and commit `ref/openrouter/streaming_pins.json` if the result is stable.
2. Execute the documented builtin-vs-bundled graduation gate on any additional target release platforms before changing production defaults outside the checked macOS M2 host.
3. Re-scan the pruned P0/P1 backlog after the agent collaboration routing/memory slice; remaining unblocked local work is lower priority than credential-gated OpenRouter pins and cross-platform native-plugin graduation.
