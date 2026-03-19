# Audio Streaming (v0)

Date: 2026-02-19
Status: implemented rolling foundation (broker signaling relay + session status APIs + WebUI voice-session controls + browser-side WebRTC negotiation client + managed agentd-side media peer runtime + RTP proof); embedded agentd-native media service still pending, but the runtime contract now exposes an explicit backend seam (`external` today, `builtin` reserved) and the shipped external backend now has durable restart-aware status recovery

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
- `ctest` now also includes `session_voice_signal_protocol_tests` for direct typed broker-signal payload coverage.
- `ctest` now also includes `session_voice_signal_session_tests` for direct native signal-session state coverage.
- `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` covers the agentd-managed runtime surface for that same peer,
  including start/status, inbound RTP proof against a live browser peer, and managed stop/teardown.
- `runtime_kind` on `POST /api/v1/session/voice_webrtc_peer` is now a start-only backend selector; stop requests ignore
  it and target the actual managed runtime state for the session.
- The managed runtime contract now explicitly reports:
  - `builtin_available=false`
  - `bundled_available=true|false`
  - `external_available=true|false`
  - `builtin_unavailable_reason`, `bundled_unavailable_reason`, `external_unavailable_reason`
  - `default_runtime_kind=builtin|bundled|external`
  - `default_runtime_kind_source=auto|env|config`
  - `default_runtime_kind_available=true|false`
  - `default_runtime_kind_unavailable_reason`
  - `peer.runtime_kind=bundled|external`
  so the current shipped Node/Playwright path is a named bundled backend with an explicit `external` override rather
  than an implicit assumption baked into the API.
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
  the reserved native backend path is cleaned up by backend-aware runtime refresh instead of accidental child-PID
  assumptions.
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
- `broker_session_id` is now mutually exclusive with `broker_agent_id` / `broker_deployment_id`, keeping the borrowed
  broker-session path distinct from the agentd-owned auto-create path.
- That managed runtime now also performs bounded startup confirmation and fails closed when the child exits before ready,
  cleaning up any agentd-owned broker audio session created for the failed start.
- The operator-configured backend seam is now also durable daemon config (`audio_webrtc.peer_tool_path`,
  `audio_webrtc.node_bin`, `audio_webrtc.default_runtime_kind`) rather than env-only. `default_runtime_kind` may be
  `builtin`, `bundled`, or `external`: config/env can intentionally pin the future native backend even though it still
  surfaces as unavailable/not implemented today, and the main runtime smoke proves both builtin-default unavailability
  and explicit config-backed external launch/default behavior. Daemon startup also honors
  `AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND`, which surfaces as `default_runtime_kind_source=env`.
- Safe daemon config now reports the same backend availability facts (`builtin_available`, `bundled_available`,
  `external_available`, `default_runtime_kind_available`) plus unavailable reasons, so a configured default such as
  `external` can be seen as unavailable before start-time failure and misconfigured `node_bin` now shows up as an
  unlaunchable backend rather than only surfacing after a start request.
- If persisted daemon config is corrupted to an invalid `audio_webrtc.default_runtime_kind`, agentd now self-heals that
  field back to `auto` on load and rewrites the SQLite runtime-config record instead of surfacing impossible state.

## Open questions

- Should signaling use WebSocket instead of SSE+POST for bidirectional framing?
- Do we need a broker-issued ephemeral token for direct agentd signaling as a fallback?
- How should the broker authenticate agentd participation (mTLS vs bearer)?
- How should the shipped managed media peer move from a Node/Playwright child runtime into an embedded long-lived agentd-native media service?
- How should the future `builtin` backend terminate WebRTC/SRTP natively inside agentd without inheriting the current child-process browser dependency?

## References

- WebRTC SDP offer/answer and ICE candidate exchange (browser-native).
- Opus codec over WebRTC (default browser support).
