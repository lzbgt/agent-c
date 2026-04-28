#!/usr/bin/env python3
"""Shared agentd <-> codexw broker runtime contract helpers."""

from __future__ import annotations

import base64
import hashlib
import json
import platform
import socket
import time
import uuid
from typing import Any


LOCAL_API_VERSION = "agentd-codexw-facade-v1"
RUNTIME_KIND = "agentd"
RUNTIME_CAPABILITIES_SCHEMA = "broker.runtime_capabilities.v1"

RUNTIME_ACTIONS: dict[str, dict[str, Any]] = {
    "workflow.submit": {
        "transport": "local_api",
        "method": "POST",
        "path": "/api/v1/runtime/actions",
        "input": "agentd_workflow_submit_request",
    },
    "workflow.read": {
        "transport": "local_api",
        "method": "POST",
        "path": "/api/v1/runtime/actions",
        "input": {"workflow_id": "string"},
    },
    "workflow.cancel": {
        "transport": "local_api",
        "method": "POST",
        "path": "/api/v1/runtime/actions",
        "input": {"workflow_id": "string"},
    },
    "schedule.list": {
        "transport": "local_api",
        "method": "POST",
        "path": "/api/v1/runtime/actions",
        "input": {"status": "optional string", "limit": "optional integer", "offset": "optional integer"},
    },
    "experience.list": {
        "transport": "local_api",
        "method": "POST",
        "path": "/api/v1/runtime/actions",
        "input": "agentd_experience_record_filter",
    },
    "experience.export": {
        "transport": "local_api",
        "method": "POST",
        "path": "/api/v1/runtime/actions",
        "input": "agentd_experience_record_filter",
    },
}


def runtime_capabilities() -> dict[str, Any]:
    return {
        "schema": RUNTIME_CAPABILITIES_SCHEMA,
        "runtime_kind": RUNTIME_KIND,
        "actions": RUNTIME_ACTIONS,
        "surfaces": {
            "workflow": True,
            "schedule": True,
            "experience": True,
            "transcript": True,
            "files": True,
        },
    }


def legacy_capabilities() -> list[str]:
    return [
        "agentd.run",
        "agentd.workflow",
        "agentd.schedule",
        "agentd.rl.experience_records",
        "codexw.local_api.runtime",
        "codexw.local_api.runtime_actions",
        "codexw.local_api.turn_start",
        "codexw.local_api.transcript",
    ]


def canonical_json_bytes(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def runtime_capabilities_hash(capabilities: dict[str, Any] | None = None) -> str:
    body = canonical_json_bytes(capabilities if capabilities is not None else runtime_capabilities())
    return hashlib.sha256(body).hexdigest()


def default_runtime_instance_id(prefix: str = "agentd") -> str:
    return f"{prefix}-{uuid.uuid4().hex[:12]}"


def host_id() -> str:
    value = socket.gethostname().strip() or platform.node().strip()
    return value or "unknown-host"


def target_os() -> str:
    return platform.system().lower() or "unknown"


def target_arch() -> str:
    return platform.machine() or "unknown"


def runtime_identity_headers(
    *,
    instance_id: str,
    connection_mode: str = "service",
    capabilities: dict[str, Any] | None = None,
    host: str | None = None,
    os_name: str | None = None,
    arch: str | None = None,
) -> dict[str, str]:
    del connection_mode
    return {
        "X-Codexw-Runtime-Kind": RUNTIME_KIND,
        "X-Codexw-Runtime-Instance-Id": instance_id,
        "X-Codexw-Runtime-Capabilities-SHA256": runtime_capabilities_hash(capabilities),
        "X-Codexw-Runtime-Host-Id": host or host_id(),
        "X-Codexw-Runtime-Target-OS": os_name or target_os(),
        "X-Codexw-Runtime-Target-Arch": arch or target_arch(),
    }


def runtime_snapshot(
    *,
    instance_id: str,
    deployment_id: str,
    agentd_base_url: str,
    process_started_at_ms: int | None = None,
    connection_mode: str = "service",
    preferred_broker_transport: str = "native_deployment_connect",
    implementation: str = "native_agentd_broker_connector",
    agentd_health: Any | None = None,
    agentd_capabilities: Any | None = None,
) -> dict[str, Any]:
    return {
        "ok": True,
        "local_api_version": LOCAL_API_VERSION,
        "runtime": {
            "kind": RUNTIME_KIND,
            "runtime_kind": RUNTIME_KIND,
            "instance_id": instance_id,
            "process_started_at_ms": process_started_at_ms or int(time.time() * 1000),
            "suggested_deployment_id": deployment_id,
            "connection_mode": connection_mode,
            "version": LOCAL_API_VERSION,
            "host_os": target_os(),
            "host_arch": target_arch(),
            "preferred_broker_transport": preferred_broker_transport,
            "recommended_remote_clients": ["ios"],
            "agentd_base_url": agentd_base_url.rstrip("/"),
            "agentd_health": agentd_health,
            "agentd_capabilities": agentd_capabilities,
            "capabilities": legacy_capabilities(),
            "runtime_capabilities": runtime_capabilities(),
            "runtime_capabilities_hash": runtime_capabilities_hash(),
            "live_session": {
                "status": "available",
                "transport": "broker_deployment_connect",
                "implementation": implementation,
                "session_mode": "runtime_actions",
                "transport_ready": True,
                "requires_broker_auth": True,
                "requires_peer_mutual_auth": True,
            },
        },
    }


def deployment_snapshot_frame(
    *,
    deployment_id: str,
    display_name: str,
    runtime: dict[str, Any],
    session: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "type": "deployment.snapshot",
        "deployment_id": deployment_id,
        "display_name": display_name,
        "runtime": runtime,
        "session": session,
        "last_error": None,
    }


def base64_body_digest(body: bytes) -> str:
    return base64.b64encode(hashlib.sha256(body).digest()).decode("ascii")
