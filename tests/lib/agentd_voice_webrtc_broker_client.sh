#!/usr/bin/env bash

# Shared helpers for WebRTC smoke tests that need direct access to the test audio broker.

broker_session_create() {
  local payload="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    "${VOICE_BROKER_URL}/v1/audio/sessions" \
    -H "Authorization: Bearer audio-webui-token" \
    -H 'Content-Type: application/json' \
    -d "${payload}"
}

broker_session_get() {
  local session_id="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    "${VOICE_BROKER_URL}/v1/audio/sessions/${session_id}" \
    -H "Authorization: Bearer audio-webui-token"
}

broker_session_status_code() {
  local session_id="$1"
  curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' \
    "${VOICE_BROKER_URL}/v1/audio/sessions/${session_id}" \
    -H "Authorization: Bearer audio-webui-token"
}

broker_session_delete_status() {
  local session_id="$1"
  curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' -X DELETE \
    "${VOICE_BROKER_URL}/v1/audio/sessions/${session_id}" \
    -H "Authorization: Bearer audio-webui-token"
}

broker_session_signal() {
  local session_id="$1"
  local payload="$2"
  curl -fsS --noproxy "*" --max-time 10 \
    "${VOICE_BROKER_URL}/v1/audio/sessions/${session_id}/signal" \
    -H "Authorization: Bearer audio-webui-token" \
    -H "Content-Type: application/json" \
    -d "${payload}" >/dev/null
}
