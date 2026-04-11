#!/usr/bin/env bash

# Shared Postgres + audio-broker fixture setup for WebRTC runtime smoke tests.

voice_webrtc_pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

voice_webrtc_init_broker_fixture() {
  local pg_lib="${ROOT}/tests/lib/pg_test_lib.sh"
  if [[ -f "${pg_lib}" ]]; then
    # shellcheck disable=SC1090
    source "${pg_lib}"
  fi

  PG_DSN_OVERRIDE="${AGENTD_TEST_PG_DSN:-}"
  USE_DOCKER="1"
  USE_LOCAL_PG="0"
  if [[ -n "${PG_DSN_OVERRIDE}" ]]; then
    USE_DOCKER="0"
  elif ! command -v docker >/dev/null 2>&1; then
    USE_DOCKER="0"
  elif ! timeout 2 docker info >/dev/null 2>&1; then
    USE_DOCKER="0"
  fi

  if [[ "${USE_DOCKER}" == "0" && -z "${PG_DSN_OVERRIDE}" ]]; then
    if pg_test_has_local_pg; then
      USE_LOCAL_PG="1"
    else
      reason="$(pg_test_unavailable_reason)"
      echo "SKIP: docker not ready and local Postgres not available (${reason}); set AGENTD_TEST_PG_DSN to run" >&2
      exit 77
    fi
  fi

  POSTGRES_NAME=""
  BROKER_PORT=""
  PORT_DAEMON=""
  PG_PORT=""
  BROKER_PID=""

  if [[ "${USE_DOCKER}" == "1" ]]; then
    PG_PORT="$(voice_webrtc_pick_port)"
  fi
  BROKER_PORT="$(voice_webrtc_pick_port)"
  PORT_DAEMON="$(voice_webrtc_pick_port)"
  POSTGRES_NAME="agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}"
  SESSION_DB_PATH="${LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}.sqlite"
  STATE_DIR="${LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}.state"
}

voice_webrtc_cleanup_broker_fixture() {
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill -TERM "${BROKER_PID}" >/dev/null 2>&1 || true
    wait "${BROKER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ "${USE_DOCKER:-0}" == "1" ]]; then
    docker rm -f "${POSTGRES_NAME}" >/dev/null 2>&1 || true
  fi
  if [[ "${USE_LOCAL_PG:-0}" == "1" ]]; then
    pg_test_stop_local || true
  fi
}

voice_webrtc_start_broker_fixture() {
  if [[ "${USE_DOCKER}" == "1" ]]; then
    if docker ps -a --format '{{.Names}}' | grep -q "^${POSTGRES_NAME}$"; then
      docker rm -f "${POSTGRES_NAME}" >/dev/null 2>&1 || true
    fi
    docker run -d --rm --name "${POSTGRES_NAME}" -e POSTGRES_PASSWORD=postgres -p "${PG_PORT}:5432" postgres:16 >/dev/null
    for _ in $(seq 1 30); do
      if docker exec "${POSTGRES_NAME}" pg_isready -U postgres >/dev/null 2>&1; then
        break
      fi
      sleep 1
    done
    if ! docker exec "${POSTGRES_NAME}" pg_isready -U postgres >/dev/null 2>&1; then
      echo "Postgres did not become ready" >&2
      exit 1
    fi
  fi

  if [[ "${USE_LOCAL_PG}" == "1" ]]; then
    if ! pg_test_start_local; then
      echo "SKIP: local Postgres init failed" >&2
      exit 77
    fi
  fi

  CLIENT_AUTH_JSON="${RUN_LOG_DIR:-${LOG_DIR}}/broker_audio_runtime_client_auth.json"
  cat >"${CLIENT_AUTH_JSON}" <<JSON
{
  "clients": [
    {
      "client_id": "audio-agentd",
      "token": "audio-agentd-token",
      "admin": true
    },
    {
      "client_id": "audio-webui",
      "token": "audio-webui-token",
      "admin": true
    }
  ]
}
JSON

  BROKER_BIN="${RUN_LOG_DIR:-${LOG_DIR}}/agentd-broker-audio-runtime"
  (
    cd "${ROOT}/broker"
    go build -trimpath -o "${BROKER_BIN}" ./cmd/agentd-broker
  ) >>"${LOG_FILE}" 2>&1

  DSN="${PG_DSN_OVERRIDE}"
  if [[ -z "${DSN}" ]]; then
    if [[ "${USE_LOCAL_PG}" == "1" ]]; then
      DSN="${PG_TEST_DSN}"
    else
      DSN="postgres://postgres:postgres@127.0.0.1:${PG_PORT}/postgres?sslmode=disable"
    fi
  fi

  "${BROKER_BIN}" \
    --listen "127.0.0.1:${BROKER_PORT}" \
    --db-dsn "${DSN}" \
    --client-auth-file "${CLIENT_AUTH_JSON}" >>"${LOG_FILE}" 2>&1 &
  BROKER_PID=$!

  for _ in $(seq 1 60); do
    if curl -fsS "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
      break
    fi
    sleep 0.2
  done
  if ! curl -fsS "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
    echo "Broker did not become ready; see ${LOG_FILE}" >&2
    exit 1
  fi

  VOICE_BROKER_URL="http://127.0.0.1:${BROKER_PORT}"
  VOICE_BROKER_TOKEN="audio-agentd-token"
}
