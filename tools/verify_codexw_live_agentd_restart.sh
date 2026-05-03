#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CODEXW_ROOT="${CODEXW_ROOT:-$(cd "${ROOT}/.." && pwd)/codexw}"
BROKER_URL="${CODEXW_BROKER_BASE_URL:-https://broker.hubstack.cn}"
BROKER_ADMIN="${CODEXW_BROKER_ADMIN:-${CODEXW_ROOT}/scripts/broker-admin}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/tools/agentd_codexw_native_broker_connector.py}"
KEEP_DEPLOYMENT="${KEEP_DEPLOYMENT:-0}"
NAME="codexw_live_agentd_restart"

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
DEPLOYMENT_ID="${DEPLOYMENT_ID:-agentd-restart-proof-${RUN_ID}}"
RUNTIME_INSTANCE_ID="${RUNTIME_INSTANCE_ID:-agentd-restart-${RUN_ID}}"
TOKEN_ID="${TOKEN_ID:-agentd-restart-proof-${RUN_ID}}"
RUN_DIR="${RUN_DIR:-${ROOT}/build/${NAME}-${DEPLOYMENT_ID}}"
IDENTITY_DIR="${RUN_DIR}/native-identity"
AGENTD_PORT="$(pick_port)"
AGENTD_TOKEN="agentd-restart-proof-${RUN_ID}"
AGENTD_URL="http://127.0.0.1:${AGENTD_PORT}"
RESTART_REASON="${RESTART_REASON:-live dry-run agentd restart proof}"
RESTART_DRAIN_TIMEOUT_MS="${RESTART_DRAIN_TIMEOUT_MS:-21000}"
FAKE_SERVER="${RUN_DIR}/fake_agentd_restart.py"
FAKE_LOG="${RUN_DIR}/fake-agentd-restart.log"
CONNECTOR_LOG="${RUN_DIR}/connector.log"
RESTART_BODY_JSON="${RUN_DIR}/ota-restart-body.json"
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
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


TOKEN = os.environ["AGENTD_TOKEN"]
RESTART_BODY_JSON = Path(os.environ["RESTART_BODY_JSON"])


class Handler(BaseHTTPRequestHandler):
    server_version = "agentd-restart-proof/1"

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
            self._send(200, {"ok": True, "service": "fake-agentd-restart-proof"})
            return
        if path == "/api/v1/caps":
            self._send(200, {"ok": True, "capabilities": {"ota_restart": True, "codexw_live_proof": True}})
            return
        if path == "/api/v1/ota/status":
            self._send(
                200,
                {
                    "ok": True,
                    "enabled": False,
                    "state": "ready",
                    "detail": "fake restart endpoint; restart accepts but does not mutate host state",
                    "restart": {
                        "source": "agentd.ota.restart",
                        "available": True,
                        "enabled": True,
                        "state": "ready",
                        "detail": "fake dry-run supervisor restart boundary",
                        "safe_boundary": "agentd_supervisor_restart_drain",
                        "method": "systemd",
                        "service": "agentd-restart-proof",
                        "dry_run": True,
                        "drain_timeout_ms": 15000,
                    },
                },
            )
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

    def do_POST(self) -> None:
        if not self._require_auth():
            return
        path = urlparse(self.path).path
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0") or "0"))
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            self._send(400, {"ok": False, "error": "invalid_json"})
            return
        if path != "/api/v1/ota/restart":
            self._send(404, {"ok": False, "error": "not_found", "path": path})
            return
        proof = {
            "ok": True,
            "accepted": True,
            "operation": "restart",
            "status": "queued",
            "non_mutating_proof": True,
            "received_at_unix_ms": int(time.time() * 1000),
            "body": body,
        }
        tmp = RESTART_BODY_JSON.with_suffix(".tmp")
        tmp.write_text(json.dumps(proof, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        tmp.replace(RESTART_BODY_JSON)
        self._send(200, proof)


def main() -> None:
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()


if __name__ == "__main__":
    main()
PY

AGENTD_TOKEN="${AGENTD_TOKEN}" \
RESTART_BODY_JSON="${RESTART_BODY_JSON}" \
python3 "${FAKE_SERVER}" "${AGENTD_PORT}" >"${FAKE_LOG}" 2>&1 &
FAKE_PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "fake agentd restart endpoint did not become healthy; log: ${FAKE_LOG}" >&2
  exit 1
fi

issue_payload="$("${BROKER_ADMIN}" --timeout-seconds 120 enrollment-token-issue \
  --id "${TOKEN_ID}" \
  --description "live dry-run agentd restart proof")"
TOKEN_SECRET="$(printf '%s' "${issue_payload}" | json_field token.shared_secret)"

start_connector() {
  local log_path="$1"
  shift || true
  AGENTD_BASE_URL="${AGENTD_URL}" \
  AGENTD_AUTH_TOKEN="${AGENTD_TOKEN}" \
  AGENTD_CODEXW_ENROLLMENT_TOKEN_ID="${TOKEN_ID}" \
  AGENTD_CODEXW_ENROLLMENT_SECRET="${TOKEN_SECRET}" \
  "${CONNECTOR_BIN}" \
    --broker-url "${BROKER_URL}" \
    --deployment-id "${DEPLOYMENT_ID}" \
    --display-name "${DEPLOYMENT_ID}" \
    --runtime-instance-id "${RUNTIME_INSTANCE_ID}" \
    --identity-dir "${IDENTITY_DIR}" \
    --agentd-base-url "${AGENTD_URL}" \
    --agentd-auth-token "${AGENTD_TOKEN}" \
    --runtime-restart-mode agentd_ota \
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
restart_reason = "${RESTART_REASON}"
restart_drain_timeout_ms = int("${RESTART_DRAIN_TIMEOUT_MS}")

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
    raise SystemExit("agentd restart proof runtime instance did not appear; last=" + json.dumps(last_payload)[:2000])
if instance.get("connection", {}).get("state") != "online":
    raise SystemExit("agentd restart proof runtime instance not online: " + json.dumps(instance)[:2000])

instance_id = instance.get("instance_id") or expected_instance_id
path_id = urllib.parse.quote(instance_id, safe="")

caps = client.request("GET", f"/api/v2/runtime-instances/{path_id}/capabilities", token=token)
actions = client.request("GET", f"/api/v2/runtime-instances/{path_id}/actions", token=token)
action_rows = actions.get("actions") if isinstance(actions.get("actions"), list) else []
restart_action = next((row for row in action_rows if row.get("action") == "runtime.restart"), None)
if not restart_action:
    raise SystemExit("runtime.restart action descriptor missing: " + json.dumps(actions)[:2000])
if not restart_action.get("enabled"):
    raise SystemExit("runtime.restart action descriptor is not enabled: " + json.dumps(restart_action)[:2000])
if restart_action.get("safe_boundary") != "idle":
    raise SystemExit("runtime.restart broker safe boundary mismatch: " + json.dumps(restart_action)[:2000])
runtime_caps = caps.get("capabilities") if isinstance(caps.get("capabilities"), dict) else {}
runtime_actions = runtime_caps.get("actions") if isinstance(runtime_caps.get("actions"), dict) else {}
runtime_restart_capability = (
    runtime_actions.get("runtime.restart") if isinstance(runtime_actions.get("runtime.restart"), dict) else {}
)
if runtime_restart_capability.get("safe_boundary") != "agentd_supervisor_restart_drain":
    raise SystemExit(
        "runtime.restart capability safe boundary mismatch: " + json.dumps(runtime_restart_capability)[:2000]
    )

status = client.request("GET", f"/api/v2/runtime-instances/{path_id}/status", token=token)
restart = status.get("restart") if isinstance(status.get("restart"), dict) else {}
if not restart.get("enabled"):
    raise SystemExit("runtime status did not enable restart: " + json.dumps(status)[:2000])
if restart.get("safe_boundary") != "agentd_supervisor_restart_drain":
    raise SystemExit("runtime status restart safe boundary mismatch: " + json.dumps(status)[:2000])
if restart.get("method") != "systemd" or restart.get("service") != "agentd-restart-proof":
    raise SystemExit("runtime status restart supervisor identity mismatch: " + json.dumps(status)[:2000])

idempotency_key = "agentd-restart-proof-${RUN_ID}"
preflight_response = client.request(
    "POST",
    f"/api/v2/runtime-instances/{path_id}/actions/preflight",
    {
        "action": "runtime.restart",
        "input": {
            "reason": restart_reason,
            "drain_timeout_ms": restart_drain_timeout_ms,
        },
    },
    token=token,
)
if not preflight_response.get("ok") or not preflight_response.get("preview"):
    raise SystemExit("runtime.restart preflight response was not ok: " + json.dumps(preflight_response)[:2000])
if preflight_response.get("mutates_runtime"):
    raise SystemExit("runtime.restart preflight must be non-mutating: " + json.dumps(preflight_response)[:2000])

action_body = {
    "action": "runtime.restart",
    "confirmed": True,
    "idempotency_key": idempotency_key,
    "input": {
        "reason": restart_reason,
        "drain_timeout_ms": restart_drain_timeout_ms,
    },
}
action_response = client.request("POST", f"/api/v2/runtime-instances/{path_id}/actions", action_body, token=token)
if not action_response.get("ok"):
    raise SystemExit("runtime.restart action response was not ok: " + json.dumps(action_response)[:2000])
replay_response = client.request("POST", f"/api/v2/runtime-instances/{path_id}/actions", action_body, token=token)
if not replay_response.get("ok") or not replay_response.get("replayed"):
    raise SystemExit("runtime.restart idempotency replay failed: " + json.dumps(replay_response)[:2000])

restart_body_path = Path("${RESTART_BODY_JSON}")
deadline = time.time() + 20
while time.time() < deadline:
    if restart_body_path.exists():
        break
    time.sleep(0.25)
if not restart_body_path.exists():
    raise SystemExit("fake OTA endpoint did not receive restart request")
restart_record = json.loads(restart_body_path.read_text(encoding="utf-8"))
forwarded = restart_record.get("body") if isinstance(restart_record.get("body"), dict) else {}
expected_forwarded = {
    "reason": restart_reason,
    "idempotency_key": idempotency_key,
    "drain_timeout_ms": restart_drain_timeout_ms,
}
for key, value in expected_forwarded.items():
    if forwarded.get(key) != value:
        raise SystemExit(
            f"forwarded restart body mismatch for {key}: "
            + json.dumps({"expected": expected_forwarded, "forwarded": forwarded}, sort_keys=True)
        )

audit = client.request("GET", f"/api/v2/runtime-instances/{path_id}/audit?limit=8", token=token)
audit_events = audit.get("events") if isinstance(audit.get("events"), list) else []
if not any(event.get("action") == "runtime_instance.action" and event.get("outcome") == "success" for event in audit_events):
    raise SystemExit("runtime restart success audit event missing: " + json.dumps(audit)[:2000])
if not any(event.get("action") == "runtime_instance.action" and event.get("outcome") == "replayed" for event in audit_events):
    raise SystemExit("runtime restart replay audit event missing: " + json.dumps(audit)[:2000])

proof = {
    "ok": True,
    "deployment_id": deployment_id,
    "runtime_instance_id": instance_id,
    "runtime_kind": instance.get("runtime_kind"),
    "connection_state": instance.get("connection", {}).get("state"),
    "runtime_restart_enabled": restart_action.get("enabled"),
    "broker_safe_boundary": restart_action.get("safe_boundary"),
    "runtime_safe_boundary": runtime_restart_capability.get("safe_boundary"),
    "restart_status": restart,
    "idempotency_key": idempotency_key,
    "preflight_response": preflight_response,
    "forwarded_restart_body": forwarded,
    "action_response": action_response,
    "replay_response": replay_response,
    "audit_events_returned": len(audit_events),
}
Path("${PROOF_JSON}").write_text(json.dumps(proof, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
print(json.dumps(proof, indent=2, sort_keys=True))
PY

echo "${NAME} OK"
