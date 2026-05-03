#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CODEXW_ROOT="${CODEXW_ROOT:-$(cd "${ROOT}/.." && pwd)/codexw}"
BROKER_URL="${CODEXW_BROKER_BASE_URL:-https://broker.hubstack.cn}"
BROKER_ADMIN="${CODEXW_BROKER_ADMIN:-${CODEXW_ROOT}/scripts/broker-admin}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/tools/agentd_codexw_native_broker_connector.py}"
KEEP_DEPLOYMENT="${KEEP_DEPLOYMENT:-0}"
NAME="codexw_live_agentd_ota_candidate"

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
DEPLOYMENT_ID="${DEPLOYMENT_ID:-agentd-ota-candidate-proof-${RUN_ID}}"
RUNTIME_INSTANCE_ID="${RUNTIME_INSTANCE_ID:-agentd-ota-candidate-${RUN_ID}}"
TOKEN_ID="${TOKEN_ID:-agentd-ota-candidate-proof-${RUN_ID}}"
RUN_DIR="${RUN_DIR:-${ROOT}/build/${NAME}-${DEPLOYMENT_ID}}"
IDENTITY_DIR="${RUN_DIR}/native-identity"
AGENTD_PORT="$(pick_port)"
AGENTD_TOKEN="agentd-ota-candidate-proof-${RUN_ID}"
AGENTD_URL="http://127.0.0.1:${AGENTD_PORT}"
CANDIDATE_URL="${CANDIDATE_URL:-https://example.invalid/agentd/ota/live-proof/${RUN_ID}/agentd.tar.zst}"
CANDIDATE_SHA256="${CANDIDATE_SHA256:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"
CANDIDATE_VERSION="${CANDIDATE_VERSION:-agentd-ota-proof-${RUN_ID}}"
CANDIDATE_DRAIN_TIMEOUT_MS="${CANDIDATE_DRAIN_TIMEOUT_MS:-17000}"
OPERATOR_DRAIN_TIMEOUT_MS="${OPERATOR_DRAIN_TIMEOUT_MS:-23000}"
OPERATOR_REASON="${OPERATOR_REASON:-live non-mutating agentd OTA candidate proof}"
FAKE_SERVER="${RUN_DIR}/fake_agentd_ota.py"
FAKE_LOG="${RUN_DIR}/fake-agentd-ota.log"
CONNECTOR_LOG="${RUN_DIR}/connector.log"
UPDATE_BODY_JSON="${RUN_DIR}/ota-update-body.json"
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
UPDATE_BODY_JSON = Path(os.environ["UPDATE_BODY_JSON"])
CANDIDATE = {
    "url": os.environ["CANDIDATE_URL"],
    "sha256": os.environ["CANDIDATE_SHA256"],
    "version": os.environ["CANDIDATE_VERSION"],
    "channel": "live-proof",
    "target_os": "darwin",
    "target_arch": "arm64",
    "reason": "fake OTA candidate exposed for broker proof",
    "drain_timeout_ms": int(os.environ["CANDIDATE_DRAIN_TIMEOUT_MS"]),
}


class Handler(BaseHTTPRequestHandler):
    server_version = "agentd-ota-proof/1"

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
            self._send(200, {"ok": True, "service": "fake-agentd-ota-proof"})
            return
        if path == "/api/v1/caps":
            self._send(200, {"ok": True, "capabilities": {"ota": True, "codexw_live_proof": True}})
            return
        if path == "/api/v1/ota/status":
            self._send(
                200,
                {
                    "ok": True,
                    "enabled": True,
                    "state": "ready",
                    "detail": "fake OTA endpoint; update accepts but does not mutate host state",
                    "drain_active": False,
                    "jobs_running": 0,
                    "jobs_queued": 0,
                    "workflows_running": 0,
                    "workflow_tasks_running": 0,
                    "workflow_tasks_queued": 0,
                    "candidate": CANDIDATE,
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
        if path != "/api/v1/ota/update":
            self._send(404, {"ok": False, "error": "not_found", "path": path})
            return
        proof = {
            "ok": True,
            "accepted": True,
            "non_mutating_proof": True,
            "received_at_unix_ms": int(time.time() * 1000),
            "body": body,
        }
        tmp = UPDATE_BODY_JSON.with_suffix(".tmp")
        tmp.write_text(json.dumps(proof, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        tmp.replace(UPDATE_BODY_JSON)
        self._send(200, proof)


def main() -> None:
    port = int(sys.argv[1])
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
PY

AGENTD_TOKEN="${AGENTD_TOKEN}" \
UPDATE_BODY_JSON="${UPDATE_BODY_JSON}" \
CANDIDATE_URL="${CANDIDATE_URL}" \
CANDIDATE_SHA256="${CANDIDATE_SHA256}" \
CANDIDATE_VERSION="${CANDIDATE_VERSION}" \
CANDIDATE_DRAIN_TIMEOUT_MS="${CANDIDATE_DRAIN_TIMEOUT_MS}" \
python3 "${FAKE_SERVER}" "${AGENTD_PORT}" >"${FAKE_LOG}" 2>&1 &
FAKE_PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "fake agentd OTA endpoint did not become healthy; log: ${FAKE_LOG}" >&2
  exit 1
fi

issue_payload="$("${BROKER_ADMIN}" --timeout-seconds 120 enrollment-token-issue \
  --id "${TOKEN_ID}" \
  --description "live non-mutating agentd OTA candidate proof")"
TOKEN_SECRET="$(printf '%s' "${issue_payload}" | json_field token.shared_secret)"

start_connector() {
  local log_path="$1"
  shift
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
    --runtime-update-mode agentd_ota \
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
candidate_url = "${CANDIDATE_URL}"
candidate_sha256 = "${CANDIDATE_SHA256}"
candidate_version = "${CANDIDATE_VERSION}"
operator_reason = "${OPERATOR_REASON}"
operator_drain_timeout_ms = int("${OPERATOR_DRAIN_TIMEOUT_MS}")

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
    raise SystemExit("agentd OTA proof runtime instance did not appear; last=" + json.dumps(last_payload)[:2000])
if instance.get("connection", {}).get("state") != "online":
    raise SystemExit("agentd OTA proof runtime instance not online: " + json.dumps(instance)[:2000])

instance_id = instance.get("instance_id") or expected_instance_id
path_id = urllib.parse.quote(instance_id, safe="")

caps = client.request("GET", f"/api/v2/runtime-instances/{path_id}/capabilities", token=token)
actions = client.request("GET", f"/api/v2/runtime-instances/{path_id}/actions", token=token)
action_rows = actions.get("actions") if isinstance(actions.get("actions"), list) else []
update_action = next((row for row in action_rows if row.get("action") == "runtime.update"), None)
if not update_action:
    raise SystemExit("runtime.update action descriptor missing: " + json.dumps(actions)[:2000])
if not update_action.get("enabled"):
    raise SystemExit("runtime.update action descriptor is not enabled: " + json.dumps(update_action)[:2000])
if update_action.get("safe_boundary") != "idle":
    raise SystemExit("runtime.update safe boundary mismatch: " + json.dumps(update_action)[:2000])
runtime_caps = caps.get("capabilities") if isinstance(caps.get("capabilities"), dict) else {}
runtime_actions = runtime_caps.get("actions") if isinstance(runtime_caps.get("actions"), dict) else {}
runtime_update_capability = (
    runtime_actions.get("runtime.update") if isinstance(runtime_actions.get("runtime.update"), dict) else {}
)
if runtime_update_capability.get("safe_boundary") != "agentd_ota_drain":
    raise SystemExit(
        "runtime.update capability safe boundary mismatch: " + json.dumps(runtime_update_capability)[:2000]
    )

status = client.request("GET", f"/api/v2/runtime-instances/{path_id}/status", token=token)
update = status.get("update") if isinstance(status.get("update"), dict) else {}
candidate = update.get("candidate") if isinstance(update.get("candidate"), dict) else {}
if not update.get("enabled"):
    raise SystemExit("runtime status did not enable update: " + json.dumps(status)[:2000])
if candidate.get("url") != candidate_url:
    raise SystemExit("runtime status candidate url mismatch: " + json.dumps(status)[:2000])
if candidate.get("sha256") != candidate_sha256:
    raise SystemExit("runtime status candidate sha256 mismatch: " + json.dumps(status)[:2000])
if candidate.get("version") != candidate_version:
    raise SystemExit("runtime status candidate version mismatch: " + json.dumps(status)[:2000])

idempotency_key = "agentd-ota-candidate-proof-${RUN_ID}"
action_response = client.request(
    "POST",
    f"/api/v2/runtime-instances/{path_id}/actions",
    {
        "action": "runtime.update",
        "confirmed": True,
        "idempotency_key": idempotency_key,
        "input": {
            "reason": operator_reason,
            "drain_timeout_ms": operator_drain_timeout_ms,
        },
    },
    token=token,
)
if not action_response.get("ok"):
    raise SystemExit("runtime.update action response was not ok: " + json.dumps(action_response)[:2000])

update_body_path = Path("${UPDATE_BODY_JSON}")
deadline = time.time() + 20
while time.time() < deadline:
    if update_body_path.exists():
        break
    time.sleep(0.25)
if not update_body_path.exists():
    raise SystemExit("fake OTA endpoint did not receive update request")
update_record = json.loads(update_body_path.read_text(encoding="utf-8"))
forwarded = update_record.get("body") if isinstance(update_record.get("body"), dict) else {}
expected = {
    "url": candidate_url,
    "sha256": candidate_sha256,
    "version": candidate_version,
    "reason": operator_reason,
    "drain_timeout_ms": operator_drain_timeout_ms,
}
for key, value in expected.items():
    if forwarded.get(key) != value:
        raise SystemExit(
            f"forwarded OTA update body mismatch for {key}: "
            + json.dumps({"expected": expected, "forwarded": forwarded}, sort_keys=True)
        )

audit = client.request("GET", f"/api/v2/runtime-instances/{path_id}/audit?limit=8", token=token)
audit_events = audit.get("events") if isinstance(audit.get("events"), list) else []
if not any(event.get("action") == "runtime_instance.action" and event.get("outcome") == "success" for event in audit_events):
    raise SystemExit("runtime action success audit event missing: " + json.dumps(audit)[:2000])

proof = {
    "ok": True,
    "deployment_id": deployment_id,
    "runtime_instance_id": instance_id,
    "runtime_kind": instance.get("runtime_kind"),
    "connection_state": instance.get("connection", {}).get("state"),
    "runtime_update_enabled": update_action.get("enabled"),
    "broker_safe_boundary": update_action.get("safe_boundary"),
    "runtime_safe_boundary": runtime_update_capability.get("safe_boundary"),
    "candidate": candidate,
    "idempotency_key": idempotency_key,
    "forwarded_update_body": forwarded,
    "action_response": action_response,
    "audit_events_returned": len(audit_events),
}
Path("${PROOF_JSON}").write_text(json.dumps(proof, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
print(json.dumps(proof, indent=2, sort_keys=True))
PY

echo "${NAME} OK"
