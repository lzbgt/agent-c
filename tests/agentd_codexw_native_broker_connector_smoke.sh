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
if "workflow.submit" not in payload["runtime_capabilities"]["actions"]:
    print("missing workflow action", payload["runtime_capabilities"], file=sys.stderr)
    raise SystemExit(1)
for forbidden in ("runtime.restart", "runtime.update", "runtime.upgrade"):
    if forbidden in payload["runtime_capabilities"]["actions"]:
        print("agentd connector must not advertise unsafe operator action", forbidden, payload["runtime_capabilities"], file=sys.stderr)
        raise SystemExit(1)
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

SCRIPT = "${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py"
KEY_PATH = "${KEY_PATH}"
CERT_PATH = "${CERT_PATH}"

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


thread = threading.Thread(target=broker_thread, daemon=True)
thread.start()
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
        "--timestamp",
        "1700000000",
        "--timeout",
        "5",
        "--connect",
        "--once",
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
if summary.get("mode") != "connect" or summary.get("commands") != 1:
    raise SystemExit(f"bad connector summary: {summary}")
if "command_result" not in results:
    raise SystemExit("broker did not receive command result")
PY

echo "agentd_codexw_native_broker_connector_smoke OK"
