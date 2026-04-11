#!/usr/bin/env bash

# Shared helpers for WebRTC smoke tests that call the test agentd daemon directly.

url_quote() {
  python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "$1"
}

create_session() {
  local session_id="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"${session_id}\"}" \
    "${DAEMON_URL}/api/v1/session/new" >/dev/null
}

delete_session() {
  local session_id="$1"
  curl -fsS --noproxy "*" --max-time 10 -X DELETE \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    "${DAEMON_URL}/api/v1/session?session_id=$(url_quote "${session_id}")"
}

delete_session_quiet() {
  delete_session "$1" >/dev/null
}

voice_peer_status() {
  local session_id="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=$(url_quote "${session_id}")"
}

voice_peer_request() {
  local payload="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "${payload}" \
    "${DAEMON_URL}/api/v1/session/voice_webrtc_peer"
}

voice_peer_request_status() {
  local body_path="$1"
  local payload="$2"
  curl -sS --noproxy "*" --max-time 10 -o "${body_path}" -w '%{http_code}' \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "${payload}" \
    "${DAEMON_URL}/api/v1/session/voice_webrtc_peer"
}

config_get() {
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    "${DAEMON_URL}/api/v1/config"
}

config_update() {
  local payload="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "${payload}" \
    "${DAEMON_URL}/api/v1/config/update"
}
