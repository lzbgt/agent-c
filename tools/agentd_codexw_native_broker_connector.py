#!/usr/bin/env python3
"""Prepare native agentd deployment-connect identity for a codexw broker."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import mimetypes
import os
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from agentd_codexw_contract import (
    agentd_runtime_events,
    agentd_runtime_session_create,
    agentd_runtime_sessions,
    agentd_runtime_status,
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


def log_connector_event(args: argparse.Namespace, event: str, **fields: Any) -> None:
    payload = {
        "event": event,
        "deployment_id": getattr(args, "deployment_id", ""),
        "runtime_instance_id": getattr(args, "runtime_instance_id", ""),
        **fields,
    }
    print(json.dumps(payload, sort_keys=True), file=sys.stderr, flush=True)
DEFAULT_CERT_NAME = "deployment.cert.pem"
DEFAULT_CSR_NAME = "deployment.csr.pem"
DEFAULT_ENROLLMENT_MATERIAL_NAME = "deployment.enrollment.json"
RUNTIME_UPDATE_MODE_DISABLED = "disabled"
RUNTIME_UPDATE_MODE_AGENTD_OTA = "agentd_ota"
RUNTIME_RESTART_MODE_DISABLED = "disabled"
RUNTIME_RESTART_MODE_AGENTD_OTA = "agentd_ota"
DEFAULT_SELF_TEST_STALE_AFTER_SECONDS = 900


def env_bool(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def env_int(name: str, default: int) -> int:
    value = os.environ.get(name)
    if value is None or not value.strip():
        return default
    try:
        return int(value)
    except ValueError:
        return default


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


def write_json_private(path: Path, payload: dict[str, Any], mode: int = 0o600) -> None:
    path = path.expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    tmp_path.write_text(json_pretty(payload) + "\n", encoding="utf-8")
    tmp_path.chmod(mode)
    tmp_path.replace(path)


def unix_ms(args: argparse.Namespace | None = None) -> int:
    if args is not None and getattr(args, "timestamp", 0):
        return int(args.timestamp) * 1000
    return int(time.time() * 1000)


def connector_readiness_status(args: argparse.Namespace) -> dict[str, Any]:
    source = "agentd.codexw.self_test"
    stale_after_ms = max(0, int(getattr(args, "self_test_stale_after_seconds", DEFAULT_SELF_TEST_STALE_AFTER_SECONDS))) * 1000
    status_path = str(getattr(args, "self_test_output_path", "") or "").strip()
    if not status_path:
        return {
            "source": source,
            "available": False,
            "ok": False,
            "state": "unconfigured",
            "policy_state": "missing",
            "detail": "AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH is not configured",
            "stale_after_ms": stale_after_ms,
        }
    path = Path(status_path).expanduser()
    if not path.exists():
        return {
            "source": source,
            "available": False,
            "ok": False,
            "state": "missing",
            "policy_state": "missing",
            "detail": "durable connector self-test status file has not been written yet",
            "stale_after_ms": stale_after_ms,
        }
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        return {
            "source": source,
            "available": True,
            "ok": False,
            "state": "invalid",
            "policy_state": "failed",
            "detail": f"durable connector self-test status file is not valid JSON: {exc}",
            "stale_after_ms": stale_after_ms,
        }
    if not isinstance(payload, dict):
        return {
            "source": source,
            "available": True,
            "ok": False,
            "state": "invalid",
            "policy_state": "failed",
            "detail": "durable connector self-test status file must contain a JSON object",
            "stale_after_ms": stale_after_ms,
        }
    checks = payload.get("checks") if isinstance(payload.get("checks"), list) else []
    failed_checks = [
        str(check.get("name") or "unnamed")
        for check in checks
        if isinstance(check, dict) and not bool(check.get("ok"))
    ]
    last_ok = bool(payload.get("ok"))
    checked_ms = payload.get("checked_unix_ms")
    if not isinstance(checked_ms, (int, float)):
        checked_ms = None
    age_ms = max(0, unix_ms(args) - int(checked_ms)) if checked_ms is not None else None
    stale = checked_ms is None or (stale_after_ms > 0 and age_ms is not None and age_ms > stale_after_ms)
    if not last_ok:
        state = "failed"
        detail = (
            "failed checks: " + ", ".join(failed_checks[:5])
            if failed_checks
            else "last connector self-test failed"
        )
    elif stale:
        state = "stale"
        detail = "last connector self-test is stale"
    else:
        state = "fresh"
        detail = "last connector self-test passed"
    result: dict[str, Any] = {
        "source": source,
        "available": True,
        "ok": state == "fresh",
        "last_ok": last_ok,
        "state": state,
        "policy_state": state,
        "detail": detail,
        "check_count": len([check for check in checks if isinstance(check, dict)]),
        "failed_check_count": len(failed_checks),
        "failed_checks": failed_checks[:20],
        "mode": str(payload.get("mode") or ""),
        "runtime_instance_id": str(payload.get("runtime_instance_id") or ""),
        "deployment_id": str(payload.get("deployment_id") or ""),
        "stale_after_ms": stale_after_ms,
    }
    if checked_ms is not None:
        result["checked_unix_ms"] = int(checked_ms)
        result["age_ms"] = age_ms
    return result


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
            capabilities=runtime_capabilities_for_args(args),
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
    try:
        with opener.open(request, timeout=args.timeout) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        detail = raw.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"agentd {method} {path} failed ({exc.code}): {detail}") from exc
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


def websocket_read_json(sock: socket.socket, send_lock: threading.Lock | None = None) -> dict[str, Any]:
    while True:
        opcode, payload = websocket_read_frame(sock)
        if opcode == 0x1:
            obj = json.loads(payload.decode("utf-8"))
            if not isinstance(obj, dict):
                raise ValueError("websocket JSON frame must be an object")
            return obj
        if opcode == 0x8:
            code = None
            reason = ""
            if len(payload) >= 2:
                code = struct.unpack("!H", payload[:2])[0]
                reason = payload[2:].decode("utf-8", errors="replace")
            detail = "websocket close frame received"
            if code is not None:
                detail += f" code={code}"
            if reason:
                detail += f" reason={reason}"
            raise EOFError(detail)
        if opcode == 0x9:
            if send_lock is None:
                websocket_send_frame(sock, 0xA, payload)
            else:
                with send_lock:
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


def first_submit_prompt(input_obj: dict[str, Any]) -> str:
    for key in ("prompt", "text", "message", "objective", "title"):
        value = input_obj.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def workflow_submit_request(input_obj: dict[str, Any]) -> dict[str, Any]:
    if isinstance(input_obj.get("tasks"), list) and input_obj["tasks"]:
        return input_obj
    prompt = first_submit_prompt(input_obj)
    if not prompt:
        raise ValueError("workflow.submit input requires tasks or a text prompt")
    request = {
        "prompt": prompt,
        "no_session": bool(input_obj.get("no_session", True)),
        "tools": str(input_obj.get("tools") or "none"),
    }
    for key in ("model", "base_url", "timeout_ms", "max_steps", "session_id"):
        value = input_obj.get(key)
        if value not in (None, ""):
            request[key] = value
    return {
        "allow_sessions": bool(input_obj.get("allow_sessions", True)),
        "tasks": [
            {
                "task_id": str(input_obj.get("task_id") or "codexw_prompt"),
                "request": request,
            }
        ],
    }


def voice_webrtc_peer_session_id(args: argparse.Namespace, input_obj: dict[str, Any]) -> str:
    value = input_obj.get("session_id")
    if isinstance(value, str) and value.strip():
        return value.strip()
    return codexw_session_id(args)


def voice_webrtc_peer_start_request(args: argparse.Namespace, input_obj: dict[str, Any]) -> dict[str, Any]:
    session_id = voice_webrtc_peer_session_id(args, input_obj)
    if bool(input_obj.get("ensure_session", True)):
        agentd_runtime_session_create(lambda m, p, b=None: agentd_request(args, m, p, b), {"session_id": session_id})
    request = dict(input_obj)
    request["session_id"] = session_id
    request["action"] = "start"
    if not str(request.get("broker_url") or "").strip() and str(args.broker_url or "").strip():
        request["broker_url"] = str(args.broker_url).strip()
    if not str(request.get("broker_token") or "").strip() and str(args.broker_token or "").strip():
        request["broker_token"] = str(args.broker_token).strip()
    if not str(request.get("broker_agent_id") or "").strip() and str(args.deployment_id or "").strip():
        request["broker_agent_id"] = str(args.deployment_id).strip()
    if not str(request.get("broker_deployment_id") or "").strip() and str(args.deployment_id or "").strip():
        request["broker_deployment_id"] = str(args.deployment_id).strip()
    return request


def voice_webrtc_peer_stop_request(args: argparse.Namespace, input_obj: dict[str, Any]) -> dict[str, Any]:
    request = dict(input_obj)
    request["session_id"] = voice_webrtc_peer_session_id(args, input_obj)
    request["action"] = "stop"
    return request


def operator_runtime_actions(args: argparse.Namespace) -> dict[str, dict[str, Any]]:
    actions: dict[str, dict[str, Any]] = {}
    if args.runtime_update_mode == RUNTIME_UPDATE_MODE_AGENTD_OTA:
        actions["runtime.update"] = {
            "transport": "local_api",
            "method": "POST",
            "path": "/api/v1/runtime/actions",
            "input": "agentd_ota_update_request",
            "safe_boundary": "agentd_ota_drain",
        }
    if runtime_restart_advertisable(args):
        actions["runtime.restart"] = {
            "transport": "local_api",
            "method": "POST",
            "path": "/api/v1/runtime/actions",
            "input": "agentd_ota_restart_request",
            "safe_boundary": "agentd_supervisor_restart_drain",
        }
    return actions


def runtime_capabilities_for_args(args: argparse.Namespace) -> dict[str, Any]:
    return runtime_capabilities(operator_runtime_actions(args))


def cached_agentd_ota_status(args: argparse.Namespace) -> dict[str, Any]:
    cached = getattr(args, "_agentd_ota_status_cache", None)
    if isinstance(cached, dict):
        return cached
    try:
        status = agentd_request(args, "GET", "/api/v1/ota/status")
        if not isinstance(status, dict):
            status = {"ok": True, "value": status}
    except Exception as exc:  # noqa: BLE001
        setattr(args, "_agentd_ota_status_cache_error", str(exc))
        status = {}
    setattr(args, "_agentd_ota_status_cache", status)
    return status


def runtime_restart_status_from_args(args: argparse.Namespace) -> dict[str, Any]:
    status = cached_agentd_ota_status(args)
    restart = status.get("restart") if isinstance(status.get("restart"), dict) else {}
    return restart


def fresh_runtime_restart_status_from_args(args: argparse.Namespace) -> dict[str, Any]:
    status = agentd_request(args, "GET", "/api/v1/ota/status")
    if not isinstance(status, dict):
        return {}
    restart = status.get("restart") if isinstance(status.get("restart"), dict) else {}
    return restart


def runtime_restart_advertisable(args: argparse.Namespace) -> bool:
    if args.runtime_restart_mode != RUNTIME_RESTART_MODE_AGENTD_OTA:
        return False
    restart = runtime_restart_status_from_args(args)
    return (
        bool(restart.get("enabled"))
        and str(restart.get("safe_boundary") or "") == "agentd_supervisor_restart_drain"
    )


def first_string_from_any(obj: Any, keys: tuple[str, ...]) -> str:
    if not isinstance(obj, dict):
        return ""
    for key in keys:
        value = obj.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def runtime_update_request_body(input_obj: dict[str, Any]) -> dict[str, Any]:
    artifact = input_obj.get("artifact")
    release = input_obj.get("release")
    url = (
        first_string_from_any(input_obj, ("url", "artifact_url", "download_url"))
        or first_string_from_any(artifact, ("url", "download_url"))
        or first_string_from_any(release, ("url", "download_url"))
    )
    if not url:
        raise ValueError("runtime.update input.url is required")
    sha256 = (
        first_string_from_any(input_obj, ("sha256", "artifact_sha256"))
        or first_string_from_any(artifact, ("sha256", "artifact_sha256"))
        or first_string_from_any(release, ("sha256", "artifact_sha256"))
    )
    version = (
        first_string_from_any(input_obj, ("version", "target_version"))
        or first_string_from_any(artifact, ("version", "name"))
        or first_string_from_any(release, ("version", "name"))
    )
    reason = first_string_from_any(input_obj, ("reason",)) or "codexw broker runtime.update"
    body: dict[str, Any] = {"url": url, "reason": reason}
    if sha256:
        body["sha256"] = sha256
    if version:
        body["version"] = version
    drain_timeout_ms = input_obj.get("drain_timeout_ms")
    if isinstance(drain_timeout_ms, (int, float)) or (isinstance(drain_timeout_ms, str) and drain_timeout_ms.strip()):
        try:
            parsed_timeout = int(drain_timeout_ms)
        except Exception as exc:  # noqa: BLE001
            raise ValueError("runtime.update input.drain_timeout_ms must be an integer") from exc
        if parsed_timeout < 0:
            raise ValueError("runtime.update input.drain_timeout_ms must be non-negative")
        body["drain_timeout_ms"] = parsed_timeout
    return body


def runtime_restart_request_body(input_obj: dict[str, Any], command_body: dict[str, Any]) -> dict[str, Any]:
    reason = (
        first_string_from_any(input_obj, ("reason",))
        or first_string_from_any(command_body, ("reason",))
        or "codexw broker runtime.restart"
    )
    body: dict[str, Any] = {"reason": reason}
    idempotency_key = (
        first_string_from_any(input_obj, ("idempotency_key",))
        or first_string_from_any(command_body, ("idempotency_key",))
    )
    if idempotency_key:
        body["idempotency_key"] = idempotency_key
    trace_id = first_string_from_any(input_obj, ("trace_id",)) or first_string_from_any(command_body, ("trace_id",))
    if trace_id:
        body["trace_id"] = trace_id
    drain_timeout_ms = input_obj.get("drain_timeout_ms", command_body.get("drain_timeout_ms"))
    if isinstance(drain_timeout_ms, (int, float)) or (isinstance(drain_timeout_ms, str) and drain_timeout_ms.strip()):
        try:
            parsed_timeout = int(drain_timeout_ms)
        except Exception as exc:  # noqa: BLE001
            raise ValueError("runtime.restart input.drain_timeout_ms must be an integer") from exc
        if parsed_timeout < 0:
            raise ValueError("runtime.restart input.drain_timeout_ms must be non-negative")
        body["drain_timeout_ms"] = parsed_timeout
    return body


def forward_runtime_action(args: argparse.Namespace, body: dict[str, Any]) -> dict[str, Any]:
    action = body.get("action")
    if not isinstance(action, str) or not action.strip():
        raise ValueError("runtime action is required")
    action = action.strip()
    input_obj = request_input(body)
    if action == "workflow.submit":
        result = agentd_request(args, "POST", "/api/v1/workflow/submit", workflow_submit_request(input_obj))
    elif action == "workflow.read":
        workflow_id = require_string(input_obj, "workflow_id")
        result = agentd_request(
            args,
            "GET",
            "/api/v1/workflow?"
            + urllib.parse.urlencode(
                {"workflow_id": workflow_id, "include_tasks": "1", "include_results": "1"}
            ),
        )
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
    elif action == "voice.webrtc_peer.status":
        session_id = voice_webrtc_peer_session_id(args, input_obj)
        result = agentd_request(
            args,
            "GET",
            "/api/v1/session/voice_webrtc_peer?" + urllib.parse.urlencode({"session_id": session_id}),
        )
    elif action == "voice.webrtc_peer.start":
        result = agentd_request(args, "POST", "/api/v1/session/voice_webrtc_peer", voice_webrtc_peer_start_request(args, input_obj))
    elif action == "voice.webrtc_peer.stop":
        result = agentd_request(args, "POST", "/api/v1/session/voice_webrtc_peer", voice_webrtc_peer_stop_request(args, input_obj))
    elif action == "runtime.update":
        if args.runtime_update_mode != RUNTIME_UPDATE_MODE_AGENTD_OTA:
            raise ValueError("runtime.update is disabled; set AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota")
        result = agentd_request(args, "POST", "/api/v1/ota/update", runtime_update_request_body(input_obj))
    elif action == "runtime.restart":
        if args.runtime_restart_mode != RUNTIME_RESTART_MODE_AGENTD_OTA:
            raise ValueError("runtime.restart is disabled; set AGENTD_CODEXW_RUNTIME_RESTART_MODE=agentd_ota")
        restart = fresh_runtime_restart_status_from_args(args)
        if not bool(restart.get("enabled")):
            raise ValueError("runtime.restart is not enabled by agentd /api/v1/ota/status restart policy")
        if str(restart.get("safe_boundary") or "") != "agentd_supervisor_restart_drain":
            raise ValueError("runtime.restart safe_boundary is not agentd_supervisor_restart_drain")
        result = agentd_request(args, "POST", "/api/v1/ota/restart", runtime_restart_request_body(input_obj, body))
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
        process_started_at_ms=args.connector_started_at_ms,
        connection_mode=args.connection_mode,
        preferred_broker_transport="native_deployment_connect",
        implementation="native_agentd_broker_connector",
        agentd_health=health,
        agentd_capabilities=caps,
        runtime_capabilities_manifest=runtime_capabilities_for_args(args),
    )


def codexw_session_id(args: argparse.Namespace) -> str:
    return f"agentd:{args.deployment_id}"


def local_session_snapshot(args: argparse.Namespace) -> dict[str, Any]:
    session_id = codexw_session_id(args)
    return {
        "ok": True,
        "local_api_version": "agentd-codexw-native-v1",
        "session_id": session_id,
        "thread_id": session_id,
        "session": {
            "id": session_id,
            "scope": "process",
            "title": "agentd broker session",
            "status": "active",
            "attached_thread_id": session_id,
            "attachment": {
                "id": f"attach:{session_id}",
                "client_id": "client_mobile",
                "lease_seconds": 300,
                "attached_thread_id": session_id,
            },
        },
        "working": False,
        "process_scoped": True,
    }


def session_route_tail(args: argparse.Namespace, route_path: str) -> str | None:
    prefix = f"/api/v1/session/{codexw_session_id(args)}"
    if route_path == prefix:
        return ""
    if route_path.startswith(prefix + "/"):
        return route_path[len(prefix) + 1 :]
    generic_prefix = "/api/v1/session/"
    if route_path.startswith(generic_prefix):
        parts = route_path[len(generic_prefix) :].split("/", 1)
        if len(parts) == 2:
            return parts[1]
    return None


def file_root(args: argparse.Namespace) -> Path:
    return Path(args.file_root).expanduser().resolve()


def safe_file_target(args: argparse.Namespace, raw_path: str | None) -> Path:
    root = file_root(args)
    value = str(raw_path or "").strip()
    candidate = Path(value).expanduser() if value else root
    if not candidate.is_absolute():
        candidate = root / value.lstrip("/")
    target = candidate.resolve()
    if target != root and root not in target.parents:
        raise ValueError(f"path is outside agentd file root: {target}")
    return target


def relative_to_file_root(args: argparse.Namespace, path: Path) -> str:
    root = file_root(args)
    try:
        return str(path.resolve().relative_to(root))
    except ValueError:
        return str(path)


def local_file_entry(args: argparse.Namespace, path: Path) -> dict[str, Any]:
    stat = path.stat()
    content_type = "" if path.is_dir() else (mimetypes.guess_type(str(path))[0] or "application/octet-stream")
    return {
        "name": path.name or str(path),
        "path": str(path),
        "relative_path": relative_to_file_root(args, path),
        "is_directory": path.is_dir(),
        "size_bytes": None if path.is_dir() else stat.st_size,
        "content_type": content_type,
    }


def local_file_list(args: argparse.Namespace, body: Any | None) -> dict[str, Any]:
    request = body if isinstance(body, dict) else {}
    target = safe_file_target(args, request.get("path"))
    if target.is_file():
        target = target.parent
    if not target.exists() or not target.is_dir():
        raise ValueError(f"directory not found: {target}")
    offset = max(0, int(request.get("offset") or 0))
    limit = max(1, min(int(request.get("limit") or 80), 500))
    all_entries = sorted(target.iterdir(), key=lambda item: (not item.is_dir(), item.name.lower()))
    page = all_entries[offset : offset + limit]
    next_offset = offset + len(page) if offset + len(page) < len(all_entries) else None
    parent_path = str(target.parent) if target != file_root(args) else ""
    return {
        "ok": True,
        "session_id": codexw_session_id(args),
        "root_path": str(file_root(args)),
        "path": str(target),
        "relative_path": relative_to_file_root(args, target),
        "parent_path": parent_path,
        "offset": offset,
        "limit": limit,
        "returned": len(page),
        "total_entries": len(all_entries),
        "next_offset": next_offset,
        "has_more": next_offset is not None,
        "entries": [local_file_entry(args, item) for item in page],
    }


def local_file_read(args: argparse.Namespace, body: Any | None) -> dict[str, Any]:
    request = body if isinstance(body, dict) else {}
    target = safe_file_target(args, request.get("path") or request.get("filename"))
    if not target.is_file():
        raise ValueError(f"file not found: {target}")
    offset = max(0, int(request.get("offset") or 0))
    limit = max(1, min(int(request.get("limit") or 65536), 512 * 1024))
    size = target.stat().st_size
    with target.open("rb") as handle:
        handle.seek(offset)
        chunk = handle.read(limit)
    next_offset = offset + len(chunk) if offset + len(chunk) < size else None
    return {
        "ok": True,
        "session_id": codexw_session_id(args),
        "root_path": str(file_root(args)),
        "path": str(target),
        "relative_path": relative_to_file_root(args, target),
        "filename": target.name,
        "content_type": mimetypes.guess_type(str(target))[0] or "application/octet-stream",
        "size_bytes": size,
        "offset": offset,
        "returned_bytes": len(chunk),
        "next_offset": next_offset,
        "has_more": next_offset is not None,
        "preview_truncated": next_offset is not None,
        "data_base64": base64.b64encode(chunk).decode("ascii"),
    }


def shell_jobs(args: argparse.Namespace) -> dict[str, dict[str, Any]]:
    jobs = getattr(args, "_codexw_shell_jobs", None)
    if not isinstance(jobs, dict):
        jobs = {}
        setattr(args, "_codexw_shell_jobs", jobs)
    return jobs


def local_shell_job(args: argparse.Namespace, shell_id: str, command: str, cwd: Path, output: str, exit_code: int) -> dict[str, Any]:
    lines = output.splitlines()
    return {
        "id": shell_id,
        "pid": 0,
        "command": command,
        "cwd": str(cwd),
        "intent": "observation",
        "label": "agentd broker shell",
        "alias": "",
        "service_capabilities": [],
        "dependency_capabilities": [],
        "interaction_recipe_names": [],
        "origin": {"source_tool": "agentd_codexw_shell"},
        "status": "completed" if exit_code == 0 else "failed",
        "exit_code": exit_code,
        "total_lines": len(lines),
        "last_output_age_seconds": 0,
        "recent_lines": lines[-80:],
        "output_lines": lines[-400:],
    }


def local_shell_list(args: argparse.Namespace) -> dict[str, Any]:
    return {"ok": True, "session_id": codexw_session_id(args), "shells": list(shell_jobs(args).values())}


def local_shell_start(args: argparse.Namespace, body: Any | None) -> dict[str, Any]:
    request = body if isinstance(body, dict) else {}
    command = str(request.get("command") or "").strip()
    if not command:
        raise ValueError("shell command is required")
    cwd = safe_file_target(args, request.get("cwd")) if request.get("cwd") else file_root(args)
    if not cwd.is_dir():
        cwd = cwd.parent
    try:
        proc = subprocess.run(
            command,
            cwd=str(cwd),
            shell=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=max(1.0, float(args.shell_timeout_seconds)),
            check=False,
        )
        output = proc.stdout
        exit_code = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
        output += f"\ncommand timed out after {args.shell_timeout_seconds}s"
        exit_code = 124
    shell_id = f"agentd-shell-{int(time.time() * 1000)}"
    job = local_shell_job(args, shell_id, command, cwd, output[-20000:], exit_code)
    if label := str(request.get("label") or "").strip():
        job["label"] = label
    if intent := str(request.get("intent") or "").strip():
        job["intent"] = intent
    shell_jobs(args)[shell_id] = job
    return {"ok": True, "session_id": codexw_session_id(args), "shell": job}


def local_shell_read(args: argparse.Namespace, reference: str) -> dict[str, Any]:
    job = shell_jobs(args).get(reference)
    if not job:
        raise KeyError(reference)
    return {"ok": True, "session_id": codexw_session_id(args), "shell": job}


def local_shell_terminate(args: argparse.Namespace, reference: str) -> dict[str, Any]:
    job = shell_jobs(args).get(reference)
    if not job:
        raise KeyError(reference)
    job["status"] = "completed"
    return {"ok": True, "session_id": codexw_session_id(args), "shell": job}


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
        session_tail = session_route_tail(args, route_path)
        if method == "GET" and route_path == "/api/v1/runtime":
            result = runtime_payload(args)
        elif method == "GET" and route_path in ("/api/v1/session", f"/api/v1/session/{codexw_session_id(args)}"):
            result = local_session_snapshot(args)
        elif method == "GET" and route_path == "/api/v1/runtime/status":
            result = agentd_runtime_status(
                request_agentd,
                update_enabled=args.runtime_update_mode == RUNTIME_UPDATE_MODE_AGENTD_OTA,
                restart_enabled=args.runtime_restart_mode == RUNTIME_RESTART_MODE_AGENTD_OTA,
                connector=connector_readiness_status(args),
            )
        elif method == "GET" and route_path == "/api/v1/runtime/sessions":
            result = agentd_runtime_sessions(request_agentd)
        elif method == "POST" and route_path == "/api/v1/runtime/sessions":
            if not isinstance(body, dict):
                raise ValueError("runtime session-create command body must be an object")
            result = agentd_runtime_session_create(request_agentd, body)
        elif method == "GET" and route_path == "/api/v1/runtime/events":
            result = agentd_runtime_events(request_agentd, query)
        elif method == "POST" and route_path == "/api/v1/runtime/actions":
            if not isinstance(body, dict):
                raise ValueError("runtime action command body must be an object")
            result = forward_runtime_action(args, body)
        elif method == "GET" and route_path == "/healthz":
            result = {"ok": True, "service": "agentd-codexw-native-broker-connector"}
        elif session_tail == "shells" and method == "GET":
            result = local_shell_list(args)
        elif session_tail == "shells/start" and method == "POST":
            result = local_shell_start(args, body)
        elif session_tail and session_tail.startswith("shells/"):
            parts = session_tail.split("/")
            reference = parts[1] if len(parts) >= 2 else ""
            if method == "GET" and len(parts) == 2:
                result = local_shell_read(args, reference)
            elif method == "POST" and len(parts) == 3 and parts[2] == "terminate":
                result = local_shell_terminate(args, reference)
            elif method == "POST" and len(parts) == 3 and parts[2] == "send":
                raise ValueError("agentd codexw shell adapter runs one-shot commands; send is unsupported after completion")
            else:
                raise ValueError(f"unsupported shell route: {method} {route_path}")
        elif session_tail == "files/list" and method == "POST":
            result = local_file_list(args, body)
        elif session_tail == "files/read" and method == "POST":
            result = local_file_read(args, body)
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
        log_connector_event(
            args,
            "command_failed",
            method=method,
            path=route_path,
            error=str(exc),
        )
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
    log_connector_event(args, "connect_opening", url=broker_ws_url(args.broker_url))
    sock = websocket_connect(broker_ws_url(args.broker_url), headers, args.timeout)
    log_connector_event(args, "connect_opened")
    send_lock = threading.Lock()
    sent_commands = 0
    sent_commands_lock = threading.Lock()
    command_threads: list[threading.Thread] = []
    close_reason = ""
    stop_commands = threading.Event()

    def send_json(obj: dict[str, Any]) -> None:
        with send_lock:
            websocket_send_json(sock, obj)

    def send_frame(opcode: int, payload: bytes = b"") -> None:
        with send_lock:
            websocket_send_frame(sock, opcode, payload)

    def reap_command_threads() -> None:
        command_threads[:] = [thread for thread in command_threads if thread.is_alive()]

    def note_command_handled() -> int:
        nonlocal sent_commands
        with sent_commands_lock:
            sent_commands += 1
            return sent_commands

    def handle_command_background(frame: dict[str, Any]) -> None:
        try:
            result = handle_command(args, frame)
            if stop_commands.is_set():
                return
            send_json(result)
            count = note_command_handled()
            log_connector_event(args, "command_handled", commands=count)
        except Exception as exc:  # noqa: BLE001
            log_connector_event(
                args,
                "command_send_failed",
                request_id=str(frame.get("request_id") or ""),
                method=str(frame.get("method") or ""),
                path=str(frame.get("path") or ""),
                error=str(exc),
            )

    def heartbeat_loop() -> None:
        interval = max(1.0, float(getattr(args, "heartbeat_interval", 10.0)))
        while not stop_commands.wait(interval):
            try:
                send_json(
                    {
                        "type": "deployment.heartbeat",
                        "deployment_id": args.deployment_id,
                        "timestamp": str(int(time.time())),
                    }
                )
                log_connector_event(args, "heartbeat_sent")
            except Exception as exc:  # noqa: BLE001
                log_connector_event(args, "heartbeat_failed", error=str(exc))
                return

    try:
        send_json(
            {
                "type": "deployment.hello",
                "deployment_id": args.deployment_id,
                "display_name": args.display_name or args.deployment_id,
            },
        )
        send_json(
            deployment_snapshot_frame(
                deployment_id=args.deployment_id,
                display_name=args.display_name or args.deployment_id,
                runtime=runtime_payload(args),
                session=None,
            ),
        )
        next_snapshot = time.monotonic() + args.snapshot_interval
        deadline = time.monotonic() + args.max_runtime_seconds if args.max_runtime_seconds > 0 else None
        heartbeat_thread = threading.Thread(target=heartbeat_loop, daemon=True)
        heartbeat_thread.start()
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                break
            timeout = min(args.timeout, max(0.1, next_snapshot - time.monotonic()))
            if deadline is not None:
                timeout = min(timeout, max(0.1, deadline - time.monotonic()))
            sock.settimeout(timeout)
            try:
                frame = websocket_read_json(sock, send_lock=send_lock)
            except socket.timeout:
                frame = {}
            except EOFError as exc:
                close_reason = str(exc)
                log_connector_event(args, "connect_closed", reason=close_reason, commands=sent_commands)
                break
            if frame.get("type") == "deployment.command":
                if args.once:
                    send_json(handle_command(args, frame))
                    count = note_command_handled()
                    log_connector_event(args, "command_handled", commands=count)
                else:
                    thread = threading.Thread(
                        target=handle_command_background,
                        args=(frame,),
                        daemon=True,
                    )
                    command_threads.append(thread)
                    thread.start()
                if args.once:
                    break
            reap_command_threads()
            if time.monotonic() >= next_snapshot:
                send_json(
                    deployment_snapshot_frame(
                        deployment_id=args.deployment_id,
                        display_name=args.display_name or args.deployment_id,
                        runtime=runtime_payload(args),
                        session=None,
                    ),
                )
                next_snapshot = time.monotonic() + args.snapshot_interval
    finally:
        stop_commands.set()
        try:
            send_frame(0x8)
        except Exception:  # noqa: BLE001
            pass
        sock.close()
    return {
        "ok": True,
        "mode": "connect",
        "deployment_id": args.deployment_id,
        "commands": sent_commands,
        "close_reason": close_reason,
    }


def build_dry_run_payload(args: argparse.Namespace) -> dict[str, Any]:
    timestamp = str(int(args.timestamp or time.time()))
    capabilities = runtime_capabilities_for_args(args)
    runtime = runtime_snapshot(
        instance_id=args.runtime_instance_id,
        deployment_id=args.deployment_id,
        agentd_base_url=args.agentd_base_url,
        connection_mode=args.connection_mode,
        preferred_broker_transport="native_deployment_connect",
        implementation="native_agentd_broker_connector",
        runtime_capabilities_manifest=capabilities,
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
        "runtime_capabilities": capabilities,
        "runtime_capabilities_hash": runtime_capabilities_hash(capabilities),
        "runtime_capabilities_canonical_json_sha256": hashlib.sha256(
            canonical_json_bytes(capabilities)
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


def update_candidate_input_from_status(status: dict[str, Any]) -> dict[str, Any]:
    update = status.get("update") if isinstance(status.get("update"), dict) else {}
    candidate = update.get("candidate") if isinstance(update.get("candidate"), dict) else {}
    candidate_input = candidate.get("input") if isinstance(candidate.get("input"), dict) else {}
    if candidate_input:
        return candidate_input
    return candidate


def compare_preflight_update_input(*, status: dict[str, Any], preflight: dict[str, Any]) -> tuple[bool, dict[str, Any]]:
    prepared = preflight.get("prepared_input") if isinstance(preflight.get("prepared_input"), dict) else {}
    candidate_input = update_candidate_input_from_status(status)
    keys = ("url", "sha256", "version", "channel", "target_os", "target_arch", "drain_timeout_ms")
    mismatches: dict[str, Any] = {}
    for key in keys:
        expected = candidate_input.get(key)
        if expected in (None, ""):
            continue
        actual = prepared.get(key)
        if str(actual) != str(expected):
            mismatches[key] = {"expected": expected, "actual": actual}
    return not mismatches, {
        "prepared_input": prepared,
        "candidate_input": candidate_input,
        "mismatches": mismatches,
    }


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

    if args.runtime_update_mode == RUNTIME_UPDATE_MODE_AGENTD_OTA:
        try:
            ota_status = agentd_request(args, "GET", "/api/v1/ota/status")
            checks.append(check_status("agentd_ota_status", bool(ota_status.get("ok", True)), response=ota_status))
        except Exception as exc:  # noqa: BLE001
            checks.append(check_status("agentd_ota_status", False, error=str(exc)))
    if args.runtime_restart_mode == RUNTIME_RESTART_MODE_AGENTD_OTA:
        try:
            ota_status = agentd_request(args, "GET", "/api/v1/ota/status")
            restart = ota_status.get("restart") if isinstance(ota_status.get("restart"), dict) else {}
            checks.append(
                check_status(
                    "agentd_ota_restart_status",
                    bool(restart.get("enabled"))
                    and str(restart.get("safe_boundary") or "") == "agentd_supervisor_restart_drain",
                    response=restart or ota_status,
                )
            )
        except Exception as exc:  # noqa: BLE001
            checks.append(check_status("agentd_ota_restart_status", False, error=str(exc)))

    should_check_broker = bool(
        args.require_broker_visible
        or args.require_update_preflight
        or args.broker_token
        or (args.broker_user and args.broker_password)
    )
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
            if args.require_update_preflight:
                if not matched:
                    checks.append(
                        check_status(
                            "broker_runtime_update_preflight",
                            False,
                            error="runtime instance was not found in broker inventory",
                        )
                    )
                else:
                    instance_id = str(matched.get("instance_id") or args.runtime_instance_id).strip()
                    path_id = urllib.parse.quote(instance_id, safe="")
                    status = broker_json_request(
                        args,
                        method="GET",
                        path=f"/api/v2/runtime-instances/{path_id}/status",
                        bearer_token=token,
                    )
                    preflight = broker_json_request(
                        args,
                        method="POST",
                        path=f"/api/v2/runtime-instances/{path_id}/actions/preflight",
                        bearer_token=token,
                        body={
                            "action": "runtime.update",
                            "input": {
                                "reason": "agentd codexw connector self-test preflight",
                            },
                        },
                    )
                    update = status.get("update") if isinstance(status.get("update"), dict) else {}
                    input_ok, details = compare_preflight_update_input(status=status, preflight=preflight)
                    ok = (
                        bool(preflight.get("ok"))
                        and bool(preflight.get("preview"))
                        and not bool(preflight.get("mutates_runtime"))
                        and bool(preflight.get("would_dispatch"))
                        and bool(update.get("enabled"))
                        and input_ok
                    )
                    checks.append(
                        check_status(
                            "broker_runtime_update_preflight",
                            ok,
                            runtime_instance_id=instance_id,
                            update_enabled=bool(update.get("enabled")),
                            preview=bool(preflight.get("preview")),
                            mutates_runtime=bool(preflight.get("mutates_runtime")),
                            would_dispatch=bool(preflight.get("would_dispatch")),
                            **details,
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
        "checked_unix_ms": unix_ms(args),
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
        log_connector_event(args, "connect_attempt", attempt=attempts)
        try:
            result = run_connect(args)
            result["attempts"] = attempts
            result["last_error"] = last_error
            if args.once or not args.reconnect:
                return result
            log_connector_event(
                args,
                "connect_reconnect_scheduled",
                attempt=attempts,
                reason=result.get("close_reason") or "broker connection ended",
                delay_seconds=max(0.1, min(delay, args.reconnect_max_delay)),
            )
            delay = args.reconnect_initial_delay
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
            log_connector_event(args, "connect_error", attempt=attempts, error=last_error)
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
    parser.add_argument("--file-root", default=os.environ.get("AGENTD_CODEXW_FILE_ROOT", os.getcwd()))
    parser.add_argument(
        "--shell-timeout-seconds",
        type=float,
        default=env_int("AGENTD_CODEXW_SHELL_TIMEOUT_SECONDS", 20),
        help="maximum wall-clock seconds for one-shot codexw broker shell commands",
    )
    parser.add_argument("--broker-user", default=os.environ.get("AGENTD_CODEXW_BROKER_USER", ""))
    parser.add_argument("--broker-password", default=os.environ.get("AGENTD_CODEXW_BROKER_PASSWORD", ""))
    parser.add_argument("--broker-token", default=os.environ.get("AGENTD_CODEXW_BROKER_TOKEN", ""))
    parser.add_argument(
        "--runtime-update-mode",
        default=os.environ.get("AGENTD_CODEXW_RUNTIME_UPDATE_MODE", RUNTIME_UPDATE_MODE_DISABLED),
        choices=[RUNTIME_UPDATE_MODE_DISABLED, RUNTIME_UPDATE_MODE_AGENTD_OTA],
        help="advertise broker runtime.update only when agentd OTA owns the local safe update boundary",
    )
    parser.add_argument(
        "--runtime-restart-mode",
        default=os.environ.get("AGENTD_CODEXW_RUNTIME_RESTART_MODE", RUNTIME_RESTART_MODE_DISABLED),
        choices=[RUNTIME_RESTART_MODE_DISABLED, RUNTIME_RESTART_MODE_AGENTD_OTA],
        help="advertise broker runtime.restart only when agentd OTA status proves a supervisor restart boundary",
    )
    parser.add_argument("--timestamp", type=int, default=0, help="fixed Unix timestamp for deterministic tests")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--dry-run", action="store_true", help="print broker-ready identity/snapshot JSON")
    parser.add_argument("--self-test", action="store_true", help="run a read-only connector readiness check")
    parser.add_argument(
        "--self-test-output-path",
        default=os.environ.get("AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH", ""),
        help="optional path where self-test writes the last readiness result as JSON",
    )
    parser.add_argument(
        "--self-test-stale-after-seconds",
        type=int,
        default=env_int("AGENTD_CODEXW_SELF_TEST_STALE_AFTER_SECONDS", DEFAULT_SELF_TEST_STALE_AFTER_SECONDS),
        help="seconds before durable self-test status is reported stale in runtime status",
    )
    parser.add_argument("--require-broker-visible", action="store_true", help="self-test fails unless the broker reports this runtime online")
    parser.add_argument(
        "--require-update-preflight",
        action="store_true",
        default=env_bool("AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT"),
        help="self-test fails unless broker runtime.update preflight succeeds without mutating the runtime",
    )
    parser.add_argument("--connect", action="store_true", help="open the native broker deployment websocket")
    parser.add_argument("--once", action="store_true", help="exit after the first command result or broker close")
    parser.add_argument("--reconnect", action="store_true", help="reconnect after broker disconnects or transient errors")
    parser.add_argument("--reconnect-initial-delay", type=float, default=0.1)
    parser.add_argument("--reconnect-max-delay", type=float, default=30.0)
    parser.add_argument("--snapshot-interval", type=float, default=30.0)
    parser.add_argument("--heartbeat-interval", type=float, default=10.0)
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
    args.connector_started_at_ms = int((args.timestamp or time.time()) * 1000)
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
        if args.self_test_output_path:
            write_json_private(Path(args.self_test_output_path), result)
        print(json_pretty(result))
        return 0 if result.get("ok") else 1
    print(json_pretty({**build_dry_run_payload(args), "identity": identity}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
