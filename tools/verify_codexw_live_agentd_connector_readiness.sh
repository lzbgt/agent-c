#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CODEXW_ROOT="${CODEXW_ROOT:-$(cd "${ROOT}/.." && pwd)/codexw}"
BROKER_URL="${CODEXW_BROKER_BASE_URL:-https://broker.hubstack.cn}"
BROKER_ADMIN="${CODEXW_BROKER_ADMIN:-${CODEXW_ROOT}/scripts/broker-admin}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/tools/agentd_codexw_native_broker_connector.py}"
KEEP_DEPLOYMENT="${KEEP_DEPLOYMENT:-0}"
NAME="codexw_live_agentd_connector_readiness"

if [[ ! -x "${BROKER_ADMIN}" ]]; then
  echo "codexw broker-admin not found or not executable: ${BROKER_ADMIN}" >&2
  exit 2
fi
if [[ ! -f "${CONNECTOR_BIN}" ]]; then
  echo "agentd native connector script not found: ${CONNECTOR_BIN}" >&2
  exit 2
fi

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

json_field() {
  python3 -c 'import json,sys; obj=json.load(sys.stdin); cur=obj
for part in sys.argv[1].split("."):
    cur = cur[part]
print(cur)' "$1"
}

RUN_ID="$(date +%Y%m%d%H%M%S)"
DEPLOYMENT_ID="${DEPLOYMENT_ID:-agentd-readiness-proof-${RUN_ID}}"
RUNTIME_INSTANCE_ID="${RUNTIME_INSTANCE_ID:-agentd-readiness-${RUN_ID}}"
TOKEN_ID="${TOKEN_ID:-agentd-readiness-proof-${RUN_ID}}"
RUN_DIR="${RUN_DIR:-${ROOT}/build/${NAME}-${DEPLOYMENT_ID}}"
IDENTITY_DIR="${RUN_DIR}/native-identity"
AGENTD_PORT="$(pick_port)"
AGENTD_TOKEN="agentd-readiness-proof-${RUN_ID}"
AGENTD_URL="http://127.0.0.1:${AGENTD_PORT}"
SELF_TEST_STATUS_JSON="${RUN_DIR}/self-test-status.json"
FAKE_SERVER="${RUN_DIR}/fake_agentd.py"
FAKE_LOG="${RUN_DIR}/fake-agentd.log"
CONNECTOR_LOG="${RUN_DIR}/connector.log"
PROOF_JSON="${RUN_DIR}/proof.json"

mkdir -p "${RUN_DIR}" "${IDENTITY_DIR}"

FAKE_PID=""
CONNECTOR_PID=""

cleanup_connector() {
  if [[ -n "${CONNECTOR_PID}" ]]; then
    kill -TERM "${CONNECTOR_PID}" >/dev/null 2>&1 || true
    wait "${CONNECTOR_PID}" >/dev/null 2>&1 || true
    CONNECTOR_PID=""
  fi
}

cleanup_fake() {
  if [[ -n "${FAKE_PID}" ]]; then
    kill -TERM "${FAKE_PID}" >/dev/null 2>&1 || true
    wait "${FAKE_PID}" >/dev/null 2>&1 || true
    FAKE_PID=""
  fi
}

cleanup() {
  cleanup_connector
  cleanup_fake
  if [[ "${KEEP_DEPLOYMENT}" != "1" ]]; then
    "${BROKER_ADMIN}" --timeout-seconds 120 deployment-delete --deployment-id "${DEPLOYMENT_ID}" >/dev/null 2>&1 || true
    "${BROKER_ADMIN}" --timeout-seconds 120 enrollment-token-delete --id "${TOKEN_ID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cat >"${FAKE_SERVER}" <<'PY'
#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


TOKEN = os.environ["AGENTD_TOKEN"]


class Handler(BaseHTTPRequestHandler):
    server_version = "agentd-readiness-proof/1"

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _authorized(self) -> bool:
        return self.headers.get("Authorization") == f"Bearer {TOKEN}"

    def _send(self, status: int, payload: dict[str, object]) -> None:
        body = json.dumps(payload, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _require_auth(self) -> bool:
        if self._authorized():
            return True
        self._send(401, {"ok": False, "error": "unauthorized"})
        return False

    def do_GET(self) -> None:
        if not self._require_auth():
            return
        path = urlparse(self.path).path
        if path == "/api/v1/health":
            self._send(200, {"ok": True, "service": "fake-agentd-readiness-proof"})
            return
        if path == "/api/v1/caps":
            self._send(200, {"ok": True, "capabilities": {"codexw_readiness_proof": True}})
            return
        if path == "/api/v1/db/sessions":
            self._send(200, {"ok": True, "sessions": []})
            return
        if path == "/api/v1/db/workflows":
            self._send(200, {"ok": True, "workflows": []})
            return
        if path in {"/api/v1/db/client_events", "/api/v1/db/workflow_events"}:
            self._send(200, {"ok": True, "client_events": [], "events": []})
            return
        self._send(404, {"ok": False, "error": "not_found", "path": path})


def main() -> None:
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()


if __name__ == "__main__":
    main()
PY

AGENTD_TOKEN="${AGENTD_TOKEN}" python3 "${FAKE_SERVER}" "${AGENTD_PORT}" >"${FAKE_LOG}" 2>&1 &
FAKE_PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "fake agentd endpoint did not become healthy; log: ${FAKE_LOG}" >&2
  exit 1
fi

issue_payload="$("${BROKER_ADMIN}" --timeout-seconds 120 enrollment-token-issue \
  --id "${TOKEN_ID}" \
  --description "live agentd connector readiness status proof")"
TOKEN_SECRET="$(printf '%s' "${issue_payload}" | json_field token.shared_secret)"

seed_self_test_status() {
  python3 - <<PY
import json
import time
from pathlib import Path

payload = {
    "ok": False,
    "mode": "self_test",
    "checked_unix_ms": int(time.time() * 1000) - 120_000,
    "deployment_id": "${DEPLOYMENT_ID}",
    "runtime_instance_id": "${RUNTIME_INSTANCE_ID}",
    "runtime_kind": "agentd",
    "checks": [
        {"name": "identity_files", "ok": True},
        {"name": "agentd_health", "ok": True},
        {"name": "broker_runtime_instance_visible", "ok": False, "error": "seeded readiness proof failure"},
    ],
}
path = Path("${SELF_TEST_STATUS_JSON}")
path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
PY
}

start_connector() {
  local log_path="$1"
  shift || true
  AGENTD_BASE_URL="${AGENTD_URL}" \
  AGENTD_AUTH_TOKEN="${AGENTD_TOKEN}" \
  AGENTD_CODEXW_ENROLLMENT_TOKEN_ID="${TOKEN_ID}" \
  AGENTD_CODEXW_ENROLLMENT_SECRET="${TOKEN_SECRET}" \
  AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH="${SELF_TEST_STATUS_JSON}" \
  "${CONNECTOR_BIN}" \
    --broker-url "${BROKER_URL}" \
    --deployment-id "${DEPLOYMENT_ID}" \
    --display-name "${DEPLOYMENT_ID}" \
    --runtime-instance-id "${RUNTIME_INSTANCE_ID}" \
    --identity-dir "${IDENTITY_DIR}" \
    --agentd-base-url "${AGENTD_URL}" \
    --agentd-auth-token "${AGENTD_TOKEN}" \
    --self-test-output-path "${SELF_TEST_STATUS_JSON}" \
    --bootstrap-identity \
    --connect \
    "$@" \
    >"${log_path}" 2>&1 &
  CONNECTOR_PID=$!
}

start_connector "${CONNECTOR_LOG}.bootstrap"

for _ in $(seq 1 120); do
  if [[ -f "${IDENTITY_DIR}/deployment.cert.pem" ]]; then
    break
  fi
  sleep 0.25
done
if [[ ! -f "${IDENTITY_DIR}/deployment.cert.pem" ]]; then
  echo "connector did not enroll a deployment certificate; log: ${CONNECTOR_LOG}.bootstrap" >&2
  exit 1
fi

for _ in $(seq 1 60); do
  if "${BROKER_ADMIN}" --timeout-seconds 120 deployment-approve --deployment-id "${DEPLOYMENT_ID}" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done
"${BROKER_ADMIN}" --timeout-seconds 120 deployment-approve --deployment-id "${DEPLOYMENT_ID}" >/dev/null 2>&1 || true
cleanup_connector

seed_self_test_status
start_connector "${CONNECTOR_LOG}"

python3 - <<PY
import importlib.machinery
import importlib.util
import json
import time
import urllib.parse
from pathlib import Path

broker_admin = "${BROKER_ADMIN}"
loader = importlib.machinery.SourceFileLoader("broker_admin", broker_admin)
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)

args = mod.build_parser().parse_args(["me"])
mod.apply_local_defaults(args)
client = mod.BrokerAdminClient(
    base_url=args.base_url,
    server_id=args.server_id,
    client_id=args.client_id,
    shared_secret=args.shared_secret,
    timeout_seconds=120,
)
token = (args.token or "").strip() or client.login(args.username, mod.require_password(args.password))

deployment_id = "${DEPLOYMENT_ID}"
expected_instance_id = "${RUNTIME_INSTANCE_ID}"

instance = None
last_payload = {}
deadline = time.time() + 75
while time.time() < deadline:
    payload = client.request("GET", "/api/v2/runtime-instances", token=token)
    last_payload = payload
    for item in payload.get("runtime_instances", []):
        placement = item.get("placement") if isinstance(item.get("placement"), dict) else {}
        if item.get("runtime_kind") == "agentd" and (
            item.get("instance_id") == expected_instance_id or placement.get("deployment_id") == deployment_id
        ):
            instance = item
            break
    if instance and instance.get("connection", {}).get("state") == "online":
        break
    time.sleep(0.5)

if not instance:
    raise SystemExit("agentd readiness proof runtime instance did not appear; last=" + json.dumps(last_payload)[:2000])
if instance.get("connection", {}).get("state") != "online":
    raise SystemExit("agentd readiness proof runtime instance was not online: " + json.dumps(instance)[:2000])

instance_id = instance.get("instance_id") or expected_instance_id
path_id = urllib.parse.quote(instance_id, safe="")
status = None
for _ in range(30):
    status = client.request("GET", f"/api/v2/runtime-instances/{path_id}/status", token=token)
    connector = status.get("connector") if isinstance(status.get("connector"), dict) else {}
    if connector.get("state") == "failed" and connector.get("failed_check_count") == 1:
        break
    time.sleep(0.5)

connector = status.get("connector") if isinstance(status, dict) and isinstance(status.get("connector"), dict) else None
if not connector:
    raise SystemExit("broker status did not include connector object: " + json.dumps(status)[:2000])
if connector.get("source") != "agentd.codexw.self_test":
    raise SystemExit("unexpected connector source: " + json.dumps(connector, sort_keys=True))
if connector.get("state") != "failed" or connector.get("ok") is not False:
    raise SystemExit("unexpected connector readiness state: " + json.dumps(connector, sort_keys=True))
if connector.get("failed_check_count") != 1:
    raise SystemExit("unexpected failed_check_count: " + json.dumps(connector, sort_keys=True))
if "broker_runtime_instance_visible" not in connector.get("failed_checks", []):
    raise SystemExit("missing failed check name: " + json.dumps(connector, sort_keys=True))
if int(connector.get("checked_unix_ms") or 0) <= 0:
    raise SystemExit("missing checked_unix_ms: " + json.dumps(connector, sort_keys=True))
if int(connector.get("age_ms") or 0) < 0:
    raise SystemExit("invalid age_ms: " + json.dumps(connector, sort_keys=True))

proof = {
    "ok": True,
    "deployment_id": deployment_id,
    "runtime_instance_id": instance_id,
    "runtime_kind": instance.get("runtime_kind"),
    "connection_state": instance.get("connection", {}).get("state"),
    "connector_source": connector.get("source"),
    "connector_state": connector.get("state"),
    "connector_ok": connector.get("ok"),
    "connector_checked_unix_ms": connector.get("checked_unix_ms"),
    "connector_age_ms": connector.get("age_ms"),
    "connector_failed_check_count": connector.get("failed_check_count"),
    "connector_failed_checks": connector.get("failed_checks", []),
    "update_state": (status.get("update") or {}).get("state"),
}
Path("${PROOF_JSON}").write_text(json.dumps(proof, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
print(json.dumps(proof, indent=2, sort_keys=True))
PY

echo "${NAME} OK"
