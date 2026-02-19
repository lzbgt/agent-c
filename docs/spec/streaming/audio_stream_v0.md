# Audio Streaming (v0)

Date: 2026-02-19
Status: v0 (broker signaling relay implemented; agentd loopback smoke tool added; real WebRTC pending)

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

## Open questions

- Should signaling use WebSocket instead of SSE+POST for bidirectional framing?
- Do we need a broker-issued ephemeral token for direct agentd signaling as a fallback?
- How should the broker authenticate agentd participation (mTLS vs bearer)?
- What is the minimal agentd API surface for voice session control (start/stop, stats)?

## References

- WebRTC SDP offer/answer and ICE candidate exchange (browser-native).
- Opus codec over WebRTC (default browser support).
