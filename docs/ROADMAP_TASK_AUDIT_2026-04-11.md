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

## Directly Open Roadmap Items

`TODOS.md` now has three open roadmap workstreams, with concrete unchecked subitems under each:

1. `TODOS.md:516` - Streaming stability umbrella.
   - Concrete remaining subitems:
   - `TODOS.md:524` - Populate OpenRouter streaming pins with a verified key; the currently available key is documented as returning `401 User not found` during chat preflight.
   - `TODOS.md:525` - Re-verify live-provider usage/variant coverage after a working OpenRouter key exists, then commit the resulting pins file.
   - Priority: high, but blocked on valid OpenRouter credentials unless a new provider/pin source is supplied.

2. `TODOS.md:547` - Audio streaming hardening after the shipped agentd-native WebRTC full-duplex native-plugin proof.
   - The roadmap now treats native media as shipped beyond signaling/control glue and narrows the remaining work to production hardening.
   - 2026-04-11 follow-up: the compatibility-coverage subitem is now covered with deterministic fixtures for unsupported Opus channel/rate variants, unmapped dynamic RTP payloads, codec-name case normalization, and ICE relay-versus-srflx/prflx candidate handling. The outbound RTP encoder now normalizes selected codec names before storing them.
   - 2026-04-11 follow-up: outbound audio payload negotiation is now extracted from the embedded provider into `session_voice_builtin_audio_payload.*` with unit coverage; the provider still loads through the same `native_plugin` ABI.
   - Concrete remaining subitems: the next provider-surface extraction slice, now likely progress-event/status JSON or RTP/RTCP packet accounting, and production graduation criteria for `runtime_kind=builtin` + `audio_webrtc.builtin_mode=native_plugin` versus the bundled browser peer path.

3. `TODOS.md:700` - Node consensus firmware-native adoption on top of portable `agent_core` consensus rules and managed runtime.
   - The roadmap now distinguishes shipped host-side relay/runtime and portable consensus rules from the remaining adoption proof.
   - Concrete remaining subitems: a firmware-style reference loop or fixture using portable `agent_core` helpers without depending on the host C++ replica, a lossy-transport replay fixture for consensus frames and membership bundles, and documentation of the firmware adoption contract.

## Secondary Backlog Signals

The older P0/P1 section still contains "Next" and "Remaining" notes that may be partially stale but should be triaged:

- Budgets / scheduling:
  - Budget-pressure-aware DRR charging and resilient mixed-workload fairness remain listed after shipped `telemetry_v1`.
  - Stress proof for bounded completion/fairness under deterministic stub load remains useful.

- Agent collaboration:
  - Remaining notes mention enforceable per-attempt budgets, richer joins such as `strict_all_ok`, node-identity-aware quorum votes, broker-routed target discovery/routing policy, and identity-scoped memory.

- Memory:
  - Remaining notes mention all-layer time/size consolidation, versioned facts with `supersedes` / evidence arrays, a correlation graph linked to trace/workflow/job IDs, deterministic ranking tests, and structured conflict-resolution tests.

- Workflow / correctness:
  - Remaining notes mention workflow timeline UI filters, richer aggregation strategies, expectation validators beyond JSON pointer, and replay-mode workflow validation under stub providers.

- Tool servers:
  - Remaining P1 note: reference tool server for ESP32 serial/MQTT bridges that speaks the strict stdio protocol and advertises UM-ACDS tool schemas.

## Maintainability Signals

Line-count scan excluding `build/`, `ui/node_modules/`, `external/`, `ref/`, `refs/`, `docs/research/`, and generated OpenAPI types found one non-generated file over 2000 lines:

- `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` - 4412 lines

The scan did not identify a production implementation file over the 2000-line threshold after excluding generated OpenAPI typings.

## Recommended Next Work

Highest leverage sequence:

1. Fix or unblock OpenRouter credentials, run the OpenRouter streaming pin workflow, and commit `ref/openrouter/streaming_pins.json` if the result is stable.
2. Continue the narrowed audio hardening work with the next provider-surface extraction slice, then builtin-vs-bundled graduation criteria.
3. Make node consensus firmware-native adoption concrete with a minimal embedded-style loop/fixture plus lossy-transport replay proof and a short contract doc.
4. Refactor `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` into smaller helper scripts or fixtures to reduce maintenance risk around the active media lane.
5. Prune stale P0/P1 "Next" notes into current, testable checklist items so the roadmap reflects shipped work instead of duplicating older goals.
