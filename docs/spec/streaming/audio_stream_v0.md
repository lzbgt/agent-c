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
- WebUI advanced tools now expose a dedicated Voice panel for session-scoped play/pause/snapshot control and
  durable stats inspection on top of those agentd endpoints, with deterministic Playwright coverage.
- `tests/webui_observe_voice_hello_openworld.sh` and `ui/e2e/observe_voice_hello.spec.ts`
  cover the current browser voice-presentation harness, which is artifact/scene driven rather than
  a full WebRTC session.
- `ui/e2e/broker_audio_panel.spec.ts` covers the browser-side WebRTC control flow against a deterministic mocked peer.
- `tests/agentd_audio_webrtc_peer_smoke.sh` plus `tools/agentd_audio_webrtc_peer.js` cover a real browser-to-agentd-side
  RTP path over broker signaling, including offer/answer exchange, ICE candidates, inbound audio stats, and `bye`
  teardown against a live headless Chromium peer.
- `tests/agentd_session_voice_webrtc_peer_runtime_smoke.sh` covers the agentd-managed runtime surface for that same peer,
  including start/status, inbound RTP proof against a live browser peer, and managed stop/teardown.
- The managed runtime contract now explicitly reports:
  - `builtin_available=false`
  - `default_runtime_kind=external`
  - `peer.runtime_kind=external`
  so the current shipped Node/Playwright path is a named backend rather than an implicit assumption baked into the API.
- The shipped external backend now persists DB-backed runtime snapshots plus per-session stdout logs, so
  `GET /api/v1/session/voice_webrtc_peer` can recover running/stopped state after agentd restart and duplicate starts
  can return `already_running` from persisted runtime state.

## Open questions

- Should signaling use WebSocket instead of SSE+POST for bidirectional framing?
- Do we need a broker-issued ephemeral token for direct agentd signaling as a fallback?
- How should the broker authenticate agentd participation (mTLS vs bearer)?
- How should the shipped managed media peer move from a Node/Playwright child runtime into an embedded long-lived agentd-native media service?
- How should the future `builtin` backend terminate WebRTC/SRTP natively inside agentd without inheriting the current child-process browser dependency?

## References

- WebRTC SDP offer/answer and ICE candidate exchange (browser-native).
- Opus codec over WebRTC (default browser support).
