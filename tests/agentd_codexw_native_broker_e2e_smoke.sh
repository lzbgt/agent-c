#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

CODEXW_ROOT="${CODEXW_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)/codexw}"
if [[ ! -f "${CODEXW_ROOT}/broker/go.mod" ]]; then
  echo "SKIP: codexw sibling broker repo not found at ${CODEXW_ROOT}" >&2
  exit 0
fi
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go command not found" >&2
  exit 0
fi

HOST="127.0.0.1"
BROKER_PORT="$(agentd_smoke_pick_port)"
AGENTD_PORT="$(agentd_smoke_pick_port)"
NAME="agentd_codexw_native_broker_e2e_smoke"
LOG_DIR="$(agentd_smoke_log_dir)"
RUN_DIR="${LOG_DIR}/${NAME}_${BROKER_PORT}"
BROKER_CONFIG="${RUN_DIR}/broker/config.json"
BROKER_LOG="${RUN_DIR}/broker.log"
LAUNCHER_LOG="${RUN_DIR}/launcher.log"
ADMIN_USER="admin"
ADMIN_PASSWORD="agentd-native-e2e-password"
DEPLOYMENT_ID="agentd-native-e2e-${BROKER_PORT}"
BROKER_URL="http://${HOST}:${BROKER_PORT}"

mkdir -p "${RUN_DIR}/broker"

cleanup() {
  if [[ -n "${LAUNCHER_PID:-}" ]]; then
    kill -TERM "${LAUNCHER_PID}" >/dev/null 2>&1 || true
    wait "${LAUNCHER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill -TERM "${BROKER_PID}" >/dev/null 2>&1 || true
    wait "${BROKER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

(
  cd "${CODEXW_ROOT}/broker"
  go run ./cmd/codexw-broker \
    --setup \
    --setup-overwrite \
    --config "${BROKER_CONFIG}" \
    --listen "${HOST}:${BROKER_PORT}" \
    --bootstrap-username "${ADMIN_USER}" \
    --bootstrap-password "${ADMIN_PASSWORD}" \
    --bootstrap-password-change-required=false \
    >"${RUN_DIR}/broker_setup.json"
)

(
  cd "${CODEXW_ROOT}/broker"
  go run ./cmd/codexw-broker \
    --config "${BROKER_CONFIG}" \
    --listen "${HOST}:${BROKER_PORT}" \
    >"${BROKER_LOG}" 2>&1
) &
BROKER_PID=$!

for _ in $(seq 1 160); do
  if curl -fsS --noproxy "*" \
    -H "Content-Type: application/json" \
    -d "{\"username\":\"${ADMIN_USER}\",\"password\":\"${ADMIN_PASSWORD}\"}" \
    "${BROKER_URL}/api/v1/auth/login" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

if ! curl -fsS --noproxy "*" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"${ADMIN_USER}\",\"password\":\"${ADMIN_PASSWORD}\"}" \
  "${BROKER_URL}/api/v1/auth/login" >/dev/null 2>&1; then
  echo "codexw broker did not become ready; log follows" >&2
  cat "${BROKER_LOG}" >&2 || true
  exit 1
fi

"${SCRIPT_DIR}/../tools/run_agentd_codexw_compat.sh" \
  --broker-mode native \
  --broker-url "${BROKER_URL}" \
  -u "${ADMIN_USER}" \
  -p "${ADMIN_PASSWORD}" \
  --deployment-id "${DEPLOYMENT_ID}" \
  --agentd-bin "${AGENTD_BIN}" \
  --agentd-port "${AGENTD_PORT}" \
  --state-dir "${RUN_DIR}/agentd-state" \
  --db-path "${RUN_DIR}/agentd.sqlite" \
  --native-identity-dir "${RUN_DIR}/native-identity" \
  --native-no-reconnect \
  >"${LAUNCHER_LOG}" 2>&1 &
LAUNCHER_PID=$!

python3 - <<PY
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

broker_url = "${BROKER_URL}"
admin_user = "${ADMIN_USER}"
admin_password = "${ADMIN_PASSWORD}"
deployment_id = "${DEPLOYMENT_ID}"


def request(method, path, body=None, token=""):
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(broker_url + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=5) as resp:
        raw = resp.read()
    return json.loads(raw.decode()) if raw else {}


token = request("POST", "/api/v1/auth/login", {"username": admin_user, "password": admin_password})["token"]
instance = None
deadline = time.time() + 30
while time.time() < deadline:
    try:
        payload = request("GET", "/api/v2/runtime-instances", token=token)
    except urllib.error.HTTPError:
        time.sleep(0.2)
        continue
    for candidate in payload.get("runtime_instances", []):
        if candidate.get("runtime_kind") == "agentd" and candidate.get("placement", {}).get("deployment_id") == deployment_id:
            instance = candidate
            break
    if instance is not None:
        break
    time.sleep(0.2)

if instance is None:
    print("native agentd runtime instance did not appear", file=sys.stderr)
    raise SystemExit(1)
instance_id = instance["instance_id"]
if instance.get("connection", {}).get("state") != "online":
    print(f"expected online instance, got {instance}", file=sys.stderr)
    raise SystemExit(1)

path_id = urllib.parse.quote(instance_id, safe="")
caps = request("GET", f"/api/v2/runtime-instances/{path_id}/capabilities", token=token)
actions = caps.get("capabilities", {}).get("actions", {})
if "experience.list" not in actions or "workflow.submit" not in actions:
    print(f"missing agentd actions: {caps}", file=sys.stderr)
    raise SystemExit(1)

result = request(
    "POST",
    f"/api/v2/runtime-instances/{path_id}/actions",
    {"action": "experience.list", "input": {"limit": 1}},
    token=token,
)
if not result.get("ok") or result.get("action") != "experience.list":
    print(f"bad runtime action broker response: {result}", file=sys.stderr)
    raise SystemExit(1)
body = result.get("result") or {}
if not isinstance(body, dict) or not body.get("ok", False):
    print(f"bad agentd action result body: {result}", file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_codexw_native_broker_e2e_smoke OK"
