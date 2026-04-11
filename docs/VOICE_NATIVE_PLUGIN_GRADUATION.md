# Voice Native-Plugin Graduation Criteria

Status: defined on 2026-04-11.

Latest local gate execution: passed on 2026-04-11 for the macOS M2 workspace at
`/Users/zongbaolu/work/agent`; see the "Gate Execution Log" section.

This document defines when `runtime_kind=builtin` with
`audio_webrtc.builtin_mode=native_plugin` can graduate from explicit opt-in to a
preferred managed WebRTC backend. It does not change the runtime default by
itself.

## Current Decision

The production-safe default remains the bundled browser peer unless an operator
explicitly selects the builtin backend through request, environment, or daemon
config:

- request: `runtime_kind=builtin`
- environment: `AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND=builtin`,
  `AGENTD_AUDIO_WEBRTC_BUILTIN_MODE=native_plugin`,
  `AGENTD_AUDIO_WEBRTC_BUILTIN_NATIVE_LIBRARY=/abs/path/to/provider`
- daemon config: `audio_webrtc.default_runtime_kind=builtin`,
  `audio_webrtc.builtin_mode=native_plugin`,
  `audio_webrtc.builtin_native_library_path=/abs/path/to/provider`

Bundled remains the fallback backend only when the operator selects it or when
auto/default policy still resolves to bundled. An explicit builtin request or
configured builtin default must fail closed if the native plugin is unavailable;
it must not silently launch the bundled browser peer.

## Graduation Levels

Level 0, preview:

- `native_plugin` is explicit opt-in only.
- Bundled remains the default for automatic backend selection.
- Failures expose `builtin_available=false`,
  `builtin_unavailable_reason`, and
  `default_runtime_kind_unavailable_reason` when relevant.

Level 1, lab default:

- A lab deployment may set `audio_webrtc.default_runtime_kind=builtin` only for
  hosts where the dependency and smoke gates below pass in the same deployment
  artifact.
- Bundled remains operator-selectable for rollback.
- Runtime status must show `peer.runtime_kind=builtin`,
  `peer.media_engine_kind=builtin_native_plugin`, and provider metadata under
  `peer.native_media_provider`.

Level 2, production preferred:

- The daemon may prefer builtin under auto/default policy only after all hard
  gates below pass on each supported platform and release artifact.
- Bundled remains an explicit compatibility backend until a separate removal
  decision is documented.
- Rollback must be config-only: changing `audio_webrtc.default_runtime_kind`
  back to `bundled` must restore the browser-peer path without rebuilding.

## Hard Gates

Build and dependency readiness:

- `AGENTD_ENABLE_BUILTIN_EMBEDDED_TRANSPORT_PROVIDER` is enabled for the release
  artifact.
- The embedded provider shared library is present:
  `libagentd_voice_builtin_media_engine_embedded_transport.{so,dylib,dll}`.
- `python3 tools/check_voice_native_media_stack.py --pretty` reports usable
  local `libjuice`, `libsrtp2`, `usrsctp`, OpenSSL DTLS-SRTP, and Opus status
  for the target host profile.
- `python3 tools/inspect_voice_media_provider.py <provider> --pretty` validates
  the exported ABI, provider metadata, required callbacks, and capabilities.

Config and fail-closed behavior:

- `GET /api/v1/config` reports
  `daemon.audio_webrtc.builtin_mode=native_plugin`,
  `builtin_native_library_path_configured=true`, and
  `builtin_native_probe.loadable=true` for the selected provider.
- `default_runtime_kind=builtin` reports
  `default_runtime_kind_available=true` only when the same provider is loadable.
- Missing, invalid, or ABI-incompatible provider paths report
  `builtin_available=false` and fail builtin starts without creating a bundled
  runtime as an implicit substitute.
- A live runtime with a conflicting backend policy still returns the existing
  `409` conflict semantics and exposes `backend_policy_drift`.

Signaling and lifecycle:

- Explicit `runtime_kind=builtin` starts and defaulted builtin starts both
  support broker auto-create and borrowed `broker_session_id` paths.
- Stop semantics clean up managed broker sessions and preserve stopped status
  across repeated status reads.
- Persisted config for builtin defaults survives an agentd restart and launches
  the same provider without environment variables.
- Stale, corrupt, or impossible builtin persisted runtime snapshots still
  self-heal through the existing cleanup paths instead of surfacing fake live
  runtimes.

Media behavior:

- The embedded provider reaches `peer.media_engine_state=media_active` during a
  real browser full-duplex smoke.
- Runtime telemetry shows `peer.native_media_supported=true` and
  `peer.native_media_active=true` after live RTP or RTCP is ingested or
  transmitted.
- Status includes inbound and outbound RTP packet/byte counters, last RTP
  header fields, RTCP packet counters, RTCP Sender Report counters, RTCP
  Receiver Report counters/report-block fields, decoded audio counters, and
  outbound audio counters.
- The browser smoke proves full-duplex media with supported Opus or G.711
  negotiation, including the unsupported-edge fixtures already pinned for
  channel/rate variants, unmapped dynamic payloads, mixed-case codec names,
  extra audio m-lines, non-audio m-lines, relay-versus-srflx/prflx candidates,
  and end-of-candidates normalization.
- The bundled browser peer smoke still passes as the rollback backend.

Verification gate:

- `cmake --build build -j "$(sysctl -n hw.ncpu)"` passes.
- `ctest --test-dir build -R 'session_voice_(builtin_packet_accounting|builtin_embedded_status|builtin_embedded_transport_provider|builtin_audio_payload|builtin_sdp_answer|sdp_candidate|audio_decode|audio_encode)_tests' --output-on-failure` passes.
- `ctest --test-dir build -R 'session_voice_builtin_dtls_transport_tests|session_voice_rtcp_report_tests' --output-on-failure` passes.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_builtin_native_plugin_smoke|agentd_session_voice_webrtc_peer_builtin_default_native_plugin_smoke|agentd_session_voice_webrtc_peer_runtime_smoke|agentd_audio_webrtc_peer_smoke' --output-on-failure` passes or reports an explicit skip reason for missing Playwright/browser dependencies; a production-preferred decision requires the non-skipped pass on the target release host.
- `tools/verify_repo_guards.sh` passes.

## Rollback Rule

Rollback must be operational, not code-only. A deployment that promotes builtin
must keep a documented way to set:

```json
{
  "audio_webrtc": {
    "default_runtime_kind": "bundled"
  }
}
```

and verify that new no-request `voice_webrtc_peer` starts return
`peer.runtime_kind=bundled` / `peer.media_engine_kind=browser_peer`.

## Gate Execution Log

2026-04-11 macOS M2 workspace:

- Release artifact checked: `build/libagentd_voice_builtin_media_engine_embedded_transport.so`.
- `python3 tools/check_voice_native_media_stack.py --pretty > build/native_plugin_graduation_stack_20260411.json 2> build/native_plugin_graduation_stack_20260411.err`
  - Result: `ok=true`; stderr empty.
- `python3 tools/inspect_voice_media_provider.py build/libagentd_voice_builtin_media_engine_embedded_transport.so --pretty > build/native_plugin_graduation_provider_20260411.json 2> build/native_plugin_graduation_provider_20260411.err`
  - Result: `ok=true`; ABI v5; provider `agentd_builtin_embedded_transport_provider` version `0.13.0`; required callbacks present; `native_media_supported=true`; RTP ingest/transmit capabilities present; stderr empty.
- `cmake --build build -j "$(sysctl -n hw.ncpu)" > build/native_plugin_graduation_build_20260411.log 2>&1`
  - Result: `build_rc=0`.
- `ctest --test-dir build -R 'session_voice_(builtin_packet_accounting|builtin_embedded_status|builtin_embedded_transport_provider|builtin_audio_payload|builtin_sdp_answer|sdp_candidate|audio_decode|audio_encode)_tests' --output-on-failure > build/native_plugin_graduation_unit_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; eight targeted native-plugin unit/provider tests passed.
- `ctest --test-dir build -R 'session_voice_builtin_dtls_transport_tests|session_voice_rtcp_report_tests' --output-on-failure > build/native_plugin_graduation_transport_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; DTLS/SRTP transport and RTCP report tests passed.
- `ctest --test-dir build -R 'agentd_session_voice_webrtc_peer_builtin_native_plugin_smoke|agentd_session_voice_webrtc_peer_builtin_default_native_plugin_smoke|agentd_session_voice_webrtc_peer_runtime_smoke|agentd_audio_webrtc_peer_smoke' --output-on-failure > build/native_plugin_graduation_smoke_ctest_20260411.log 2>&1`
  - Result: `ctest_rc=0`; four runtime/browser smokes passed with no skips.

Decision after this local pass: the macOS M2 artifact satisfies the documented
gate for lab-default use on this host. The global production default is not
changed by this evidence alone; run the same non-skipped gate on each additional
target release platform before promoting builtin under auto/default policy.
