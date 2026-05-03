#!/usr/bin/env python3
"""Prepare native agentd deployment-connect identity for a codexw broker."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from agentd_codexw_contract import (
    agentd_runtime_events,
    agentd_runtime_sessions,
    base64_body_digest,
    canonical_json_bytes,
    default_runtime_instance_id,
    deployment_snapshot_frame,
    runtime_capabilities,
    runtime_capabilities_hash,
    runtime_identity_headers,
    runtime_snapshot,
)


CONNECT_PATH = "/api/v1/deployment/connect"
ENROLL_CERTIFICATE_PATH = "/api/v1/deployment/enroll-certificate"
LOGIN_PATH = "/api/v1/auth/login"
SELF_ENROLLMENT_TOKEN_PATH = "/api/v1/auth/deployment-enrollment-tokens"
DEFAULT_KEY_NAME = "deployment.key.pem"
DEFAULT_CERT_NAME = "deployment.cert.pem"
DEFAULT_CSR_NAME = "deployment.csr.pem"
DEFAULT_ENROLLMENT_MATERIAL_NAME = "deployment.enrollment.json"


def json_dumps(obj: Any) -> bytes:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def json_pretty(obj: Any) -> str:
    return json.dumps(obj, indent=2, sort_keys=True)


def resolve_identity_paths(args: argparse.Namespace) -> None:
    identity_dir = Path(args.identity_dir).expanduser()
    args.identity_dir = str(identity_dir)
    if not args.deployment_key_path:
        args.deployment_key_path = str(identity_dir / DEFAULT_KEY_NAME)
    if not args.deployment_cert_path:
        args.deployment_cert_path = str(identity_dir / DEFAULT_CERT_NAME)
    if not args.csr_path:
        args.csr_path = str(identity_dir / DEFAULT_CSR_NAME)
    if not args.enrollment_material_path:
        args.enrollment_material_path = str(identity_dir / DEFAULT_ENROLLMENT_MATERIAL_NAME)


def run_openssl(command: list[str], *, input_bytes: bytes | None = None) -> bytes:
    proc = subprocess.run(
        ["openssl", *command],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode("utf-8", errors="replace").strip())
    return proc.stdout


def generate_private_key(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    run_openssl(["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(path)])
    path.chmod(0o600)


def generate_csr(*, key_path: Path, csr_path: Path, deployment_id: str) -> None:
    csr_path.parent.mkdir(parents=True, exist_ok=True)
    run_openssl(
        [
            "req",
            "-new",
            "-key",
            str(key_path),
            "-out",
            str(csr_path),
            "-subj",
            f"/CN={deployment_id}",
        ]
    )


def write_text_private(path: Path, text: str, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    path.chmod(mode)


def read_pem_der(path: Path, label: str) -> bytes:
    text = path.read_text(encoding="utf-8")
    lines = [line.strip() for line in text.splitlines() if line and not line.startswith("-----")]
    if not lines:
        raise ValueError(f"{label} PEM has no base64 body: {path}")
    return base64.b64decode("".join(lines), validate=True)


def cert_header_value(path: Path) -> str:
    return base64.b64encode(read_pem_der(path, "deployment certificate")).decode("ascii")


def cert_fingerprint(path: Path) -> str:
    return hashlib.sha256(read_pem_der(path, "deployment certificate")).hexdigest()


def openssl_sign_sha256(private_key_path: Path, message: bytes) -> str:
    return base64.b64encode(run_openssl(["dgst", "-sha256", "-sign", str(private_key_path)], input_bytes=message)).decode(
        "ascii"
    )


def deployment_connect_signature(
    *,
    deployment_id: str,
    path: str,
    timestamp: str,
    fingerprint: str,
    private_key_path: Path,
) -> str:
    message = "\n".join(["deployment_connect", deployment_id, path, timestamp, fingerprint]).encode("utf-8")
    return openssl_sign_sha256(private_key_path, message)


def deployment_request_signature(
    *,
    deployment_id: str,
    method: str,
    path: str,
    timestamp: str,
    fingerprint: str,
    body: bytes,
    private_key_path: Path,
) -> str:
    message = "\n".join(
        [
            "deployment_request",
            deployment_id,
            method,
            path,
            timestamp,
            fingerprint,
            base64_body_digest(body),
        ]
    ).encode("utf-8")
    return openssl_sign_sha256(private_key_path, message)


def request_auth_signature(
    *,
    secret: str,
    client_id: str,
    method: str,
    path: str,
    body: bytes,
    timestamp: str,
) -> str:
    message = "\n".join(["request", client_id, method, path, timestamp, base64_body_digest(body)])
    digest = hmac.new(secret.encode("utf-8"), message.encode("utf-8"), hashlib.sha256).digest()
    return base64.b64encode(digest).decode("ascii")


def signed_enrollment_headers(
    *,
    token_id: str,
    shared_secret: str,
    body: bytes,
    timestamp: str,
) -> dict[str, str]:
    return {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "X-Codexw-Auth-Client-Id": token_id,
        "X-Codexw-Auth-Timestamp": timestamp,
        "X-Codexw-Auth-Signature": request_auth_signature(
            secret=shared_secret,
            client_id=token_id,
            method="POST",
            path=ENROLL_CERTIFICATE_PATH,
            body=body,
            timestamp=timestamp,
        ),
    }


def deployment_connect_headers(args: argparse.Namespace, timestamp: str) -> dict[str, str]:
    cert_path = Path(args.deployment_cert_path)
    key_path = Path(args.deployment_key_path)
    fingerprint = cert_fingerprint(cert_path)
    headers = {
        "X-Codexw-Deployment-Id": args.deployment_id,
        "X-Codexw-Deployment-Name": args.display_name or args.deployment_id,
        "X-Codexw-Deployment-Mode": args.connection_mode,
        "X-Codexw-Deployment-Certificate": cert_header_value(cert_path),
        "X-Codexw-Deployment-Certificate-Fingerprint": fingerprint,
        "X-Codexw-Deployment-Certificate-Signature": deployment_connect_signature(
            deployment_id=args.deployment_id,
            path=CONNECT_PATH,
            timestamp=timestamp,
            fingerprint=fingerprint,
            private_key_path=key_path,
        ),
        "X-Codexw-Auth-Timestamp": timestamp,
    }
    headers.update(
        runtime_identity_headers(
            instance_id=args.runtime_instance_id,
            connection_mode=args.connection_mode,
            host=args.runtime_host_id,
            os_name=args.runtime_target_os,
            arch=args.runtime_target_arch,
        )
    )
    return headers


def broker_ws_url(broker_url: str) -> str:
    parsed = urllib.parse.urlparse(broker_url)
    scheme = "wss" if parsed.scheme == "https" else "ws"
    return urllib.parse.urlunparse(parsed._replace(scheme=scheme, path=CONNECT_PATH, params="", query="", fragment=""))


def broker_http_url(broker_url: str, path: str) -> str:
    parsed = urllib.parse.urlparse(broker_url)
    return urllib.parse.urlunparse(parsed._replace(path=path, params="", query="", fragment=""))


def broker_json_request(
    args: argparse.Namespace,
    *,
    method: str,
    path: str,
    body: dict[str, Any] | None = None,
    bearer_token: str = "",
) -> dict[str, Any]:
    data = None if body is None else json_dumps(body)
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    if bearer_token:
        headers["Authorization"] = f"Bearer {bearer_token}"
    request = urllib.request.Request(
        broker_http_url(args.broker_url, path),
        data=data,
        headers=headers,
        method=method,
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=args.timeout) as response:
        raw = response.read()
    return json.loads(raw.decode("utf-8")) if raw else {}


def login_broker_user(args: argparse.Namespace) -> str:
    if not args.broker_user or not args.broker_password:
        raise ValueError("--broker-user and --broker-password are required to issue an enrollment token")
    response = broker_json_request(
        args,
        method="POST",
        path=LOGIN_PATH,
        body={"username": args.broker_user, "password": args.broker_password},
    )
    token = response.get("token")
    if not isinstance(token, str) or not token.strip():
        raise ValueError("broker login response missing token")
    return token.strip()


def broker_bearer_token(args: argparse.Namespace) -> str:
    token = str(args.broker_token or "").strip()
    if token:
        return token
    return login_broker_user(args)


def issue_enrollment_token(args: argparse.Namespace) -> dict[str, str]:
    session_token = login_broker_user(args)
    token_id = args.enrollment_token_id.strip() if args.enrollment_token_id else f"{args.deployment_id}-agentd-native"
    response = broker_json_request(
        args,
        method="POST",
        path=SELF_ENROLLMENT_TOKEN_PATH,
        bearer_token=session_token,
        body={
            "id": token_id,
            "description": f"agentd native deployment enrollment for {args.deployment_id}",
        },
    )
    token = response.get("token")
    if not isinstance(token, dict):
        raise ValueError("deployment enrollment token response missing token object")
    token_id_value = token.get("id")
    secret = token.get("shared_secret")
    if not isinstance(token_id_value, str) or not token_id_value.strip():
        raise ValueError("deployment enrollment token response missing token.id")
    if not isinstance(secret, str) or not secret.strip():
        raise ValueError("deployment enrollment token response missing token.shared_secret")
    args.enrollment_token_id = token_id_value.strip()
    args.enrollment_shared_secret = secret.strip()
    return {"id": args.enrollment_token_id, "shared_secret": args.enrollment_shared_secret}


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError("websocket closed")
        chunks.extend(chunk)
    return bytes(chunks)


def websocket_connect(url: str, headers: dict[str, str], timeout: float) -> socket.socket:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme not in ("ws", "wss"):
        raise ValueError(f"unsupported websocket scheme: {parsed.scheme}")
    host = parsed.hostname
    if not host:
        raise ValueError("websocket URL host is required")
    port = parsed.port or (443 if parsed.scheme == "wss" else 80)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query
    raw_sock = socket.create_connection((host, port), timeout=timeout)
    sock = raw_sock
    if parsed.scheme == "wss":
        context = ssl.create_default_context()
        sock = context.wrap_socket(raw_sock, server_hostname=host)
    sock.settimeout(timeout)
    key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
    host_header = host if parsed.port is None else f"{host}:{port}"
    request_lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host_header}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
    ]
    for name, value in headers.items():
        request_lines.append(f"{name}: {value}")
    request_lines.extend(["", ""])
    sock.sendall("\r\n".join(request_lines).encode("utf-8"))
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise EOFError("websocket handshake closed")
        response.extend(chunk)
        if len(response) > 65536:
            raise ValueError("websocket handshake response too large")
    status_line = response.split(b"\r\n", 1)[0].decode("iso-8859-1", errors="replace")
    if " 101 " not in status_line:
        raise RuntimeError(f"websocket upgrade failed: {status_line}")
    return sock


def websocket_send_frame(sock: socket.socket, opcode: int, payload: bytes = b"") -> None:
    first = 0x80 | (opcode & 0x0F)
    mask_bit = 0x80
    length = len(payload)
    if length < 126:
        header = struct.pack("!BB", first, mask_bit | length)
    elif length <= 0xFFFF:
        header = struct.pack("!BBH", first, mask_bit | 126, length)
    else:
        header = struct.pack("!BBQ", first, mask_bit | 127, length)
    mask = secrets.token_bytes(4)
    masked = bytes(byte ^ mask[idx % 4] for idx, byte in enumerate(payload))
    sock.sendall(header + mask + masked)


def websocket_send_json(sock: socket.socket, obj: dict[str, Any]) -> None:
    websocket_send_frame(sock, 0x1, json_dumps(obj))


def websocket_read_frame(sock: socket.socket) -> tuple[int, bytes]:
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
    return opcode, payload


def websocket_read_json(sock: socket.socket) -> dict[str, Any]:
    while True:
        opcode, payload = websocket_read_frame(sock)
        if opcode == 0x1:
            obj = json.loads(payload.decode("utf-8"))
            if not isinstance(obj, dict):
                raise ValueError("websocket JSON frame must be an object")
            return obj
        if opcode == 0x8:
            raise EOFError("websocket close frame received")
        if opcode == 0x9:
            websocket_send_frame(sock, 0xA, payload)
            continue


def agentd_request(args: argparse.Namespace, method: str, path: str, body: Any | None = None) -> Any:
    data = None if body is None else json_dumps(body)
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    if args.agentd_auth_token:
        headers["Authorization"] = f"Bearer {args.agentd_auth_token}"
    request = urllib.request.Request(
        args.agentd_base_url.rstrip("/") + path,
        data=data,
        headers=headers,
        method=method,
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=args.timeout) as response:
        raw = response.read()
    return json.loads(raw.decode("utf-8")) if raw else {}


def request_input(body: dict[str, Any]) -> dict[str, Any]:
    for key in ("input", "params", "payload"):
        value = body.get(key)
        if isinstance(value, dict):
            return value
    return {}


def query_from_input(input_obj: dict[str, Any], allowed: set[str]) -> str:
    values: dict[str, str] = {}
    for key in allowed:
        value = input_obj.get(key)
        if isinstance(value, (str, int, float, bool)):
            text = str(value).strip()
            if text:
                values[key] = text
    return urllib.parse.urlencode(values)


def require_string(obj: dict[str, Any], key: str) -> str:
    value = obj.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{key} is required")
    return value.strip()


def forward_runtime_action(args: argparse.Namespace, body: dict[str, Any]) -> dict[str, Any]:
    action = body.get("action")
    if not isinstance(action, str) or not action.strip():
        raise ValueError("runtime action is required")
    action = action.strip()
    input_obj = request_input(body)
    if action == "workflow.submit":
        result = agentd_request(args, "POST", "/api/v1/workflow/submit", input_obj)
    elif action == "workflow.read":
        workflow_id = require_string(input_obj, "workflow_id")
        result = agentd_request(args, "GET", "/api/v1/workflow?" + urllib.parse.urlencode({"workflow_id": workflow_id}))
    elif action == "workflow.cancel":
        workflow_id = require_string(input_obj, "workflow_id")
        result = agentd_request(args, "POST", "/api/v1/workflow/cancel", {"workflow_id": workflow_id})
    elif action == "schedule.list":
        query = query_from_input(input_obj, {"status", "limit", "offset"})
        result = agentd_request(args, "GET", "/api/v1/workflow_schedules" + (f"?{query}" if query else ""))
    elif action in ("experience.list", "experience.export"):
        query = query_from_input(
            input_obj,
            {"offset", "limit", "label", "workflow_id", "task_id", "min_reward", "max_reward"},
        )
        result = agentd_request(args, "GET", "/api/v1/rl/experience_records" + (f"?{query}" if query else ""))
        if action == "experience.export" and isinstance(result, dict):
            result = {**result, "export_format": "json"}
    else:
        raise ValueError(f"unsupported runtime action: {action}")
    return {
        "ok": True,
        "runtime_kind": "agentd",
        "instance_id": args.runtime_instance_id,
        "action": action,
        "result": result,
    }


def runtime_payload(args: argparse.Namespace) -> dict[str, Any]:
    health = None
    caps = None
    try:
        health = agentd_request(args, "GET", "/api/v1/health")
    except Exception as exc:  # noqa: BLE001
        health = {"ok": False, "error": str(exc)}
    try:
        caps = agentd_request(args, "GET", "/api/v1/caps")
    except Exception as exc:  # noqa: BLE001
        caps = {"ok": False, "error": str(exc)}
    return runtime_snapshot(
        instance_id=args.runtime_instance_id,
        deployment_id=args.deployment_id,
        agentd_base_url=args.agentd_base_url,
        connection_mode=args.connection_mode,
        preferred_broker_transport="native_deployment_connect",
        implementation="native_agentd_broker_connector",
        agentd_health=health,
        agentd_capabilities=caps,
    )


def handle_command(args: argparse.Namespace, frame: dict[str, Any]) -> dict[str, Any]:
    request_id = str(frame.get("request_id") or "")
    method = str(frame.get("method") or "GET").upper()
    path = str(frame.get("path") or "")
    body = frame.get("body")
    parsed = urllib.parse.urlparse(path)
    route_path = parsed.path
    query = urllib.parse.parse_qs(parsed.query)
    request_agentd = lambda m, p, b=None: agentd_request(args, m, p, b)
    try:
        if method == "GET" and route_path == "/api/v1/runtime":
            result = runtime_payload(args)
        elif method == "GET" and route_path == "/api/v1/runtime/sessions":
            result = agentd_runtime_sessions(request_agentd)
        elif method == "GET" and route_path == "/api/v1/runtime/events":
            result = agentd_runtime_events(request_agentd, query)
        elif method == "POST" and route_path == "/api/v1/runtime/actions":
            if not isinstance(body, dict):
                raise ValueError("runtime action command body must be an object")
            result = forward_runtime_action(args, body)
        elif method == "GET" and route_path == "/healthz":
            result = {"ok": True, "service": "agentd-codexw-native-broker-connector"}
        else:
            return {
                "type": "deployment.command_result",
                "deployment_id": args.deployment_id,
                "request_id": request_id,
                "status": 404,
                "error": f"unsupported native agentd command path: {method} {path}",
                "body": {"ok": False, "error": {"code": "not_found", "message": route_path}},
            }
        return {
            "type": "deployment.command_result",
            "deployment_id": args.deployment_id,
            "request_id": request_id,
            "status": 200,
            "body": result,
        }
    except Exception as exc:  # noqa: BLE001
        return {
            "type": "deployment.command_result",
            "deployment_id": args.deployment_id,
            "request_id": request_id,
            "status": 500,
            "error": str(exc),
            "body": {"ok": False, "error": {"code": "agentd_command_error", "message": str(exc)}},
        }


def run_connect(args: argparse.Namespace) -> dict[str, Any]:
    timestamp = str(int(args.timestamp or time.time()))
    headers = deployment_connect_headers(args, timestamp)
    sock = websocket_connect(broker_ws_url(args.broker_url), headers, args.timeout)
    sent_commands = 0
    try:
        websocket_send_json(
            sock,
            {
                "type": "deployment.hello",
                "deployment_id": args.deployment_id,
                "display_name": args.display_name or args.deployment_id,
            },
        )
        websocket_send_json(
            sock,
            deployment_snapshot_frame(
                deployment_id=args.deployment_id,
                display_name=args.display_name or args.deployment_id,
                runtime=runtime_payload(args),
                session=None,
            ),
        )
        next_snapshot = time.monotonic() + args.snapshot_interval
        deadline = time.monotonic() + args.max_runtime_seconds if args.max_runtime_seconds > 0 else None
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                break
            timeout = min(args.timeout, max(0.1, next_snapshot - time.monotonic()))
            if deadline is not None:
                timeout = min(timeout, max(0.1, deadline - time.monotonic()))
            sock.settimeout(timeout)
            try:
                frame = websocket_read_json(sock)
            except socket.timeout:
                frame = {}
            except EOFError:
                break
            if frame.get("type") == "deployment.command":
                websocket_send_json(sock, handle_command(args, frame))
                sent_commands += 1
                if args.once:
                    break
            if time.monotonic() >= next_snapshot:
                websocket_send_json(
                    sock,
                    deployment_snapshot_frame(
                        deployment_id=args.deployment_id,
                        display_name=args.display_name or args.deployment_id,
                        runtime=runtime_payload(args),
                        session=None,
                    ),
                )
                next_snapshot = time.monotonic() + args.snapshot_interval
    finally:
        try:
            websocket_send_frame(sock, 0x8)
        except Exception:  # noqa: BLE001
            pass
        sock.close()
    return {"ok": True, "mode": "connect", "deployment_id": args.deployment_id, "commands": sent_commands}


def build_dry_run_payload(args: argparse.Namespace) -> dict[str, Any]:
    timestamp = str(int(args.timestamp or time.time()))
    runtime = runtime_snapshot(
        instance_id=args.runtime_instance_id,
        deployment_id=args.deployment_id,
        agentd_base_url=args.agentd_base_url,
        connection_mode=args.connection_mode,
        preferred_broker_transport="native_deployment_connect",
        implementation="native_agentd_broker_connector",
    )
    snapshot = deployment_snapshot_frame(
        deployment_id=args.deployment_id,
        display_name=args.display_name or args.deployment_id,
        runtime=runtime,
        session=None,
    )
    return {
        "ok": True,
        "mode": "dry_run",
        "broker": {
            "http_base_url": args.broker_url.rstrip("/"),
            "connect_url": broker_ws_url(args.broker_url),
            "enroll_certificate_url": broker_http_url(args.broker_url, ENROLL_CERTIFICATE_PATH),
        },
        "deployment_id": args.deployment_id,
        "runtime_instance_id": args.runtime_instance_id,
        "runtime_capabilities": runtime_capabilities(),
        "runtime_capabilities_hash": runtime_capabilities_hash(),
        "runtime_capabilities_canonical_json_sha256": hashlib.sha256(
            canonical_json_bytes(runtime_capabilities())
        ).hexdigest(),
        "connect_headers": deployment_connect_headers(args, timestamp),
        "runtime_snapshot": runtime,
        "deployment_snapshot_frame": snapshot,
        "notes": [
            "dry_run does not open the deployment websocket",
            "connect_headers are intentionally complete enough for broker-side signature verification",
        ],
    }


def check_status(name: str, ok: bool, **details: Any) -> dict[str, Any]:
    result = {"name": name, "ok": bool(ok)}
    result.update(details)
    return result


def run_self_test(args: argparse.Namespace) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []
    cert_path = Path(args.deployment_cert_path)
    key_path = Path(args.deployment_key_path)
    checks.append(
        check_status(
            "identity_files",
            cert_path.exists() and key_path.exists(),
            deployment_cert_path=str(cert_path),
            deployment_key_path=str(key_path),
        )
    )
    if cert_path.exists():
        try:
            checks.append(check_status("identity_certificate_fingerprint", True, sha256=cert_fingerprint(cert_path)))
        except Exception as exc:  # noqa: BLE001
            checks.append(check_status("identity_certificate_fingerprint", False, error=str(exc)))

    try:
        health = agentd_request(args, "GET", "/api/v1/health")
        checks.append(check_status("agentd_health", bool(health.get("ok", True)), response=health))
    except Exception as exc:  # noqa: BLE001
        checks.append(check_status("agentd_health", False, error=str(exc)))

    try:
        sessions = agentd_runtime_sessions(lambda m, p, b=None: agentd_request(args, m, p, b))
        checks.append(
            check_status(
                "runtime_sessions_surface",
                sessions.get("runtime_kind") == "agentd" and isinstance(sessions.get("sessions"), list),
                returned=len(sessions.get("sessions") or []),
            )
        )
    except Exception as exc:  # noqa: BLE001
        checks.append(check_status("runtime_sessions_surface", False, error=str(exc)))

    try:
        events = agentd_runtime_events(lambda m, p, b=None: agentd_request(args, m, p, b), {"limit": ["8"]})
        checks.append(
            check_status(
                "runtime_events_surface",
                events.get("runtime_kind") == "agentd" and isinstance(events.get("events"), list),
                returned=len(events.get("events") or []),
            )
        )
    except Exception as exc:  # noqa: BLE001
        checks.append(check_status("runtime_events_surface", False, error=str(exc)))

    should_check_broker = bool(args.require_broker_visible or args.broker_token or (args.broker_user and args.broker_password))
    if should_check_broker:
        try:
            token = broker_bearer_token(args)
            inventory = broker_json_request(args, method="GET", path="/api/v2/runtime-instances", bearer_token=token)
            instances = inventory.get("runtime_instances") or []
            matched = None
            for instance in instances:
                if not isinstance(instance, dict):
                    continue
                placement = instance.get("placement") if isinstance(instance.get("placement"), dict) else {}
                if instance.get("instance_id") == args.runtime_instance_id or placement.get("deployment_id") == args.deployment_id:
                    matched = instance
                    break
            online = bool(matched and (matched.get("connection") or {}).get("state") == "online")
            checks.append(
                check_status(
                    "broker_runtime_instance_visible",
                    online if args.require_broker_visible else matched is not None,
                    runtime_instance_id=args.runtime_instance_id,
                    deployment_id=args.deployment_id,
                    matched=matched is not None,
                    online=online,
                )
            )
        except Exception as exc:  # noqa: BLE001
            checks.append(check_status("broker_runtime_instance_visible", False, error=str(exc)))
    else:
        checks.append(
            check_status(
                "broker_runtime_instance_visible",
                True,
                skipped=True,
                reason="set AGENTD_CODEXW_BROKER_TOKEN or broker user/password to verify broker inventory",
            )
        )

    ok = all(check.get("ok") for check in checks)
    return {
        "ok": ok,
        "mode": "self_test",
        "deployment_id": args.deployment_id,
        "runtime_instance_id": args.runtime_instance_id,
        "runtime_kind": "agentd",
        "checks": checks,
    }


def enroll_certificate(args: argparse.Namespace) -> dict[str, Any]:
    if not args.enrollment_token_id or not args.enrollment_shared_secret:
        if args.broker_user and args.broker_password:
            issue_enrollment_token(args)
        else:
            raise ValueError(
                "--enrollment-token-id and --enrollment-shared-secret, or --broker-user and --broker-password, are required"
            )
    csr_pem = Path(args.csr_path).read_text(encoding="utf-8")
    body = json_dumps(
        {
            "deployment_id": args.deployment_id,
            "certificate_request_pem": csr_pem,
            "certificate_days": args.certificate_days,
        }
    )
    timestamp = str(int(args.timestamp or time.time()))
    request = urllib.request.Request(
        broker_http_url(args.broker_url, ENROLL_CERTIFICATE_PATH),
        data=body,
        headers=signed_enrollment_headers(
            token_id=args.enrollment_token_id,
            shared_secret=args.enrollment_shared_secret,
            body=body,
            timestamp=timestamp,
        ),
        method="POST",
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=args.timeout) as response:
        raw = response.read()
    return json.loads(raw.decode("utf-8")) if raw else {}


def persist_enrollment_response(args: argparse.Namespace, response: dict[str, Any]) -> None:
    certificate = response.get("certificate")
    if not isinstance(certificate, dict):
        raise ValueError("enrollment response missing certificate object")
    certificate_pem = certificate.get("certificate_pem")
    if not isinstance(certificate_pem, str) or "BEGIN CERTIFICATE" not in certificate_pem:
        raise ValueError("enrollment response missing certificate.certificate_pem")
    write_text_private(Path(args.deployment_cert_path), certificate_pem, mode=0o600)
    write_text_private(Path(args.enrollment_material_path), json_pretty(response) + "\n", mode=0o600)


def bootstrap_identity(args: argparse.Namespace) -> dict[str, Any]:
    key_path = Path(args.deployment_key_path)
    cert_path = Path(args.deployment_cert_path)
    csr_path = Path(args.csr_path)
    created_key = False
    created_csr = False
    enrolled = False
    if not key_path.exists():
        generate_private_key(key_path)
        created_key = True
    if not csr_path.exists() or args.force_csr:
        generate_csr(key_path=key_path, csr_path=csr_path, deployment_id=args.deployment_id)
        created_csr = True
    if not cert_path.exists():
        if not args.enrollment_token_id or not args.enrollment_shared_secret:
            if args.broker_user and args.broker_password:
                issue_enrollment_token(args)
            else:
                raise ValueError(
                    "deployment certificate is missing; provide --enrollment-token-id and "
                    "--enrollment-shared-secret, or --broker-user and --broker-password, with --bootstrap-identity"
                )
        response = enroll_certificate(args)
        persist_enrollment_response(args, response)
        enrolled = True
    return {
        "identity_dir": args.identity_dir,
        "deployment_key_path": args.deployment_key_path,
        "deployment_cert_path": args.deployment_cert_path,
        "csr_path": args.csr_path,
        "created_key": created_key,
        "created_csr": created_csr,
        "enrolled_certificate": enrolled,
    }


def prepare_identity(args: argparse.Namespace) -> dict[str, Any]:
    if args.enroll_certificate and args.bootstrap_identity:
        key_path = Path(args.deployment_key_path)
        csr_path = Path(args.csr_path)
        created_key = False
        created_csr = False
        if not key_path.exists():
            generate_private_key(key_path)
            created_key = True
        if not csr_path.exists() or args.force_csr:
            generate_csr(key_path=key_path, csr_path=csr_path, deployment_id=args.deployment_id)
            created_csr = True
        return {
            "identity_dir": args.identity_dir,
            "deployment_key_path": args.deployment_key_path,
            "deployment_cert_path": args.deployment_cert_path,
            "csr_path": args.csr_path,
            "created_key": created_key,
            "created_csr": created_csr,
            "enrolled_certificate": False,
        }
    if args.bootstrap_identity:
        return bootstrap_identity(args)
    key_path = Path(args.deployment_key_path)
    cert_path = Path(args.deployment_cert_path)
    missing = [str(path) for path in (key_path, cert_path) if not path.exists()]
    if missing and not args.enroll_certificate:
        raise ValueError(
            "missing deployment identity file(s): "
            + ", ".join(missing)
            + "; pass --bootstrap-identity or explicit existing paths"
        )
    return {
        "identity_dir": args.identity_dir,
        "deployment_key_path": args.deployment_key_path,
        "deployment_cert_path": args.deployment_cert_path,
        "csr_path": args.csr_path,
        "created_key": False,
        "created_csr": False,
        "enrolled_certificate": False,
    }


def run_connect_supervised(args: argparse.Namespace) -> dict[str, Any]:
    attempts = 0
    delay = args.reconnect_initial_delay
    deadline = time.monotonic() + args.max_runtime_seconds if args.max_runtime_seconds > 0 else None
    last_error = ""
    while True:
        attempts += 1
        try:
            result = run_connect(args)
            result["attempts"] = attempts
            result["last_error"] = last_error
            if args.once or not args.reconnect:
                return result
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
            if not args.reconnect:
                raise
        if deadline is not None and time.monotonic() >= deadline:
            return {
                "ok": False,
                "mode": "connect",
                "deployment_id": args.deployment_id,
                "attempts": attempts,
                "last_error": last_error,
            }
        sleep_for = max(0.1, min(delay, args.reconnect_max_delay))
        if deadline is not None:
            sleep_for = min(sleep_for, max(0.0, deadline - time.monotonic()))
        if sleep_for <= 0:
            continue
        time.sleep(sleep_for)
        delay = min(args.reconnect_max_delay, max(delay * 2, args.reconnect_initial_delay))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broker-url", default=os.environ.get("AGENTD_CODEXW_BROKER_URL", ""), help="codexw broker base URL, e.g. https://broker.example")
    parser.add_argument("--deployment-id", default=os.environ.get("AGENTD_CODEXW_DEPLOYMENT_ID", ""))
    parser.add_argument("--display-name", default=os.environ.get("AGENTD_CODEXW_DISPLAY_NAME", ""))
    parser.add_argument("--runtime-instance-id", default=os.environ.get("AGENTD_CODEXW_RUNTIME_INSTANCE_ID", ""))
    parser.add_argument("--runtime-host-id", default=os.environ.get("AGENTD_CODEXW_RUNTIME_HOST_ID", ""))
    parser.add_argument("--runtime-target-os", default=os.environ.get("AGENTD_CODEXW_RUNTIME_TARGET_OS", ""))
    parser.add_argument("--runtime-target-arch", default=os.environ.get("AGENTD_CODEXW_RUNTIME_TARGET_ARCH", ""))
    parser.add_argument(
        "--connection-mode",
        default=os.environ.get("AGENTD_CODEXW_CONNECTION_MODE", "service"),
        choices=["service", "user-session", "connect-only"],
    )
    parser.add_argument("--identity-dir", default=os.environ.get("AGENTD_CODEXW_IDENTITY_DIR", ".codexw-agentd/native"))
    parser.add_argument("--deployment-cert-path", default=os.environ.get("AGENTD_CODEXW_DEPLOYMENT_CERT_PATH", ""))
    parser.add_argument("--deployment-key-path", default=os.environ.get("AGENTD_CODEXW_DEPLOYMENT_KEY_PATH", ""))
    parser.add_argument("--agentd-base-url", default=os.environ.get("AGENTD_BASE_URL", "http://127.0.0.1:8123"))
    parser.add_argument("--agentd-auth-token", default=os.environ.get("AGENTD_AUTH_TOKEN", ""))
    parser.add_argument("--broker-user", default=os.environ.get("AGENTD_CODEXW_BROKER_USER", ""))
    parser.add_argument("--broker-password", default=os.environ.get("AGENTD_CODEXW_BROKER_PASSWORD", ""))
    parser.add_argument("--broker-token", default=os.environ.get("AGENTD_CODEXW_BROKER_TOKEN", ""))
    parser.add_argument("--timestamp", type=int, default=0, help="fixed Unix timestamp for deterministic tests")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--dry-run", action="store_true", help="print broker-ready identity/snapshot JSON")
    parser.add_argument("--self-test", action="store_true", help="run a read-only connector readiness check")
    parser.add_argument("--require-broker-visible", action="store_true", help="self-test fails unless the broker reports this runtime online")
    parser.add_argument("--connect", action="store_true", help="open the native broker deployment websocket")
    parser.add_argument("--once", action="store_true", help="exit after the first command result or broker close")
    parser.add_argument("--reconnect", action="store_true", help="reconnect after broker disconnects or transient errors")
    parser.add_argument("--reconnect-initial-delay", type=float, default=1.0)
    parser.add_argument("--reconnect-max-delay", type=float, default=30.0)
    parser.add_argument("--snapshot-interval", type=float, default=30.0)
    parser.add_argument("--max-runtime-seconds", type=float, default=0.0)
    parser.add_argument("--enroll-certificate", action="store_true", help="POST a CSR to the broker enrollment endpoint")
    parser.add_argument("--bootstrap-identity", action="store_true", help="generate key/CSR and enroll a cert when missing")
    parser.add_argument("--csr-path", default="")
    parser.add_argument("--force-csr", action="store_true")
    parser.add_argument("--enrollment-material-path", default="")
    parser.add_argument("--certificate-days", type=int, default=365)
    parser.add_argument("--enrollment-token-id", default=os.environ.get("AGENTD_CODEXW_ENROLLMENT_TOKEN_ID", ""))
    parser.add_argument("--enrollment-shared-secret", default=os.environ.get("AGENTD_CODEXW_ENROLLMENT_SECRET", ""))
    args = parser.parse_args(argv)
    if not str(args.broker_url).strip():
        parser.error("--broker-url or AGENTD_CODEXW_BROKER_URL is required")
    if not str(args.deployment_id).strip():
        parser.error("--deployment-id or AGENTD_CODEXW_DEPLOYMENT_ID is required")
    if not str(args.agentd_base_url).strip():
        parser.error("--agentd-base-url or AGENTD_BASE_URL is required")
    resolve_identity_paths(args)
    if not args.runtime_instance_id:
        args.runtime_instance_id = default_runtime_instance_id()
    modes = sum(1 for enabled in (args.dry_run, args.self_test, args.connect, args.enroll_certificate) if enabled)
    if modes != 1:
        parser.error("choose exactly one of --dry-run, --self-test, --connect, or --enroll-certificate")
    if args.self_test and args.bootstrap_identity:
        parser.error("--self-test is read-only and cannot be combined with --bootstrap-identity")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    identity = prepare_identity(args)
    if args.enroll_certificate:
        response = enroll_certificate(args)
        if args.bootstrap_identity:
            persist_enrollment_response(args, response)
            identity = {**identity, "enrolled_certificate": True}
        print(json_pretty({"ok": True, "identity": identity, "enrollment": response}))
        return 0
    if args.connect:
        print(json_pretty({**run_connect_supervised(args), "identity": identity}))
        return 0
    if args.self_test:
        result = {**run_self_test(args), "identity": identity}
        print(json_pretty(result))
        return 0 if result.get("ok") else 1
    print(json_pretty({**build_dry_run_payload(args), "identity": identity}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
