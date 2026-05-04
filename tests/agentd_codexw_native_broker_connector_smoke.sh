#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agentd-codexw-native.XXXXXX")"
cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

KEY_PATH="${TMP_DIR}/deployment.key.pem"
CERT_PATH="${TMP_DIR}/deployment.cert.pem"
OUT_PATH="${TMP_DIR}/dry-run.json"

openssl ecparam -name prime256v1 -genkey -noout -out "${KEY_PATH}" >/dev/null 2>&1
openssl req -new -x509 \
  -key "${KEY_PATH}" \
  -out "${CERT_PATH}" \
  -days 1 \
  -subj "/CN=agentd-native-smoke" \
  >/dev/null 2>&1

"${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py" \
  --broker-url "http://127.0.0.1:8787" \
  --deployment-id "agentd-native-smoke" \
  --display-name "agentd native smoke" \
  --runtime-instance-id "agentd-native-smoke-instance" \
  --deployment-cert-path "${CERT_PATH}" \
  --deployment-key-path "${KEY_PATH}" \
  --agentd-base-url "http://127.0.0.1:18080" \
  --timestamp 1700000000 \
  --dry-run \
  >"${OUT_PATH}"

python3 - <<PY
import base64
import json
import re
import sys
from pathlib import Path

payload = json.loads(Path("${OUT_PATH}").read_text())
headers = payload["connect_headers"]
runtime = payload["runtime_snapshot"]["runtime"]
frame = payload["deployment_snapshot_frame"]

if payload.get("mode") != "dry_run":
    print("bad mode", payload, file=sys.stderr)
    raise SystemExit(1)
if payload.get("runtime_capabilities_hash") != payload.get("runtime_capabilities_canonical_json_sha256"):
    print("hash mismatch", payload, file=sys.stderr)
    raise SystemExit(1)
if not re.fullmatch(r"[0-9a-f]{64}", payload.get("runtime_capabilities_hash", "")):
    print("bad capability hash", payload, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Runtime-Kind") != "agentd":
    print("bad runtime kind header", headers, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Runtime-Instance-Id") != "agentd-native-smoke-instance":
    print("bad instance header", headers, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Runtime-Capabilities-SHA256") != payload["runtime_capabilities_hash"]:
    print("bad capability header", headers, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Deployment-Id") != "agentd-native-smoke":
    print("bad deployment id header", headers, file=sys.stderr)
    raise SystemExit(1)
for key in ("X-Codexw-Deployment-Certificate", "X-Codexw-Deployment-Certificate-Signature"):
    try:
        base64.b64decode(headers.get(key, ""), validate=True)
    except Exception as exc:
        print(f"bad base64 header {key}: {exc}", headers, file=sys.stderr)
        raise SystemExit(1)
if runtime.get("kind") != "agentd" or runtime.get("runtime_kind") != "agentd":
    print("bad runtime snapshot", runtime, file=sys.stderr)
    raise SystemExit(1)
if runtime.get("runtime_capabilities", {}).get("schema") != "broker.runtime_capabilities.v1":
    print("bad runtime capabilities", runtime, file=sys.stderr)
    raise SystemExit(1)
if frame.get("type") != "deployment.snapshot" or frame.get("deployment_id") != "agentd-native-smoke":
    print("bad snapshot frame", frame, file=sys.stderr)
    raise SystemExit(1)
for action in ("workflow.submit", "voice.webrtc_peer.status", "voice.webrtc_peer.start", "voice.webrtc_peer.stop"):
    if action not in payload["runtime_capabilities"]["actions"]:
        print("missing runtime action", action, payload["runtime_capabilities"], file=sys.stderr)
        raise SystemExit(1)
for surface in ("sessions", "session_create", "events", "status", "files", "shell", "voice_webrtc_peer", "proxy_http"):
    if payload["runtime_capabilities"].get("surfaces", {}).get(surface) is not True:
        print("missing runtime surface", surface, payload["runtime_capabilities"], file=sys.stderr)
        raise SystemExit(1)
media = payload["runtime_capabilities"].get("media", {})
if media.get("direct_p2p") is not False or media.get("video") is not False or media.get("audio") is not False:
    print("agentd connector must explicitly advertise media unsupported", payload["runtime_capabilities"], file=sys.stderr)
    raise SystemExit(1)
if "direct P2P media streaming is not implemented" not in str(media.get("reason", "")):
    print("agentd media boundary reason missing", media, file=sys.stderr)
    raise SystemExit(1)
if payload["runtime_capabilities"].get("surfaces", {}).get("proxy_sse") is True:
    print("native deployment-connect must not advertise local-API SSE proxy", payload["runtime_capabilities"], file=sys.stderr)
    raise SystemExit(1)
for forbidden in ("runtime.restart", "runtime.update", "runtime.upgrade"):
    if forbidden in payload["runtime_capabilities"]["actions"]:
        print("agentd connector must not advertise unsafe operator action", forbidden, payload["runtime_capabilities"], file=sys.stderr)
        raise SystemExit(1)
PY

printf 'agentd native file smoke\n' >"${TMP_DIR}/file-smoke.txt"
python3 - <<PY
import importlib.util
import json
import sys
from pathlib import Path

script = Path("${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py")
spec = importlib.util.spec_from_file_location("agentd_codexw_native_broker_connector", script)
module = importlib.util.module_from_spec(spec)
sys.path.insert(0, str(script.parent))
spec.loader.exec_module(module)
args = module.parse_args([
    "--broker-url", "http://127.0.0.1:8787",
    "--deployment-id", "agentd-native-smoke",
    "--runtime-instance-id", "agentd-native-smoke-instance",
    "--deployment-cert-path", "${CERT_PATH}",
    "--deployment-key-path", "${KEY_PATH}",
    "--agentd-base-url", "http://127.0.0.1:18080",
    "--broker-token", "broker-read-token",
    "--file-root", "${TMP_DIR}",
    "--dry-run",
])

def command(method, path, body=None):
    return module.handle_command(args, {
        "request_id": f"req-{method}-{path}",
        "method": method,
        "path": path,
        "body": body,
    })

captured_agentd = []
def fake_agentd_request(args, method, path, body=None):
    captured_agentd.append({"method": method, "path": path, "body": body})
    if method == "POST" and path == "/api/v1/session/new":
        return {"ok": True, "session_id": (body or {}).get("session_id"), "created": False}
    if method == "GET" and path.startswith("/api/v1/session/voice_webrtc_peer?"):
        return {"ok": True, "peer": {"running": False, "native_media_active": False}}
    if method == "POST" and path == "/api/v1/session/voice_webrtc_peer":
        return {"ok": True, "peer": {"running": body.get("action") == "start"}, "echo": body}
    raise AssertionError(f"unexpected agentd request: {method} {path} {body}")

module.agentd_request = fake_agentd_request

session = module.codexw_session_id(args)
session_snapshot = command("GET", "/api/v1/session")
if session_snapshot.get("status") != 200 or session_snapshot.get("body", {}).get("session_id") != session:
    print("bad native session snapshot", session_snapshot, file=sys.stderr)
    raise SystemExit(1)
files = command("POST", f"/api/v1/session/{session}/files/list", {"path": "${TMP_DIR}"})
if files.get("status") != 200 or not any(entry.get("name") == "file-smoke.txt" for entry in files.get("body", {}).get("entries", [])):
    print("bad native file list", files, file=sys.stderr)
    raise SystemExit(1)
read = command("POST", f"/api/v1/session/{session}/files/read", {"path": "${TMP_DIR}/file-smoke.txt"})
if read.get("status") != 200 or read.get("body", {}).get("filename") != "file-smoke.txt":
    print("bad native file read", read, file=sys.stderr)
    raise SystemExit(1)
shell = command("POST", f"/api/v1/session/{session}/shells/start", {"command": "printf agentd-native-shell-smoke", "cwd": "${TMP_DIR}"})
job = shell.get("body", {}).get("shell", {})
if shell.get("status") != 200 or "agentd-native-shell-smoke" not in "\\n".join(job.get("output_lines", [])):
    print("bad native shell start", shell, file=sys.stderr)
    raise SystemExit(1)
shells = command("GET", f"/api/v1/session/{session}/shells")
if shells.get("status") != 200 or not shells.get("body", {}).get("shells"):
    print("bad native shell list", shells, file=sys.stderr)
    raise SystemExit(1)
detail = command("GET", f"/api/v1/session/{session}/shells/{job['id']}")
if detail.get("status") != 200 or detail.get("body", {}).get("shell", {}).get("id") != job["id"]:
    print("bad native shell detail", detail, file=sys.stderr)
    raise SystemExit(1)
voice_status = command("POST", "/api/v1/runtime/actions", {"action": "voice.webrtc_peer.status", "input": {"session_id": session}})
if voice_status.get("status") != 200 or voice_status.get("body", {}).get("action") != "voice.webrtc_peer.status":
    print("bad voice peer status action", voice_status, file=sys.stderr)
    raise SystemExit(1)
voice_start = command("POST", "/api/v1/runtime/actions", {"action": "voice.webrtc_peer.start", "input": {"session_id": session}})
if voice_start.get("status") != 200 or voice_start.get("body", {}).get("result", {}).get("echo", {}).get("action") != "start":
    print("bad voice peer start action", voice_start, file=sys.stderr)
    raise SystemExit(1)
voice_start_echo = voice_start.get("body", {}).get("result", {}).get("echo", {})
if voice_start_echo.get("broker_url") != "http://127.0.0.1:8787" or voice_start_echo.get("broker_token") != "broker-read-token":
    print("voice peer start did not inherit broker defaults", voice_start, file=sys.stderr)
    raise SystemExit(1)
if voice_start_echo.get("broker_agent_id") != "agentd-native-smoke" or voice_start_echo.get("broker_deployment_id") != "agentd-native-smoke":
    print("voice peer start did not inherit broker routing metadata", voice_start, file=sys.stderr)
    raise SystemExit(1)
voice_stop = command("POST", "/api/v1/runtime/actions", {"action": "voice.webrtc_peer.stop", "input": {"session_id": session}})
if voice_stop.get("status") != 200 or voice_stop.get("body", {}).get("result", {}).get("echo", {}).get("action") != "stop":
    print("bad voice peer stop action", voice_stop, file=sys.stderr)
    raise SystemExit(1)
if not any(item.get("path") == "/api/v1/session/new" for item in captured_agentd):
    print("voice peer start did not ensure the agentd session exists", captured_agentd, file=sys.stderr)
    raise SystemExit(1)
PY

UPDATE_OUT_PATH="${TMP_DIR}/dry-run-runtime-update.json"
"${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py" \
  --broker-url "http://127.0.0.1:8787" \
  --deployment-id "agentd-native-smoke" \
  --display-name "agentd native smoke" \
  --runtime-instance-id "agentd-native-smoke-instance" \
  --deployment-cert-path "${CERT_PATH}" \
  --deployment-key-path "${KEY_PATH}" \
  --agentd-base-url "http://127.0.0.1:18080" \
  --runtime-update-mode agentd_ota \
  --timestamp 1700000000 \
  --dry-run \
  >"${UPDATE_OUT_PATH}"

python3 - <<PY
import json
import sys
from pathlib import Path

payload = json.loads(Path("${UPDATE_OUT_PATH}").read_text())
actions = payload["runtime_capabilities"]["actions"]
runtime_actions = payload["runtime_snapshot"]["runtime"]["runtime_capabilities"]["actions"]
if "runtime.update" not in actions:
    print("missing opt-in runtime.update", payload["runtime_capabilities"], file=sys.stderr)
    raise SystemExit(1)
if actions != runtime_actions:
    print("runtime snapshot capabilities diverge", payload, file=sys.stderr)
    raise SystemExit(1)
if actions["runtime.update"].get("safe_boundary") != "agentd_ota_drain":
    print("bad update safe boundary", actions["runtime.update"], file=sys.stderr)
    raise SystemExit(1)
for forbidden in ("runtime.restart", "runtime.upgrade"):
    if forbidden in actions:
        print("agentd connector must not advertise unsafe operator action", forbidden, actions, file=sys.stderr)
        raise SystemExit(1)
if payload["connect_headers"].get("X-Codexw-Runtime-Capabilities-SHA256") != payload["runtime_capabilities_hash"]:
    print("bad update-mode capability header", payload, file=sys.stderr)
    raise SystemExit(1)
PY

python3 - <<PY
import json
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT = "${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py"
KEY_PATH = "${KEY_PATH}"
CERT_PATH = "${CERT_PATH}"
SESSION_TOKEN = "broker-read-token"
PREFLIGHT_REQUESTS = []
SELF_TEST_OUTPUT_PATH = "${TMP_DIR}/native-self-test-status.json"


class AgentdHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def do_GET(self):
        if self.path.startswith("/api/v1/health"):
            self.send_json({"ok": True})
        elif self.path.startswith("/api/v1/ota/status"):
            self.send_json({
                "ok": True,
                "enabled": True,
                "state": "idle",
                "candidate": {
                    "url": "file:///tmp/agentd-smoke",
                    "sha256": "abc123",
                    "version": "v-smoke",
                    "drain_timeout_ms": 500,
                },
                "restart": {
                    "source": "agentd.ota.restart",
                    "available": True,
                    "enabled": True,
                    "state": "ready",
                    "safe_boundary": "agentd_supervisor_restart_drain",
                    "method": "systemd",
                    "service": "agentd",
                    "drain_timeout_ms": 700,
                },
            })
        elif self.path.startswith("/api/v1/db/sessions"):
            self.send_json({"ok": True, "sessions": [{"session_id": "agentd-session"}]})
        elif self.path.startswith("/api/v1/db/client_events"):
            self.send_json({"ok": True, "session_id": "agentd-session", "client_events": []})
        elif self.path.startswith("/api/v1/db/workflows"):
            self.send_json({"ok": True, "workflows": []})
        elif self.path.startswith("/api/v1/db/workflow_events"):
            self.send_json({"ok": True, "events": []})
        else:
            self.send_response(404)
            self.end_headers()

    def send_json(self, payload):
        raw = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


class BrokerHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        length = int(self.headers.get("content-length") or "0")
        body = json.loads(self.rfile.read(length))
        if self.path == "/api/v1/auth/login":
            assert body == {"username": "admin", "password": "secret-pass"}, body
            self.send_json({"ok": True, "token": SESSION_TOKEN})
            return
        assert self.headers.get("Authorization") == f"Bearer {SESSION_TOKEN}", dict(self.headers)
        assert self.path == "/api/v2/runtime-instances/agentd-native-self-test-instance/actions/preflight", self.path
        assert body["action"] == "runtime.update", body
        assert body["input"]["reason"] == "agentd codexw connector self-test preflight", body
        PREFLIGHT_REQUESTS.append(body)
        self.send_json(
            {
                "ok": True,
                "preview": True,
                "mutates_runtime": False,
                "would_dispatch": True,
                "runtime_instance_id": "agentd-native-self-test-instance",
                "runtime_kind": "agentd",
                "prepared_input": {
                    "url": "file:///tmp/agentd-smoke",
                    "sha256": "abc123",
                    "version": "v-smoke",
                    "drain_timeout_ms": 500,
                    "reason": "agentd codexw connector self-test preflight",
                },
                "update": {
                    "enabled": True,
                    "candidate": {
                        "input": {
                            "url": "file:///tmp/agentd-smoke",
                            "sha256": "abc123",
                            "version": "v-smoke",
                            "drain_timeout_ms": 500,
                        }
                    },
                },
            }
        )

    def do_GET(self):
        assert self.headers.get("Authorization") == f"Bearer {SESSION_TOKEN}", dict(self.headers)
        if self.path == "/api/v2/runtime-instances":
            self.send_json(
                {
                    "ok": True,
                    "runtime_instances": [
                        {
                            "instance_id": "agentd-native-self-test-instance",
                            "runtime_kind": "agentd",
                            "placement": {"deployment_id": "agentd-native-self-test"},
                            "connection": {"state": "online"},
                        }
                    ],
                }
            )
            return
        assert self.path == "/api/v2/runtime-instances/agentd-native-self-test-instance/status", self.path
        self.send_json(
            {
                "ok": True,
                "runtime_instance_id": "agentd-native-self-test-instance",
                "runtime_kind": "agentd",
                "update": {
                    "enabled": True,
                    "state": "ready",
                    "candidate": {
                        "input": {
                            "url": "file:///tmp/agentd-smoke",
                            "sha256": "abc123",
                            "version": "v-smoke",
                            "drain_timeout_ms": 500,
                        }
                    },
                },
            }
        )

    def send_json(self, payload):
        raw = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


agentd = ThreadingHTTPServer(("127.0.0.1", 0), AgentdHandler)
broker = ThreadingHTTPServer(("127.0.0.1", 0), BrokerHandler)
threading.Thread(target=agentd.serve_forever, daemon=True).start()
threading.Thread(target=broker.serve_forever, daemon=True).start()
proc = subprocess.run(
    [
        SCRIPT,
        "--broker-url",
        f"http://127.0.0.1:{broker.server_address[1]}",
        "--deployment-id",
        "agentd-native-self-test",
        "--runtime-instance-id",
        "agentd-native-self-test-instance",
        "--deployment-cert-path",
        CERT_PATH,
        "--deployment-key-path",
        KEY_PATH,
        "--agentd-base-url",
        f"http://127.0.0.1:{agentd.server_address[1]}",
        "--broker-user",
        "admin",
        "--broker-password",
        "secret-pass",
        "--runtime-update-mode",
        "agentd_ota",
        "--self-test",
        "--self-test-output-path",
        SELF_TEST_OUTPUT_PATH,
        "--require-broker-visible",
        "--require-update-preflight",
    ],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=10,
)
agentd.shutdown()
broker.shutdown()
if proc.returncode != 0:
    raise SystemExit(f"self-test failed\\nSTDOUT:\\n{proc.stdout}\\nSTDERR:\\n{proc.stderr}")
payload = json.loads(proc.stdout)
assert payload["ok"] is True, payload
status_payload = json.loads(Path(SELF_TEST_OUTPUT_PATH).read_text())
assert status_payload == payload, status_payload
checks = {check["name"]: check for check in payload["checks"]}
for name in (
    "identity_files",
    "identity_certificate_fingerprint",
    "agentd_health",
    "runtime_sessions_surface",
    "runtime_events_surface",
    "agentd_ota_status",
    "broker_runtime_instance_visible",
    "broker_runtime_update_preflight",
):
    assert checks[name]["ok"] is True, payload
assert checks["broker_runtime_instance_visible"]["online"] is True, payload
assert checks["broker_runtime_update_preflight"]["mutates_runtime"] is False, payload
assert checks["broker_runtime_update_preflight"]["would_dispatch"] is True, payload
assert checks["broker_runtime_update_preflight"]["prepared_input"]["url"] == "file:///tmp/agentd-smoke", payload
assert PREFLIGHT_REQUESTS, "self-test did not call broker runtime.update preflight"
readonly_guard = subprocess.run(
    [
        SCRIPT,
        "--broker-url",
        f"http://127.0.0.1:{broker.server_address[1]}",
        "--deployment-id",
        "agentd-native-self-test",
        "--deployment-cert-path",
        CERT_PATH,
        "--deployment-key-path",
        KEY_PATH,
        "--agentd-base-url",
        f"http://127.0.0.1:{agentd.server_address[1]}",
        "--self-test",
        "--bootstrap-identity",
    ],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=10,
)
assert readonly_guard.returncode != 0, readonly_guard.stdout
assert "--self-test is read-only" in readonly_guard.stderr, readonly_guard.stderr
PY

python3 - <<PY
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, "${SCRIPT_DIR}/../tools")
import agentd_codexw_native_broker_connector as connector  # noqa: E402

base = Path("${TMP_DIR}") / "connector-policy"
base.mkdir()


def args(path: str, timestamp: int = 1700000000, stale_after: int = 900):
    return argparse.Namespace(
        self_test_output_path=path,
        timestamp=timestamp,
        self_test_stale_after_seconds=stale_after,
    )


missing = connector.connector_readiness_status(args(str(base / "missing.json")))
assert missing["state"] == "missing", missing
assert missing["policy_state"] == "missing", missing
assert missing["ok"] is False, missing
assert missing["stale_after_ms"] == 900000, missing

fresh_path = base / "fresh.json"
fresh_path.write_text(json.dumps({
    "ok": True,
    "checked_unix_ms": 1699999999000,
    "checks": [{"name": "broker_runtime_instance_visible", "ok": True}],
}))
fresh = connector.connector_readiness_status(args(str(fresh_path), timestamp=1700000000))
assert fresh["state"] == "fresh", fresh
assert fresh["policy_state"] == "fresh", fresh
assert fresh["ok"] is True, fresh
assert fresh["last_ok"] is True, fresh
assert fresh["age_ms"] == 1000, fresh

stale = connector.connector_readiness_status(args(str(fresh_path), timestamp=1700001000))
assert stale["state"] == "stale", stale
assert stale["policy_state"] == "stale", stale
assert stale["ok"] is False, stale
assert stale["last_ok"] is True, stale
assert stale["age_ms"] == 1001000, stale

failed_path = base / "failed.json"
failed_path.write_text(json.dumps({
    "ok": False,
    "checked_unix_ms": 1699999999000,
    "checks": [{"name": "broker_runtime_instance_visible", "ok": False}],
}))
failed = connector.connector_readiness_status(args(str(failed_path), timestamp=1700000000))
assert failed["state"] == "failed", failed
assert failed["policy_state"] == "failed", failed
assert failed["ok"] is False, failed
assert failed["last_ok"] is False, failed
assert failed["failed_checks"] == ["broker_runtime_instance_visible"], failed
PY

python3 - <<PY
import base64
import hashlib
import hmac
import json
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT = "${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py"
TOKEN_ID = "agentd-bootstrap-token"
SECRET = "agentd-bootstrap-secret"
SESSION_TOKEN = "agentd-bootstrap-session"
BOOT_DIR = Path("${TMP_DIR}") / "bootstrap"
CA_KEY = Path("${TMP_DIR}") / "ca.key.pem"
CA_CERT = Path("${TMP_DIR}") / "ca.cert.pem"

subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(CA_KEY)], check=True)
subprocess.run(
    [
        "openssl",
        "req",
        "-new",
        "-x509",
        "-key",
        str(CA_KEY),
        "-out",
        str(CA_CERT),
        "-days",
        "1",
        "-subj",
        "/CN=codexw-smoke-ca",
    ],
    check=True,
)


def body_digest(body):
    return base64.b64encode(hashlib.sha256(body).digest()).decode()


def expected_sig(method, path, timestamp, body):
    message = "\n".join(["request", TOKEN_ID, method, path, timestamp, body_digest(body)])
    return base64.b64encode(hmac.new(SECRET.encode(), message.encode(), hashlib.sha256).digest()).decode()


class EnrollHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        if self.path == "/api/v1/auth/login":
            length = int(self.headers.get("content-length") or "0")
            body = json.loads(self.rfile.read(length))
            assert body == {"username": "admin", "password": "secret-pass"}, body
            self.send_payload({"ok": True, "token": SESSION_TOKEN, "user": {"username": "admin"}})
            return
        if self.path == "/api/v1/auth/deployment-enrollment-tokens":
            assert self.headers.get("Authorization") == f"Bearer {SESSION_TOKEN}", dict(self.headers)
            length = int(self.headers.get("content-length") or "0")
            body = json.loads(self.rfile.read(length))
            assert body["id"] == "agentd-bootstrap-smoke-agentd-native", body
            self.send_payload({"ok": True, "token": {"id": TOKEN_ID, "shared_secret": SECRET}})
            return
        assert self.path == "/api/v1/deployment/enroll-certificate", self.path
        length = int(self.headers.get("content-length") or "0")
        body = self.rfile.read(length)
        timestamp = self.headers.get("X-Codexw-Auth-Timestamp")
        assert self.headers.get("X-Codexw-Auth-Client-Id") == TOKEN_ID, dict(self.headers)
        assert self.headers.get("X-Codexw-Auth-Signature") == expected_sig("POST", self.path, timestamp, body)
        payload = json.loads(body)
        assert payload["deployment_id"] == "agentd-bootstrap-smoke", payload
        with tempfile.TemporaryDirectory() as td:
            csr_path = Path(td) / "deployment.csr.pem"
            cert_path = Path(td) / "deployment.cert.pem"
            csr_path.write_text(payload["certificate_request_pem"])
            subprocess.run(
                [
                    "openssl",
                    "x509",
                    "-req",
                    "-in",
                    str(csr_path),
                    "-CA",
                    str(CA_CERT),
                    "-CAkey",
                    str(CA_KEY),
                    "-CAcreateserial",
                    "-out",
                    str(cert_path),
                    "-days",
                    "1",
                    "-sha256",
                ],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            certificate_pem = cert_path.read_text()
        response = {
            "ok": True,
            "certificate": {
                "deployment_id": payload["deployment_id"],
                "certificate_pem": certificate_pem,
                "issuer_certificate_pem": CA_CERT.read_text(),
            },
        }
        self.send_payload(response, status=201)

    def send_payload(self, response, status=200):
        raw = json.dumps(response).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


server = ThreadingHTTPServer(("127.0.0.1", 0), EnrollHandler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
broker_url = f"http://127.0.0.1:{server.server_address[1]}"
proc = subprocess.run(
    [
        SCRIPT,
        "--broker-url",
        broker_url,
        "--deployment-id",
        "agentd-bootstrap-smoke",
        "--runtime-instance-id",
        "agentd-bootstrap-instance",
        "--identity-dir",
        str(BOOT_DIR),
        "--agentd-base-url",
        "http://127.0.0.1:18080",
        "--broker-user",
        "admin",
        "--broker-password",
        "secret-pass",
        "--bootstrap-identity",
        "--dry-run",
    ],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=10,
)
server.shutdown()
if proc.returncode != 0:
    raise SystemExit(f"bootstrap failed\\nSTDOUT:\\n{proc.stdout}\\nSTDERR:\\n{proc.stderr}")
payload = json.loads(proc.stdout)
identity = payload["identity"]
assert identity["created_key"] is True, identity
assert identity["created_csr"] is True, identity
assert identity["enrolled_certificate"] is True, identity
for name in ("deployment.key.pem", "deployment.csr.pem", "deployment.cert.pem", "deployment.enrollment.json"):
    if not (BOOT_DIR / name).exists():
        raise SystemExit(f"missing bootstrap file {name}")
assert payload["connect_headers"]["X-Codexw-Runtime-Kind"] == "agentd", payload["connect_headers"]
PY

python3 - <<PY
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT = "${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py"
KEY_PATH = "${KEY_PATH}"
CERT_PATH = "${CERT_PATH}"
CONNECT_SELF_TEST_OUTPUT_PATH = "${TMP_DIR}/native-connect-self-test-status.json"

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(sock, size):
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError("closed")
        chunks.extend(chunk)
    return bytes(chunks)


def send_frame(sock, obj):
    payload = json.dumps(obj, separators=(",", ":")).encode()
    if len(payload) < 126:
        header = struct.pack("!BB", 0x81, len(payload))
    elif len(payload) <= 0xFFFF:
        header = struct.pack("!BBH", 0x81, 126, len(payload))
    else:
        header = struct.pack("!BBQ", 0x81, 127, len(payload))
    sock.sendall(header + payload)


def read_frame(sock):
    first, second = recv_exact(sock, 2)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else b""
    payload = recv_exact(sock, length) if length else b""
    if masked:
        payload = bytes(byte ^ mask[idx % 4] for idx, byte in enumerate(payload))
    if opcode == 8:
        raise EOFError("close")
    return json.loads(payload.decode())


class AgentdHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def do_GET(self):
        if self.path.startswith("/api/v1/health"):
            self.send_json({"ok": True})
        elif self.path.startswith("/api/v1/caps"):
            self.send_json({"ok": True, "capabilities": ["workflow", "rl"]})
        elif self.path.startswith("/api/v1/rl/experience_records"):
            self.send_json({"ok": True, "records": [{"label": "smoke", "reward": 1.0}]})
        elif self.path.startswith("/api/v1/db/sessions"):
            self.send_json({"ok": True, "sessions": [{"session_id": "agentd-session", "created_unix_ms": 1700000000000, "updated_unix_ms": 1700000001000}]})
        elif self.path.startswith("/api/v1/db/client_events"):
            self.send_json({"ok": True, "session_id": "agentd-session", "client_events": [{"id": 1, "ts_unix_ms": 1700000002000, "type": "client.event", "data": {"note": "session smoke"}}]})
        elif self.path.startswith("/api/v1/db/workflows"):
            self.send_json({"ok": True, "workflows": [{"workflow_id": "wf-smoke", "session_id": "agentd-session", "trace_id": "trace-smoke", "status": "running", "created_unix_ms": 1700000003000, "updated_unix_ms": 1700000004000, "spec": {"title": "Smoke workflow"}}]})
        elif self.path.startswith("/api/v1/db/workflow_events"):
            self.send_json({"ok": True, "workflow_id": "wf-smoke", "events": [{"event_id": 2, "workflow_id": "wf-smoke", "ts_unix_ms": 1700000005000, "type": "workflow.started", "data": {"workflow_id": "wf-smoke"}}]})
        elif self.path.startswith("/api/v1/ota/status"):
            self.send_json({
                "ok": True,
                "enabled": True,
                "state": "idle",
                "candidate": {
                    "url": "file:///tmp/agentd-smoke",
                    "sha256": "abc123",
                    "version": "v-smoke",
                    "drain_timeout_ms": 500,
                },
                "restart": {
                    "source": "agentd.ota.restart",
                    "available": True,
                    "enabled": True,
                    "state": "ready",
                    "safe_boundary": "agentd_supervisor_restart_drain",
                    "method": "systemd",
                    "service": "agentd",
                    "drain_timeout_ms": 700,
                },
            })
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        length = int(self.headers.get("content-length") or "0")
        body = json.loads(self.rfile.read(length) or b"{}")
        if self.path.startswith("/api/v1/workflow/submit"):
            results["agentd_workflow_submit_request"] = body
            self.send_json({"ok": True, "workflow_id": "wf-submit-smoke", "status": "queued"})
        elif self.path.startswith("/api/v1/session/new"):
            results["agentd_session_new_request"] = body
            self.send_json({"ok": True, "session_id": body.get("session_id") or "agentd-created-session", "created": True})
        elif self.path.startswith("/api/v1/ota/update"):
            results["agentd_ota_update_request"] = body
            self.send_json({"ok": True, "accepted": True, "state": "draining", "request": body})
        elif self.path.startswith("/api/v1/ota/restart"):
            results["agentd_ota_restart_request"] = body
            self.send_json({"ok": True, "accepted": True, "operation": "restart", "status": "queued", "request": body})
        else:
            self.send_response(404)
            self.end_headers()

    def send_json(self, payload):
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


agentd = ThreadingHTTPServer(("127.0.0.1", 0), AgentdHandler)
agentd_thread = threading.Thread(target=agentd.serve_forever, daemon=True)
agentd_thread.start()
agentd_url = f"http://127.0.0.1:{agentd.server_address[1]}"

broker_sock = socket.socket()
broker_sock.bind(("127.0.0.1", 0))
broker_sock.listen(1)
broker_url = f"http://127.0.0.1:{broker_sock.getsockname()[1]}"
results = {}


def broker_thread():
    conn, _ = broker_sock.accept()
    with conn:
        raw = b""
        while b"\r\n\r\n" not in raw:
            raw += conn.recv(4096)
        text = raw.decode("iso-8859-1")
        headers = {}
        for line in text.split("\r\n")[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.lower()] = value.strip()
        assert headers.get("x-codexw-runtime-kind") == "agentd", headers
        assert headers.get("x-codexw-runtime-instance-id") == "agentd-native-live-instance", headers
        accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + GUID).encode()).digest()).decode()
        conn.sendall(
            (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n"
                "\r\n"
            ).encode()
        )
        send_frame(conn, {"type": "broker.hello", "deployment_id": "agentd-native-smoke", "server_id": "broker-smoke"})
        hello = read_frame(conn)
        snapshot = read_frame(conn)
        assert hello["type"] == "deployment.hello", hello
        assert snapshot["type"] == "deployment.snapshot", snapshot
        assert snapshot["runtime"]["runtime"]["instance_id"] == "agentd-native-live-instance", snapshot
        actions = snapshot["runtime"]["runtime"]["runtime_capabilities"]["actions"]
        assert "runtime.update" in actions, actions
        assert "runtime.restart" in actions, actions
        assert actions["runtime.restart"]["safe_boundary"] == "agentd_supervisor_restart_drain", actions
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-1",
                "method": "POST",
                "path": "/api/v1/runtime/actions",
                "body": {"action": "experience.list", "input": {"limit": 1}},
            },
        )
        command_result = read_frame(conn)
        results["command_result"] = command_result
        assert command_result["type"] == "deployment.command_result", command_result
        assert command_result["request_id"] == "cmd-1", command_result
        assert command_result["status"] == 200, command_result
        assert command_result["body"]["action"] == "experience.list", command_result
        assert command_result["body"]["result"]["records"][0]["label"] == "smoke", command_result
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-submit",
                "method": "POST",
                "path": "/api/v1/runtime/actions",
                "body": {
                    "action": "workflow.submit",
                    "input": {
                        "prompt": "native workflow submit smoke",
                    },
                },
            },
        )
        submit_result = read_frame(conn)
        results["submit_result"] = submit_result
        assert submit_result["status"] == 200, submit_result
        assert submit_result["body"]["action"] == "workflow.submit", submit_result
        assert submit_result["body"]["result"]["workflow_id"] == "wf-submit-smoke", submit_result
        submit_request = results["agentd_workflow_submit_request"]
        assert submit_request["tasks"][0]["task_id"] == "codexw_prompt", submit_request
        assert submit_request["tasks"][0]["request"]["prompt"] == "native workflow submit smoke", submit_request
        assert submit_request["tasks"][0]["request"]["tools"] == "none", submit_request
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-2",
                "method": "GET",
                "path": "/api/v1/runtime/sessions",
            },
        )
        sessions_result = read_frame(conn)
        results["sessions_result"] = sessions_result
        assert sessions_result["status"] == 200, sessions_result
        assert sessions_result["body"]["runtime_kind"] == "agentd", sessions_result
        session_ids = [s["session_id"] for s in sessions_result["body"]["sessions"]]
        assert "agentd-session" in session_ids and "wf-smoke" in session_ids, sessions_result
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-session-create",
                "method": "POST",
                "path": "/api/v1/runtime/sessions",
                "body": {
                    "session_id": "agentd-created-session",
                    "objective": "native session create smoke",
                },
            },
        )
        create_session_result = read_frame(conn)
        results["create_session_result"] = create_session_result
        assert create_session_result["status"] == 200, create_session_result
        created_body = create_session_result["body"]
        assert created_body["runtime_kind"] == "agentd", create_session_result
        assert created_body["session"]["session_id"] == "agentd-created-session", create_session_result
        assert created_body["session"]["objective"] == "native session create smoke", create_session_result
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-3",
                "method": "GET",
                "path": "/api/v1/runtime/events?limit=8",
            },
        )
        events_result = read_frame(conn)
        results["events_result"] = events_result
        assert events_result["status"] == 200, events_result
        assert events_result["body"]["runtime_kind"] == "agentd", events_result
        names = [event["event"] for event in events_result["body"]["events"]]
        assert "workflow.started" in names and "client.event" in names, events_result
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-4",
                "method": "GET",
                "path": "/api/v1/runtime/status",
            },
        )
        status_result = read_frame(conn)
        results["status_result"] = status_result
        assert status_result["status"] == 200, status_result
        assert status_result["body"]["runtime_kind"] == "agentd", status_result
        assert status_result["body"]["update"]["enabled"] is True, status_result
        assert status_result["body"]["update"]["candidate"]["url"] == "file:///tmp/agentd-smoke", status_result
        assert status_result["body"]["restart"]["enabled"] is True, status_result
        assert status_result["body"]["restart"]["safe_boundary"] == "agentd_supervisor_restart_drain", status_result
        connector = status_result["body"]["connector"]
        assert connector["state"] == "fresh", status_result
        assert connector["policy_state"] == "fresh", status_result
        assert connector["ok"] is True, status_result
        assert connector["last_ok"] is True, status_result
        assert connector["checked_unix_ms"] == 1699999999000, status_result
        assert connector["age_ms"] == 1000, status_result
        assert connector["stale_after_ms"] == 900000, status_result
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-5",
                "method": "POST",
                "path": "/api/v1/runtime/actions",
                "body": {
                    "action": "runtime.update",
                    "input": {
                        "url": "file:///tmp/agentd-smoke",
                        "sha256": "abc123",
                        "version": "v-smoke",
                        "reason": "native connector smoke",
                        "drain_timeout_ms": 500,
                    },
                },
            },
        )
        update_result = read_frame(conn)
        results["update_result"] = update_result
        assert update_result["status"] == 200, update_result
        assert update_result["body"]["action"] == "runtime.update", update_result
        assert update_result["body"]["result"]["accepted"] is True, update_result
        send_frame(
            conn,
            {
                "type": "deployment.command",
                "request_id": "cmd-6",
                "method": "POST",
                "path": "/api/v1/runtime/actions",
                "body": {
                    "action": "runtime.restart",
                    "idempotency_key": "restart-smoke-key",
                    "input": {
                        "reason": "native connector restart smoke",
                        "drain_timeout_ms": 700,
                    },
                },
            },
        )
        restart_result = read_frame(conn)
        results["restart_result"] = restart_result
        assert restart_result["status"] == 200, restart_result
        assert restart_result["body"]["action"] == "runtime.restart", restart_result
        assert restart_result["body"]["result"]["accepted"] is True, restart_result


thread = threading.Thread(target=broker_thread, daemon=True)
thread.start()
Path(CONNECT_SELF_TEST_OUTPUT_PATH).write_text(json.dumps({
    "ok": True,
    "mode": "self_test",
    "checked_unix_ms": 1699999999000,
    "deployment_id": "agentd-native-smoke",
    "runtime_instance_id": "agentd-native-live-instance",
    "runtime_kind": "agentd",
    "checks": [
        {"name": "identity_files", "ok": True},
        {"name": "broker_runtime_instance_visible", "ok": True},
    ],
}) + "\\n")
proc = subprocess.run(
    [
        SCRIPT,
        "--broker-url",
        broker_url,
        "--deployment-id",
        "agentd-native-smoke",
        "--display-name",
        "agentd native smoke",
        "--runtime-instance-id",
        "agentd-native-live-instance",
        "--deployment-cert-path",
        CERT_PATH,
        "--deployment-key-path",
        KEY_PATH,
        "--agentd-base-url",
        agentd_url,
        "--runtime-update-mode",
        "agentd_ota",
        "--runtime-restart-mode",
        "agentd_ota",
        "--self-test-output-path",
        CONNECT_SELF_TEST_OUTPUT_PATH,
        "--timestamp",
        "1700000000",
        "--timeout",
        "5",
        "--connect",
    ],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=10,
)
agentd.shutdown()
broker_sock.close()
thread.join(timeout=5)
if proc.returncode != 0:
    raise SystemExit(f"connector failed\\nSTDOUT:\\n{proc.stdout}\\nSTDERR:\\n{proc.stderr}")
summary = json.loads(proc.stdout)
if summary.get("mode") != "connect" or summary.get("commands") != 8:
    raise SystemExit(f"bad connector summary: {summary}")
for key in ("command_result", "submit_result", "sessions_result", "create_session_result", "events_result", "status_result", "update_result", "restart_result", "agentd_workflow_submit_request", "agentd_session_new_request", "agentd_ota_update_request", "agentd_ota_restart_request"):
    if key not in results:
        raise SystemExit(f"broker did not receive {key}")
assert results["agentd_session_new_request"] == {"session_id": "agentd-created-session"}, results["agentd_session_new_request"]
assert results["agentd_ota_update_request"] == {
    "url": "file:///tmp/agentd-smoke",
    "sha256": "abc123",
    "version": "v-smoke",
    "reason": "native connector smoke",
    "drain_timeout_ms": 500,
}, results["agentd_ota_update_request"]
assert results["agentd_ota_restart_request"] == {
    "reason": "native connector restart smoke",
    "idempotency_key": "restart-smoke-key",
    "drain_timeout_ms": 700,
}, results["agentd_ota_restart_request"]
PY

echo "agentd_codexw_native_broker_connector_smoke OK"
