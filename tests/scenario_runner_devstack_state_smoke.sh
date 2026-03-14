#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'jobs -pr | xargs -r kill >/dev/null 2>&1 || true; rm -rf "${TMP_DIR}"' EXIT

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

write_file() {
  local path="$1"
  local content="$2"
  mkdir -p "$(dirname "${path}")"
  printf '%s\n' "${content}" > "${path}"
}

AGENTD_ROOT="${TMP_DIR}/agentd"
BROKER_ROOT="${TMP_DIR}/broker"
AGENTD_PORT="$(pick_port)"
BROKER_PORT="$(pick_port)"
STATE_PATH="${TMP_DIR}/devstack_state.json"
OUT_AGENTD="${TMP_DIR}/out_agentd"
OUT_BROKER="${TMP_DIR}/out_broker"

write_file "${AGENTD_ROOT}/api/v1/health" '{"ok":true}'
write_file "${AGENTD_ROOT}/api/v1/diagnostics" '{"ok":true}'
write_file "${BROKER_ROOT}/healthz" '{"ok":true}'
write_file "${BROKER_ROOT}/readyz" '{"ok":true}'

python3 -m http.server "${AGENTD_PORT}" --bind 127.0.0.1 --directory "${AGENTD_ROOT}" >/dev/null 2>&1 &
AGENTD_HTTP_PID=$!
python3 -m http.server "${BROKER_PORT}" --bind 127.0.0.1 --directory "${BROKER_ROOT}" >/dev/null 2>&1 &
BROKER_HTTP_PID=$!

cat > "${STATE_PATH}" <<JSON
{
  "agentd_base": "http://127.0.0.1:${AGENTD_PORT}",
  "broker_base": "http://127.0.0.1:${BROKER_PORT}",
  "agentd_port": ${AGENTD_PORT},
  "broker_port": ${BROKER_PORT},
  "broker_tls": false
}
JSON

for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:${AGENTD_PORT}/api/v1/health" >/dev/null 2>&1 \
    && curl -fsS "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

AGENT_DEVSTACK_STATE="${STATE_PATH}" python3 "${ROOT}/tools/scenario_runner.py" \
  --file "${ROOT}/tools/scenarios/agentd_smoke.json" \
  --out-dir "${OUT_AGENTD}" >/dev/null

AGENT_DEVSTACK_STATE="${STATE_PATH}" python3 "${ROOT}/tools/scenario_runner.py" \
  --file "${ROOT}/tools/scenarios/broker_smoke.json" \
  --out-dir "${OUT_BROKER}" >/dev/null

grep -q 'GET http://127.0.0.1:'"${AGENTD_PORT}"'/api/v1/health' "${OUT_AGENTD}/logs/01_agentd_health.log"
grep -q 'GET http://127.0.0.1:'"${BROKER_PORT}"'/healthz' "${OUT_BROKER}/logs/01_broker_healthz.log"
