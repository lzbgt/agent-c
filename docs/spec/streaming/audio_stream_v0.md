# Audio Streaming (v0)

Date: 2026-02-19
Status: implemented rolling foundation (broker signaling relay + session status APIs + WebUI voice-session controls + browser-side WebRTC negotiation client + managed agentd-side media peer runtime + embedded builtin native-plugin RTP/RTCP proof); embedded agentd-native media service is still not a full WebRTC runtime, but the runtime contract now exposes explicit `bundled`, `external`, `builtin=signaling_stub`, and `builtin=native_plugin` backend seams and the shipped backends now have durable restart-aware status recovery

## Goals

- Establish a **bidirectional, low-latency audio path** between WebUI and agentd.
- Keep media transport **end-to-end** (broker relays signaling only; no media proxy in v0).
- Reuse existing broker auth + deployment routing for multi-agentd setups.
- Make the signaling protocol explicit, versioned, and testable.

## Non-goals

- Media mixing, multi-party conferencing, or server-side recording.
- Broker-side transcoding or RTP/SRTP termination.
- Speech-to-text / text-to-speech provider specifics (handled in agentd/clients).

## Architecture (v0)

```
WebUI (browser)  <---signaling--->  Broker  <---signaling--->  agentd
       \---------------------------------- media (WebRTC SRTP) ----------------------------------/
```

- WebUI and agentd establish a direct WebRTC connection.
- Broker provides authenticated signaling relay + session metadata only.
- Audio codecs: Opus over WebRTC (default browser support).

## Signaling surface

### 1) Create audio session

`POST /v1/audio/sessions`

Request:
```json
{
  "agent_id": "a-123",
  "deployment_id": "default",
  "mode": "webrtc",
  "metadata": {
    "client": "webui",
    "version": "v0"
  }
}
```

Response:
```json
{
  "ok": true,
  "session_id": "aud_...",
  "expires_unix_ms": 0,
  "signal": {
    "send_url": "https://<broker>/v1/audio/sessions/<id>/signal",
    "recv_url": "https://<broker>/v1/audio/sessions/<id>/signal/stream"
  }
}
```

### 2) Send signaling message

`POST /v1/audio/sessions/{session_id}/signal`

Request:
```json
{
  "type": "offer|answer|candidate|bye|control",
  "payload": {"sdp": "..."}
}
```

Notes:
- `payload` is a pass-through JSON object (for SDP offers/answers or ICE candidates).
- Broker does not interpret SDP; it only relays between participants.

### 3) Receive signaling stream

`GET /v1/audio/sessions/{session_id}/signal/stream` (SSE)

Event payload:
```json
{
  "type": "offer|answer|candidate|bye|control",
  "payload": {"sdp": "..."},
  "from": "webui|agentd",
  "ts_unix_ms": 0
}
```

## Session lifecycle

- Session creation is authenticated using the same broker auth as other endpoints.
- Sessions expire automatically (default TTL) and are cleaned up on disconnect.
- `bye` signals a graceful teardown; broker closes the signaling stream and marks session closed.

## Minimal loopback smoke test (implemented)

A v0 smoke test should:
1) Launch an agentd loopback client that subscribes to broker audio signals
2) Create an audio session via broker
3) Exchange offer/answer over the signaling endpoints
4) Verify signaling completes and session closes cleanly

## Current proof points

- `ctest` includes `broker_audio_signal_loopback_smoke` for broker signaling relay behavior.
- `ctest` includes `broker_audio_signal_docker_smoke` for broker signaling with ephemeral Postgres backing.
- `ctest` includes `agentd_audio_signal_loopback_smoke` for the agentd-side loopback tool and signaling flow.
- Agentd now also ships shared native broker-signal helpers for common control flow:
  - wait for a specific signal type over SSE
  - send standard `answer` payloads
  - send graceful `bye` payloads
  These are reused by the loopback tool instead of remaining trapped in tool-local HTTP/SSE code.
- Agentd also now ships shared typed broker-signal payload semantics in C++:
  - parsed event `sender_tag` extraction
  - typed offer/answer SDP payload parsing
  - typed ICE candidate payload parsing
  - typed `bye` payload parsing/building
  so the future builtin backend can reuse one protocol contract instead of re-deriving JSON fields ad hoc.
- Agentd now also ships a shared C++ signal-session state helper for the next builtin backend seam:
  - self-sender filtering by `sender_tag`
  - queued remote ICE candidates before remote description is applied
  - candidate drain once the remote description is accepted
  - remote `bye` close tracking
  This mirrors the currently shipped browser peer behavior in reusable native code instead of leaving it only in the JS runtime.
- The shared native signal client now also exposes a session-aware typed-ingress stream on top of raw SSE plus typed
  egress builders for answer/candidate/bye payloads, so future builtin media code can operate on parsed session events
  instead of reimplementing self-filtering and payload wiring on every call site.
- Broker now exposes live session lifecycle/status APIs:
  - `GET /v1/audio/sessions`
  - `GET /v1/audio/sessions/{session_id}`
  - `DELETE /v1/audio/sessions/{session_id}`
- WebUI broker panel now exposes explicit voice session create/list/select/send/delete controls with live signal inspection.
- WebUI broker panel now also drives browser-side `RTCPeerConnection` negotiation over those broker signaling APIs,
  including automated offer/answer/bye handling, remote-candidate handling, and a mounted remote audio element.
- Agentd now exposes minimal session-scoped voice control/stats endpoints:
  - `POST /api/v1/session/voice_control`
  - `GET /api/v1/session/voice_stats`
  These use durable `ui_action` + `client_event` plumbing to drive `media_play`, `media_pause`, and `media_snapshot`
  without inventing a parallel control channel.
- Agentd now also exposes a first-class managed WebRTC peer runtime:
  - `POST /api/v1/session/voice_webrtc_peer`
  - `GET /api/v1/session/voice_webrtc_peer`
  This lets agentd start, inspect, and stop the shipped host-side Playwright media peer without manual process bring-up.
- That managed runtime can now also create the broker audio session on demand: callers may omit `broker_session_id`
  and instead provide `broker_agent_id` (plus optional `broker_deployment_id`), after which agentd creates the broker
  session, launches the peer, and reports `peer.managed_broker_session=true`.
- WebUI advanced tools now expose a dedicated Voice panel for session-scoped play/pause/snapshot control and
  durable stats inspection on top of those agentd endpoints, with deterministic Playwright coverage.
- `tests/webui_observe_voice_hello_openworld.sh` and `ui/e2e/observe_voice_hello.spec.ts`
  cover the current browser voice-presentation harness, which is artifact/scene driven rather than
  a full WebRTC session.
- `ui/e2e/broker_audio_panel.spec.ts` covers the browser-side WebRTC control flow against a deterministic mocked peer.
- `tests/agentd_audio_webrtc_peer_smoke.sh` plus `tools/agentd_audio_webrtc_peer.js` cover a real browser-to-agentd-side
  RTP path over broker signaling, including offer/answer exchange, ICE candidates, inbound audio stats, and `bye`
  teardown against a live headless Chromium peer.
- `tests/agentd_audio_signal_loopback_smoke.sh` now also proves agentd-side graceful `bye` after the stub `answer`,
  and confirms the broker reports the session closed rather than leaving teardown implicit.
- The native session-aware signal wait path now also preserves trickled ICE candidates that arrive before the first
  remote description; the loopback smoke proves that queued pre-offer candidate survives until the remote offer is
  accepted instead of being dropped by the first-description wait helper.
- The same native session-aware path can now continue on the same session state after remote-description bootstrap and
  wait for ready post-description candidates too; the loopback smoke proves both queued pre-offer ICE and a later
  post-answer candidate through the shared C++ helper path.
- Agentd now also has a native stateful wait path for remote `bye` on that same shared session state, and a dedicated
  remote-bye smoke proves agentd can consume pre-offer ICE, post-answer ICE, and then a browser-side close reason
  without dropping back to ad hoc stream parsing.
- The full “answer a remote offer” control flow is now also collapsed into one shared C++ negotiation helper, so the
  loopback tool no longer hand-assembles offer wait, answer send, candidate wait, remote `bye`, and optional local
  `bye` sequencing inline.
- That negotiation helper now also has direct unit coverage via injectable ops, so future builtin/backend work can
  extend transport or media integration without relying only on live broker smokes to prove the core sequencing logic.
- `ctest` now also includes `session_voice_signal_protocol_tests` for direct typed broker-signal payload coverage.
- `ctest` now also includes `session_voice_signal_session_tests` for direct native signal-session state coverage.
- `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` covers the agentd-managed runtime surface for that same peer,
  including start/status, inbound RTP proof against a live browser peer, and managed stop/teardown.
- `tests/agentd_session_voice_webrtc_peer_builtin_smoke.sh` now covers the experimental builtin signaling-stub backend:
  agentd starts an in-process native runtime, auto-creates or borrows the broker signaling session, answers a remote
  offer with a stub SDP answer, observes remote ICE/`bye`, and tears down cleanly without launching the browser helper.
- `runtime_kind` on `POST /api/v1/session/voice_webrtc_peer` is now a start-only backend selector; stop requests ignore
  it and target the actual managed runtime state for the session.
- The managed runtime contract now explicitly reports:
  - `builtin_available=true|false`
  - `builtin_mode=disabled|signaling_stub|native_plugin`
  - `builtin_native_library_path_configured=true|false`
  - `bundled_available=true|false`
  - `external_available=true|false`
  - `builtin_unavailable_reason`, `bundled_unavailable_reason`, `external_unavailable_reason`
  - `default_runtime_kind=builtin|bundled|external`
  - `default_runtime_kind_source=auto|env|config`
  - `default_runtime_kind_available=true|false`
  - `default_runtime_kind_unavailable_reason`
  - `peer.runtime_kind=builtin|bundled|external`
  - `peer.media_engine_kind=builtin_reserved|builtin_signaling_stub|builtin_native_plugin|browser_peer`
  - `peer.media_engine_state=planned|starting|signaling_ready|answer_ready|signaling_active|stopping|stopped|failed`
  - `peer.media_state_updated_unix_ms`
  - `peer.media_events_total`, `peer.media_remote_offers_seen`, `peer.media_answers_sent`,
    `peer.media_remote_candidates_seen`, `peer.media_remote_byes_seen`, `peer.media_local_byes_sent`
  - `peer.native_media_supported=true|false`
  - `peer.native_media_active=true|false`
  so the current shipped Node/Playwright path is a named bundled backend with an explicit `external` override, and the
  experimental in-process builtin backend is surfaced as a real managed runtime mode rather than an API-only placeholder.
- The shipped bundled/external backend now persists DB-backed runtime snapshots plus per-session stdout logs, so
  `GET /api/v1/session/voice_webrtc_peer` can recover running/stopped state after agentd restart and duplicate starts
  can return `already_running` from persisted runtime state when the effective resolved request/runtime config remains
  compatible with the live runtime.
- If callers ask for a different live runtime config on the same session, whether explicitly or indirectly through
  changed daemon defaults like backend selection or `node_bin`, or through config that now makes the selected backend
  unlaunchable, `POST /api/v1/session/voice_webrtc_peer` now returns `409` with the current `peer` snapshot instead of
  reporting a false idempotent reuse.
- If daemon backend policy rotates underneath a live bundled/external peer, runtime reads and `409` start-conflict
  responses now also surface `backend_policy_drift.changed_fields[]` plus
  `backend_policy_drift.current_effective_start`, so callers can distinguish default-backend drift from tool-path,
  `node_bin`, or broker-default drift without diffing daemon config out of band.
- That same runtime now persists child-exit state eagerly and lets `action=stop` take `broker_token`, so agentd can
  delete a managed broker audio session itself after an ungraceful peer death that skipped the child's `bye`.
- Stop and session-delete cleanup now validate broker tokens lazily, so borrowed broker-session runtimes can still be
  stopped or erased even if the daemon's configured default broker token is malformed.
- When the runtime owns the broker audio session and broker deletion fails, agentd now still completes local
  stop/session-delete teardown and reports the broker cleanup failure explicitly instead of failing the local cleanup.
- `DELETE /api/v1/session?session_id=...&broker_token=...` now also cleans up the managed voice runtime for that session:
  the peer is stopped, persisted runtime artifacts are removed, and the owned broker audio session is deleted.
- If that delete has to stop a live bundled/external peer recovered from persisted running state after agentd restart,
  the returned cleanup snapshot now preserves the peer's terminal signal/result before the runtime record is cleared.
- `GET /api/v1/session/voice_webrtc_peer` now also self-heals stale local runtime state if the canonical session row
  disappeared outside the normal delete endpoint, and reports that cleanup in `cleanup_on_missing_session`.
- Corrupt persisted `session.voice_webrtc_peer.*` records are now self-healed too: agentd clears the bad record,
  removes stale local runtime artifacts, and reports that recovery in `cleanup_on_corrupt_record` instead of failing
  status/start/stop on a dead persisted snapshot.
- Persisted `session.voice_webrtc_peer.*` records that still claim `running=true` after a dead daemon restart are now
  self-healed too: status/stop/start clear the stale record, remove stale local runtime artifacts, and expose that
  recovery in `cleanup_on_stale_record` instead of reporting a fake recovered peer.
- That stale-record self-heal now also applies explicitly to impossible persisted `runtime_kind=builtin` snapshots, so
  backend-aware runtime refresh cleans up bogus builtin records regardless of whether builtin mode is disabled or an old
  placeholder snapshot leaked through before the current experimental runtime existed.
- `POST /api/v1/session/voice_webrtc_peer` `action=stop` now also returns `reason=not_running` when the peer already
  exited, while still allowing agentd to clean up an owned broker audio session for that finished runtime.
- If a live bundled/external peer is recovered from persisted running state after agentd restart, a later
  `action=stop` now also preserves an explicit terminal signal/result in the persisted runtime snapshot instead of
  flattening the recovered peer into a generic stopped record.
- Agentd now also supports daemon-level broker URL/token defaults for that managed runtime, so callers may omit
  `broker_url` / `broker_token` on normal `voice_webrtc_peer` start/stop flows and still let agentd create/clean up
  broker-owned audio sessions. Safe config and runtime status expose only boolean presence, not the token itself.
- If callers provide `broker_session_id`, agentd now preflights that session through the broker and fails before spawn
  when the session does not exist, instead of discovering the problem only after launching the managed peer.
- That borrowed-session preflight and the same mixed-mode request validation now also apply to `runtime_kind=builtin`,
  so builtin requests fail on invalid broker/session inputs before backend launch semantics diverge.
- When builtin mode is disabled, valid `runtime_kind=builtin` requests still return `builtin_start_contract`, which
  captures the native start intent (`broker_session` mode/details, `sender_tag`, timing knobs, staged startup
  sequence, and `mutating_broker_actions_deferred=true`) plus a shared safe `media_runtime_plan` for the future native
  peer, plus the shared planned runtime artifact layout, plus a runtime-schema-shaped `planned_runtime` preview with
  explicit `status_source=planned` and builtin execution sentinels (`tool_path="@builtin"`, `node_bin="@builtin"`).
  The same valid builtin `501` response also exposes that preview through top-level `peer`, without pretending the
  builtin media runtime already exists.
- When builtin mode is enabled through `audio_webrtc.builtin_mode=signaling_stub` or
  `AGENTD_AUDIO_WEBRTC_BUILTIN_MODE=signaling_stub`, `runtime_kind=builtin` now launches an in-process native
  signaling stub runtime instead of returning `501`. That runtime uses the shared broker-session and signaling helpers,
  emits a normal builtin runtime snapshot, answers remote offers with a stub SDP answer, and terminates on remote or
  local `bye`. It is intentionally limited to signaling/runtime control; the real agentd-native RTP/WebRTC media
  engine is still the remaining gap.
- When builtin mode is enabled through `audio_webrtc.builtin_mode=native_plugin` plus
  `audio_webrtc.builtin_native_library_path` (or the matching `AGENTD_AUDIO_WEBRTC_*` env vars),
  `runtime_kind=builtin` now loads a process-local media-engine provider shared library through a stable C ABI. That
  gives agentd a real native backend load path, direct availability/loadability reporting, and smoke/unit proof without
  yet baking a specific WebRTC/SRTP stack into the tree. The provider seam is now self-describing: config/runtime
  metadata expose `builtin_native_probe`, and live/planned builtin snapshots carry `native_media_provider` with the
  loaded provider ABI/name/version/capabilities. The current shipped providers now use pollable ABI v5, so builtin
  runtime progress, bounded PCM handoff, and agentd-to-provider outbound PCM submission can surface asynchronously even
  when no new broker signaling ingress arrives.
- The repo now also ships a daemon-owned sample provider module for that native-plugin seam. It proves the real
  process-local provider path without depending on a test fixture, but it intentionally still reports
  `native_media_supported=false` / `native_media_active=false` because it is only a signaling/answer-exchange sample,
  not an embedded RTP/SRTP/WebRTC media engine yet.
- When the local build has `libjuice + libsrtp2 + usrsctp` available, the repo now also builds an optional embedded
  transport provider module for that same seam:
  `./build/libagentd_voice_builtin_media_engine_embedded_transport.{so,dylib,dll}`. That provider uses the real ICE /
  SRTP / SCTP dependency family and returns a real libjuice local description through the native-plugin ABI. It now
  reports `native_media_supported=true` for receive-side native media ownership while still keeping
  `native_media_active=false` until live RTP is actually ingested. The stronger proof point now is that the provider
  gathers local ICE candidates before forming its answer, generates an ephemeral local DTLS identity, mirrors
  browser-style media offers into an inactive answer with `a=setup:passive` and a surfaced SHA-256 fingerprint, and
  now reuses the shared in-tree SRTP/RTP ingest utility that terminates inbound protected RTP into concrete header /
  payload counters. It now also owns a minimal receive-side audio stage: RTP payload types are mapped from the remote
  SDP, `PCMU` / `PCMA` payloads decode directly, and `OPUS` payloads decode through `libopus` when that library is
  available at build time, with decoded PCM samples staged in-process and then drained through the provider ABI into
  agentd-owned bounded PCM memory. The repo also has a direct DTLS/SRTP proof slice in
  `session_voice_builtin_dtls_transport_tests`: the same OpenSSL DTLS configuration used by the builtin native-plugin
  provider completes an in-tree DTLS 1.2 handshake, negotiates `SRTP_AES128_CM_SHA1_80`, exports DTLS-SRTP keying
  material over datagram-memory BIOs, derives real inbound/outbound libsrtp contexts from the exporter output, and now
  proves outbound SRTP protect plus inbound SRTP/RTP ingest and receive-side audio decode. Agentd can now also submit
  processed PCM back through the native-provider ABI; the embedded provider negotiates outbound Opus when `libopus` is
  present, falls back to negotiated G.711, encodes a bounded Opus or PCMU/PCMA RTP frame, protects it with the outbound
  libsrtp context, and transmits it over libjuice. It now emits reduced-size RTCP Sender Reports (`PT=200`) using the
  outbound SSRC on a bounded first-frame / packet-count / time cadence instead of after every RTP frame, protects them
  with SRTCP, and persists inbound/outbound RTCP counters plus last-packet metadata. After inbound RTP arrives, it now
  also emits a bounded RTCP Receiver Report (`PT=201`) with the inbound SSRC, fraction/cumulative loss, extended highest
  sequence, jitter, and `LSR`/`DLSR` when a remote Sender Report was observed. The Sender/Receiver Report packet layouts are pinned to the persisted RFC 3550
  reference in `docs/research/vendor-rfc3550-rtp-20260411.txt`, and the report-block tracking/packet builder now has a
  shared `session_voice_rtcp_report` helper with direct unit coverage. The remaining gap is no longer basic outbound
  RTP, Opus ownership, or one-shot Sender/Receiver Report transmit; it is fuller compound RTCP cadence and broader
  browser-peer full-duplex validation.
- That runtime contract now also exposes the media-engine seam directly: planned/live builtin paths report
  `media_engine_kind=builtin_reserved|builtin_signaling_stub|builtin_native_plugin`, bundled/external runtimes report
  `media_engine_kind=browser_peer`, and `native_media_supported` / `native_media_active` now distinguish the
  signaling-only stub from a loadable native media provider.
- Builtin native-plugin runtime snapshots now also surface provider DTLS diagnostics when available through
  `dtls_identity_ready`, `dtls_fingerprint_sha256`, `dtls_setup_role`, `dtls_certificate_subject`,
  `dtls_handshake_ready`, `dtls_exporter_ready`, `dtls_handshake_state`, `dtls_selected_srtp_profile`,
  `srtp_contexts_ready`, `srtp_inbound_ready`, `srtp_outbound_ready`, `srtp_last_error`,
  `dtls_packets_sent`, `dtls_packets_received`, `rtp_packets_received`, `rtp_payload_bytes_received`,
  `rtp_packets_sent`, `rtp_payload_bytes_sent`, `rtp_last_payload_type`, `rtp_last_sequence`,
  `rtp_last_timestamp`, `rtp_last_ssrc`, `rtp_last_sent_payload_type`, `rtp_last_sent_sequence`,
  `rtp_last_sent_timestamp`, `rtp_last_sent_ssrc`,
  `rtcp_packets_received`, `rtcp_packets_sent`, `rtcp_payload_bytes_received`,
  `rtcp_payload_bytes_sent`, `rtcp_last_packet_type`, `rtcp_last_ssrc`,
  `rtcp_last_sent_packet_type`, `rtcp_last_sent_ssrc`,
  `rtcp_sender_reports_sent`, `rtcp_receiver_reports_sent`,
  `rtcp_receiver_report_blocks_sent`, `rtcp_last_reported_rtp_ssrc`,
  `rtcp_last_report_fraction_lost`, `rtcp_last_report_cumulative_lost`,
  `rtcp_last_report_highest_sequence`, `rtcp_last_report_jitter`,
  `rtcp_last_report_lsr`, `rtcp_last_report_dlsr`,
  `audio_frames_decoded`, `audio_pcm_samples_decoded`, `audio_pcm_samples_buffered`,
  `audio_outbound_frames_sent`, `audio_pcm_samples_submitted_total`, `audio_last_outbound_samples`,
  `audio_outbound_payload_type`, `audio_outbound_codec_name`,
  `audio_outbound_sample_rate_hz`, `audio_outbound_channels`,
  `audio_drain_events_total`, `audio_pcm_samples_drained_total`, `audio_pcm_samples_owned`,
  `audio_last_drain_samples`, `audio_process_events_total`,
  `audio_pcm_samples_processed_total`, `audio_last_process_samples`,
  `audio_last_peak_abs_pcm16`, `audio_last_rms_pcm16`,
  `audio_render_events_total`, `audio_pcm_samples_rendered_total`,
  `audio_render_window_samples`, `audio_last_render_samples`,
  `audio_playback_enabled`, `audio_playback_stream_open`,
  `audio_playback_events_total`, `audio_pcm_samples_played_total`,
  `audio_pcm_samples_playback_queued`, `audio_last_playback_samples`,
  `audio_last_sample_rate_hz`, `audio_last_channels`, `audio_last_frame_samples_per_channel`,
  `audio_last_codec_name`, `audio_last_error`, `audio_outbound_last_error`, `rtcp_last_error`,
  `audio_render_wav_path`,
  `audio_render_last_error`, `audio_playback_device_name`, and
  `audio_playback_last_error`.
  Those fields can now also advance through provider-polled async status events rather than only through direct
  remote-description or remote-candidate callbacks.
- Builtin runtime observability is now explicit too: normalized per-session JSONL events and the persisted/live runtime
  snapshot both surface `media_engine_state` plus cumulative counters for offers, answers, candidates, and `bye`
  events, so the future native engine can inherit one operator-facing status contract instead of inventing telemetry
  after RTP lands.
- Those planned builtin previews now also fail closed at persistence boundaries: agentd refuses to persist
  `status_source=planned`, and any stale planned record is self-healed through `cleanup_on_corrupt_record` instead of
  being recovered as if it were a real runtime.
- `broker_session_id` is now mutually exclusive with `broker_agent_id` / `broker_deployment_id`, keeping the borrowed
  broker-session path distinct from the agentd-owned auto-create path.
- That managed runtime now also performs bounded startup confirmation and fails closed when the child exits before ready,
  cleaning up any agentd-owned broker audio session created for the failed start.
- The operator-configured backend seam is now also durable daemon config (`audio_webrtc.peer_tool_path`,
  `audio_webrtc.node_bin`, `audio_webrtc.builtin_mode`, `audio_webrtc.builtin_native_library_path`, and
  `audio_webrtc.default_runtime_kind`) rather than env-only. `default_runtime_kind` may be `builtin`, `bundled`, or
  `external`: config/env can intentionally pin the native backend, while `builtin_mode=signaling_stub|native_plugin`
  controls whether the experimental in-process builtin runtime is launchable or still surfaces as unavailable. Daemon
  startup also honors `AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND`, `AGENTD_AUDIO_WEBRTC_BUILTIN_MODE`,
  `AGENTD_AUDIO_WEBRTC_BUILTIN_NATIVE_LIBRARY`, and
  `AGENTD_AUDIO_WEBRTC_BUILTIN_LOCAL_PLAYBACK`, which surface as explicit runtime/config metadata.
- Safe daemon config now reports the same backend availability facts (`builtin_available`, `bundled_available`,
  `external_available`, `default_runtime_kind_available`) plus unavailable reasons, the explicit `builtin_mode`, and
  `builtin_native_probe`, so a configured default such as `external` can be seen as unavailable before start-time
  failure, `builtin` can be seen as intentionally disabled vs experimentally enabled, and misconfigured `node_bin` or
  native provider library can now show up as an unlaunchable backend rather than only surfacing after a start request.
- Operators can inspect a candidate provider library offline via
  `python3 tools/inspect_voice_media_provider.py /abs/path/to/provider.{so,dylib,dll} --pretty`, which validates the
  exported provider symbol/ABI and prints the declared capability metadata before daemon bring-up.
- Operators can also inspect local native dependency readiness via
  `python3 tools/check_voice_native_media_stack.py --pretty`, which checks `pkg-config`, filesystem install surfaces,
  and Homebrew formula availability for `opus`, `portaudio`, `srtp`, `libusrsctp`, `libjuice`, and `libdatachannel`.
- If persisted daemon config is corrupted to an invalid `audio_webrtc.default_runtime_kind`, agentd now self-heals that
  field back to `auto` on load and rewrites the SQLite runtime-config record instead of surfacing impossible state.

## Open questions

- Should signaling use WebSocket instead of SSE+POST for bidirectional framing?
- Do we need a broker-issued ephemeral token for direct agentd signaling as a fallback?
- How should the broker authenticate agentd participation (mTLS vs bearer)?
- How should the shipped managed media peer move from a Node/Playwright child runtime into an embedded long-lived agentd-native media service?
- Which concrete native DTLS/RTP stack should replace the current dependency-backed embedded transport provider so
  `builtin_mode=native_plugin` can report `native_media_supported=true` and terminate media fully inside agentd?

## References

- WebRTC SDP offer/answer and ICE candidate exchange (browser-native).
- Opus codec over WebRTC (default browser support).
