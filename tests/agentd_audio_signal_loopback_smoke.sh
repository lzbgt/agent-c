#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <agentd_audio_signal_loopback>" >&2
  exit 2
fi

LOOPBACK_BIN="$1"
if [[ ! -x "$LOOPBACK_BIN" ]]; then
  echo "SKIP: loopback binary not executable" >&2
  exit 77
fi
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go not found" >&2
  exit 77
fi
curl_bin="$(agentd_smoke_curl_bin)"
if [[ "${curl_bin}" == */* ]]; then
  if [[ ! -x "${curl_bin}" ]]; then
    echo "SKIP: curl not found (${curl_bin})" >&2
    exit 77
  fi
elif ! type -P "${curl_bin}" >/dev/null 2>&1; then
  echo "SKIP: curl not found (${curl_bin})" >&2
  exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found" >&2
  exit 77
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_LIB="${ROOT}/tests/lib/pg_test_lib.sh"
if [[ -f "${PG_LIB}" ]]; then
  # shellcheck disable=SC1090
  source "${PG_LIB}"
fi
LOG_DIR="${ROOT}/build"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/agentd_audio_signal_loopback_smoke.log"

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

POSTGRES_NAME="agentd_audio_loopback_smoke"
BROKER_PORT=""
PG_PORT=""
BROKER_PID=""
LOOP_PID=""

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

cleanup() {
  if [[ -n "${LOOP_PID}" ]]; then
    kill -TERM "${LOOP_PID}" >/dev/null 2>&1 || true
    wait "${LOOP_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${BROKER_PID}" ]]; then
    kill -TERM "${BROKER_PID}" >/dev/null 2>&1 || true
    wait "${BROKER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ "${USE_DOCKER}" == "1" ]]; then
    docker rm -f "${POSTGRES_NAME}" >/dev/null 2>&1 || true
  fi
  if [[ "${USE_LOCAL_PG}" == "1" ]]; then
    pg_test_stop_local || true
  fi
}
trap cleanup EXIT

if [[ "${USE_DOCKER}" == "1" ]]; then
  PG_PORT="$(pick_port)"
fi
BROKER_PORT="$(pick_port)"

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

CLIENT_AUTH_JSON="${LOG_DIR}/broker_audio_client_auth.json"
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

BROKER_BIN="${LOG_DIR}/agentd-broker-audio-loopback"
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

SESSION_ID="$(python3 - <<PY
import http.client, json, sys
host = "127.0.0.1"
port = ${BROKER_PORT}
conn = http.client.HTTPConnection(host, port, timeout=5)
body = json.dumps({"agent_id": "a-1", "mode": "webrtc"})
conn.request("POST", "/v1/audio/sessions", body=body, headers={
    "Authorization": "Bearer audio-webui-token",
    "Content-Type": "application/json",
})
resp = conn.getresponse()
data = resp.read().decode("utf-8")
if resp.status != 200:
  print(data, file=sys.stderr)
  raise SystemExit(1)
obj = json.loads(data)
if not obj.get("ok"):
  print(data, file=sys.stderr)
  raise SystemExit(1)
print(obj.get("session_id", ""))
PY
)"

if [[ -z "${SESSION_ID}" ]]; then
  echo "Failed to create session" >&2
  exit 1
fi

"${LOOPBACK_BIN}" \
  --broker-url "http://127.0.0.1:${BROKER_PORT}" \
  --token "audio-agentd-token" \
  --session-id "${SESSION_ID}" >>"${LOG_FILE}" 2>&1 &
LOOP_PID=$!

sleep 0.5

python3 - <<PY
import http.client, json, sys, time

host = "127.0.0.1"
port = ${BROKER_PORT}
session_id = "${SESSION_ID}"

# Start SSE stream
conn_stream = http.client.HTTPConnection(host, port, timeout=10)
conn_stream.request("GET", f"/v1/audio/sessions/{session_id}/signal/stream", headers={
    "Authorization": "Bearer audio-webui-token",
})
resp_stream = conn_stream.getresponse()
if resp_stream.status != 200:
  print("stream failed", resp_stream.status, file=sys.stderr)
  raise SystemExit(1)

# Consume initial keepalive
_ = resp_stream.fp.readline().decode("utf-8")

# Send offer
conn_offer = http.client.HTTPConnection(host, port, timeout=5)
offer_body = json.dumps({"type": "offer", "payload": {"sdp": "stub-offer"}})
conn_offer.request("POST", f"/v1/audio/sessions/{session_id}/signal", body=offer_body, headers={
    "Authorization": "Bearer audio-webui-token",
    "Content-Type": "application/json",
})
resp_offer = conn_offer.getresponse()
resp_offer.read()
if resp_offer.status != 200:
  print("offer failed", resp_offer.status, file=sys.stderr)
  raise SystemExit(1)

# Wait for answer
answer = None
deadline = time.time() + 5
while time.time() < deadline:
  line = resp_stream.fp.readline().decode("utf-8")
  if not line:
    time.sleep(0.05)
    continue
  line = line.strip()
  if not line.startswith("data: "):
    continue
  payload = line[len("data: "):]
  try:
    msg = json.loads(payload)
  except Exception:
    continue
  if msg.get("type") == "answer" and msg.get("payload", {}).get("sdp") == "stub-answer":
    answer = msg
    break

if not answer:
  print("missing answer", file=sys.stderr)
  raise SystemExit(1)

# Close session
conn_bye = http.client.HTTPConnection(host, port, timeout=5)
bye_body = json.dumps({"type": "bye"})
conn_bye.request("POST", f"/v1/audio/sessions/{session_id}/signal", body=bye_body, headers={
    "Authorization": "Bearer audio-webui-token",
    "Content-Type": "application/json",
})
resp_bye = conn_bye.getresponse()
resp_bye.read()
if resp_bye.status != 200:
  print("bye failed", resp_bye.status, file=sys.stderr)
  raise SystemExit(1)

print("agentd_audio_signal_loopback_smoke OK")
PY

wait "${LOOP_PID}" >/dev/null 2>&1 || true
