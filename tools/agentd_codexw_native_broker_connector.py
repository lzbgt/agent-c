#!/usr/bin/env python3
"""Prepare native agentd deployment-connect identity for a codexw broker."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from agentd_codexw_contract import (
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


def json_dumps(obj: Any) -> bytes:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


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
    proc = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(private_key_path)],
        input=message,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode("utf-8", errors="replace").strip())
    return base64.b64encode(proc.stdout).decode("ascii")


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


def enroll_certificate(args: argparse.Namespace) -> dict[str, Any]:
    if not args.enrollment_token_id or not args.enrollment_shared_secret:
        raise ValueError("--enrollment-token-id and --enrollment-shared-secret are required")
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


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broker-url", required=True, help="codexw broker base URL, e.g. https://broker.example")
    parser.add_argument("--deployment-id", required=True)
    parser.add_argument("--display-name", default="")
    parser.add_argument("--runtime-instance-id", default="")
    parser.add_argument("--runtime-host-id", default="")
    parser.add_argument("--runtime-target-os", default="")
    parser.add_argument("--runtime-target-arch", default="")
    parser.add_argument("--connection-mode", default="service", choices=["service", "user-session", "connect-only"])
    parser.add_argument("--deployment-cert-path", required=True)
    parser.add_argument("--deployment-key-path", required=True)
    parser.add_argument("--agentd-base-url", required=True)
    parser.add_argument("--agentd-auth-token", default=os.environ.get("AGENTD_AUTH_TOKEN", ""))
    parser.add_argument("--timestamp", type=int, default=0, help="fixed Unix timestamp for deterministic tests")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--dry-run", action="store_true", help="print broker-ready identity/snapshot JSON")
    parser.add_argument("--enroll-certificate", action="store_true", help="POST a CSR to the broker enrollment endpoint")
    parser.add_argument("--csr-path", default="")
    parser.add_argument("--certificate-days", type=int, default=365)
    parser.add_argument("--enrollment-token-id", default="")
    parser.add_argument("--enrollment-shared-secret", default="")
    args = parser.parse_args(argv)
    if not args.runtime_instance_id:
        args.runtime_instance_id = default_runtime_instance_id()
    if args.enroll_certificate and not args.csr_path:
        parser.error("--csr-path is required with --enroll-certificate")
    if not args.dry_run and not args.enroll_certificate:
        parser.error("choose --dry-run or --enroll-certificate")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.enroll_certificate:
        print(json.dumps(enroll_certificate(args), indent=2, sort_keys=True))
        return 0
    print(json.dumps(build_dry_run_payload(args), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
