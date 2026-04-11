#!/usr/bin/env bash

# Shared agentd lifecycle helpers for WebRTC runtime smoke tests.

start_agentd_with_voice_defaults() {
  export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
  export AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND="bundled"
  export AGENTD_AUDIO_WEBRTC_BROKER_URL="${VOICE_BROKER_URL}"
  export AGENTD_AUDIO_WEBRTC_BROKER_TOKEN="${VOICE_BROKER_TOKEN}"
  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke" >>"${LOG_FILE}" 2>&1
  unset AGENTD_AUTH_TOKEN
  unset AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND
  unset AGENTD_AUDIO_WEBRTC_BROKER_URL
  unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN
}

start_agentd_with_builtin_voice_default() {
  export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
  export AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND="builtin"
  export AGENTD_AUDIO_WEBRTC_BROKER_URL="${VOICE_BROKER_URL}"
  export AGENTD_AUDIO_WEBRTC_BROKER_TOKEN="${VOICE_BROKER_TOKEN}"
  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke_env_builtin" >>"${LOG_FILE}" 2>&1
  unset AGENTD_AUTH_TOKEN
  unset AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND
  unset AGENTD_AUDIO_WEBRTC_BROKER_URL
  unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN
}

start_agentd_without_voice_defaults() {
  export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
  unset AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND
  unset AGENTD_AUDIO_WEBRTC_BROKER_URL
  unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN
  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke" >>"${LOG_FILE}" 2>&1
  unset AGENTD_AUTH_TOKEN
}

wait_daemon_ready() {
  for _ in $(seq 1 100); do
    if curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "agentd did not become ready" >&2
  return 1
}

wait_daemon_stopped() {
  for _ in $(seq 1 100); do
    if ! curl -fsS --noproxy "*" --max-time 2 \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
      if python3 - "${HOST}" "${PORT_DAEMON}" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind((host, port))
except OSError:
    sys.exit(1)
finally:
    s.close()
sys.exit(0)
PY
      then
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "agentd did not stop cleanly" >&2
  return 1
}

restart_agentd() {
  agentd_smoke_stop
  wait_daemon_stopped
  start_agentd_with_voice_defaults
  wait_daemon_ready
}

restart_agentd_without_voice_defaults() {
  agentd_smoke_stop
  wait_daemon_stopped
  start_agentd_without_voice_defaults
  wait_daemon_ready
}

restart_agentd_with_builtin_voice_default() {
  agentd_smoke_stop
  wait_daemon_stopped
  start_agentd_with_builtin_voice_default
  wait_daemon_ready
}
