#!/usr/bin/env python3
"""Shared agentd <-> codexw broker runtime contract helpers."""

from __future__ import annotations

import base64
import hashlib
import json
import platform
import socket
import time
import urllib.parse
import uuid
from typing import Any, Callable


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


def runtime_capabilities(
    operator_actions: dict[str, dict[str, Any]] | None = None,
    *,
    proxy_sse: bool = False,
) -> dict[str, Any]:
    actions = {name: dict(spec) for name, spec in RUNTIME_ACTIONS.items()}
    if operator_actions:
        actions.update({name: dict(spec) for name, spec in operator_actions.items()})
    surfaces = {
        "workflow": True,
        "schedule": True,
        "experience": True,
        "sessions": True,
        "session_create": True,
        "events": True,
        "status": True,
        "transcript": True,
        "files": True,
        "shell": True,
        "proxy_http": True,
    }
    if proxy_sse:
        surfaces["proxy_sse"] = True
    return {
        "schema": RUNTIME_CAPABILITIES_SCHEMA,
        "runtime_kind": RUNTIME_KIND,
        "actions": actions,
        "surfaces": surfaces,
    }


def legacy_capabilities() -> list[str]:
    return [
        "agentd.run",
        "agentd.workflow",
        "agentd.schedule",
        "agentd.rl.experience_records",
        "codexw.local_api.runtime",
        "codexw.local_api.runtime_actions",
        "codexw.local_api.runtime_sessions",
        "codexw.local_api.runtime_session_create",
        "codexw.local_api.runtime_events",
        "codexw.local_api.runtime_status",
        "codexw.local_api.runtime_proxy_http",
        "codexw.local_api.turn_start",
        "codexw.local_api.transcript",
        "codexw.local_api.files",
        "codexw.local_api.shell",
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
    runtime_capabilities_manifest: dict[str, Any] | None = None,
) -> dict[str, Any]:
    runtime_caps = runtime_capabilities_manifest or runtime_capabilities()
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
            "runtime_capabilities": runtime_caps,
            "runtime_capabilities_hash": runtime_capabilities_hash(runtime_caps),
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


def agentd_runtime_status(
    request_agentd: Callable[[str, str, Any | None], Any],
    *,
    update_enabled: bool = False,
    restart_enabled: bool = False,
    connector: dict[str, Any] | None = None,
) -> dict[str, Any]:
    update: dict[str, Any] = {
        "source": "agentd.ota",
        "available": bool(update_enabled),
        "enabled": False,
        "state": "disabled",
        "detail": "runtime.update is not advertised by this connector",
    }
    restart: dict[str, Any] = {
        "source": "agentd.ota.restart",
        "available": bool(restart_enabled),
        "enabled": False,
        "state": "disabled",
        "detail": "runtime.restart is not advertised by this connector",
        "safe_boundary": "agentd_supervisor_restart_drain",
    }
    raw_status: dict[str, Any] = {}
    if update_enabled or restart_enabled:
        fetched_status = request_agentd("GET", "/api/v1/ota/status", None)
        raw_status = fetched_status if isinstance(fetched_status, dict) else {"ok": True, "value": fetched_status}
    if update_enabled:
        candidate = runtime_update_candidate_from_status(raw_status)
        update = {
            "source": "agentd.ota",
            "available": True,
            "enabled": bool(raw_status.get("enabled", raw_status.get("ok", True))),
            "state": str(raw_status.get("state") or raw_status.get("status") or "unknown"),
            "detail": str(raw_status.get("detail") or raw_status.get("message") or ""),
            "drain_active": bool(raw_status.get("drain_active", False)),
            "drain_reason": str(raw_status.get("drain_reason") or ""),
            "raw": raw_status,
        }
        if candidate:
            update["candidate"] = candidate
        for key in (
            "drain_until_unix_ms",
            "jobs_running",
            "jobs_queued",
            "workflow_tasks_running",
            "workflow_tasks_queued",
            "workflows_running",
        ):
            value = raw_status.get(key)
            if isinstance(value, (int, float)):
                update[key] = int(value)
    if restart_enabled:
        restart = runtime_restart_status_from_ota_status(raw_status)
    result = {
        "ok": True,
        "runtime_kind": RUNTIME_KIND,
        "update": update,
        "restart": restart,
    }
    if connector:
        result["connector"] = connector
    return result


def runtime_restart_status_from_ota_status(raw_status: dict[str, Any]) -> dict[str, Any]:
    source = first_dict_from_any(raw_status, ("restart", "runtime_restart"))
    restart: dict[str, Any] = {
        "source": "agentd.ota.restart",
        "available": bool(source.get("available", bool(raw_status.get("ok", True)))),
        "enabled": bool(source.get("enabled", False)),
        "state": str(source.get("state") or "disabled"),
        "detail": str(source.get("detail") or ""),
        "safe_boundary": str(source.get("safe_boundary") or "agentd_supervisor_restart_drain"),
        "raw": source or raw_status,
    }
    for key in ("method", "service"):
        value = first_string_from_any(source, (key,))
        if value:
            restart[key] = value
    for key in ("dry_run",):
        if key in source:
            restart[key] = bool(source.get(key))
    for key in ("drain_timeout_ms", "updated_unix_ms"):
        value = source.get(key)
        if isinstance(value, (int, float)):
            restart[key] = int(value)
    return restart


def runtime_update_candidate_from_status(raw_status: dict[str, Any]) -> dict[str, Any]:
    source = first_dict_from_any(raw_status, ("candidate", "update_candidate", "release", "artifact"))
    if not source:
        source = raw_status
    url = first_string_from_any(source, ("url", "artifact_url", "download_url"))
    sha256 = first_string_from_any(source, ("sha256", "artifact_sha256", "target_sha256"))
    version = first_string_from_any(source, ("version", "target_version", "name"))
    channel = first_string_from_any(source, ("channel",))
    target_os = first_string_from_any(source, ("target_os", "os"))
    target_arch = first_string_from_any(source, ("target_arch", "arch"))
    reason = first_string_from_any(source, ("reason",)) or "broker operator requested runtime.update"
    drain_timeout_ms = source.get("drain_timeout_ms", raw_status.get("drain_timeout_ms"))
    candidate: dict[str, Any] = {}
    for key, value in (
        ("url", url),
        ("sha256", sha256),
        ("version", version),
        ("channel", channel),
        ("target_os", target_os),
        ("target_arch", target_arch),
        ("reason", reason),
    ):
        if value:
            candidate[key] = value
    if isinstance(drain_timeout_ms, (int, float)) or (isinstance(drain_timeout_ms, str) and drain_timeout_ms.strip()):
        try:
            parsed_timeout = int(drain_timeout_ms)
            if parsed_timeout >= 0:
                candidate["drain_timeout_ms"] = parsed_timeout
        except Exception:
            pass
    if not any(candidate.get(key) for key in ("url", "sha256", "version")):
        return {}
    candidate["input"] = {key: value for key, value in candidate.items() if key != "input"}
    return candidate


def first_dict_from_any(obj: Any, keys: tuple[str, ...]) -> dict[str, Any]:
    if not isinstance(obj, dict):
        return {}
    for key in keys:
        value = obj.get(key)
        if isinstance(value, dict):
            return value
    return {}


def first_string_from_any(obj: Any, keys: tuple[str, ...]) -> str:
    if not isinstance(obj, dict):
        return ""
    for key in keys:
        value = obj.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def base64_body_digest(body: bytes) -> str:
    return base64.b64encode(hashlib.sha256(body).digest()).decode("ascii")


def _query(params: dict[str, Any]) -> str:
    filtered: dict[str, str] = {}
    for key, value in params.items():
        if value is None:
            continue
        text = str(value).strip()
        if text:
            filtered[key] = text
    return urllib.parse.urlencode(filtered)


def _clamp_int(value: Any, default: int, minimum: int, maximum: int) -> int:
    try:
        parsed = int(str(value))
    except Exception:
        parsed = default
    return max(minimum, min(maximum, parsed))


def _unix_ms_to_iso(ms: Any) -> str:
    try:
        parsed = int(ms)
    except Exception:
        return ""
    if parsed <= 0:
        return ""
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(parsed / 1000.0))


def _first_string(obj: dict[str, Any], keys: tuple[str, ...]) -> str:
    for key in keys:
        value = obj.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def _workflow_objective(workflow: dict[str, Any]) -> str:
    spec = workflow.get("spec")
    if isinstance(spec, dict):
        for key in ("title", "name", "objective", "description"):
            value = spec.get(key)
            if isinstance(value, str) and value.strip():
                return value.strip()
    return _first_string(workflow, ("workflow_id", "trace_id"))


def _workflow_session(workflow: dict[str, Any]) -> dict[str, Any]:
    workflow_id = _first_string(workflow, ("workflow_id",))
    status = _first_string(workflow, ("status",)) or "unknown"
    working = status in {"queued", "running", "waiting", "ready"}
    updated_ms = workflow.get("updated_unix_ms") or workflow.get("created_unix_ms")
    progress = status
    if workflow.get("cancel_requested") is True:
        progress = f"{status}, cancel requested"
    return {
        "session_id": workflow_id,
        "thread_id": _first_string(workflow, ("trace_id",)),
        "state": status,
        "objective": _workflow_objective(workflow),
        "working": working,
        "active_turn_id": workflow_id if working else "",
        "started_turn_count": 1 if working or status in {"done", "error", "cancelled"} else 0,
        "completed_turn_count": 0 if working else (1 if status in {"done", "error", "cancelled"} else 0),
        "transcript_length": 0,
        "progress": {
            "label": progress,
        },
        "source": "agentd.workflow",
        "updated_at": _unix_ms_to_iso(updated_ms),
    }


def _daemon_session(row: dict[str, Any]) -> dict[str, Any]:
    session_id = _first_string(row, ("session_id",))
    return {
        "session_id": session_id,
        "state": "idle",
        "objective": session_id,
        "working": False,
        "transcript_length": 0,
        "progress": {
            "label": "daemon session",
        },
        "source": "agentd.session",
        "updated_at": _unix_ms_to_iso(row.get("updated_unix_ms") or row.get("created_unix_ms")),
    }


def agentd_runtime_sessions(agentd_request: Callable[[str, str, Any | None], Any], *, limit: int = 50) -> dict[str, Any]:
    """Project agentd daemon sessions/workflows into codexw broker runtime sessions."""
    limit = _clamp_int(limit, 50, 1, 200)
    sessions: list[dict[str, Any]] = []
    seen: set[str] = set()

    try:
        response = agentd_request("GET", f"/api/v1/db/sessions?{_query({'limit': limit})}", None)
        for row in response.get("sessions", []) if isinstance(response, dict) else []:
            if not isinstance(row, dict):
                continue
            session = _daemon_session(row)
            sid = session.get("session_id")
            if isinstance(sid, str) and sid and sid not in seen:
                sessions.append(session)
                seen.add(sid)
    except Exception:
        pass

    try:
        response = agentd_request("GET", f"/api/v1/db/workflows?{_query({'limit': limit, 'include_spec': 1})}", None)
        for row in response.get("workflows", []) if isinstance(response, dict) else []:
            if not isinstance(row, dict):
                continue
            session = _workflow_session(row)
            sid = session.get("session_id")
            if isinstance(sid, str) and sid and sid not in seen:
                sessions.append(session)
                seen.add(sid)
    except Exception:
        pass

    return {
        "ok": True,
        "runtime_kind": RUNTIME_KIND,
        "sessions": sessions[:limit],
    }


def agentd_runtime_session_create(
    agentd_request: Callable[[str, str, Any | None], Any],
    body: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Create an agentd daemon session and return a broker-neutral session row."""
    body = body or {}
    request: dict[str, Any] = {}
    for key in ("session_id", "create_files"):
        if key in body:
            request[key] = body[key]
    response = agentd_request("POST", "/api/v1/session/new", request)
    if not isinstance(response, dict):
        response = {"ok": True, "value": response}
    session_id = _first_string(response, ("session_id",)) or _first_string(body, ("session_id",))
    state = "created" if bool(response.get("created", True)) else "idle"
    objective = _first_string(body, ("objective", "prompt", "title", "name")) or session_id
    session = {
        "session_id": session_id,
        "state": state,
        "objective": objective,
        "working": False,
        "transcript_length": 0,
        "progress": {
            "label": "daemon session created" if state == "created" else "daemon session",
        },
        "source": "agentd.session",
    }
    return {
        "ok": True,
        "runtime_kind": RUNTIME_KIND,
        "session": session,
        "result": response,
    }


def _event_seq(ts_unix_ms: Any, local_id: Any) -> int:
    try:
        ts = int(ts_unix_ms)
    except Exception:
        ts = int(time.time() * 1000)
    try:
        lid = int(local_id)
    except Exception:
        lid = 0
    return max(1, ts * 1000 + (lid % 1000))


def _decode_data_json(row: dict[str, Any]) -> dict[str, Any]:
    data = row.get("data")
    if isinstance(data, dict):
        return data
    data_json = row.get("data_json")
    if isinstance(data_json, str) and data_json.strip():
        try:
            parsed = json.loads(data_json)
            if isinstance(parsed, dict):
                return parsed
        except Exception:
            return {"raw": data_json}
    return {}


def _client_event(session_id: str, row: dict[str, Any]) -> dict[str, Any]:
    local_id = row.get("id") or 0
    seq = _event_seq(row.get("ts_unix_ms"), local_id)
    event_name = _first_string(row, ("type",)) or "client.event"
    return {
        "session_id": session_id,
        "runtime_event_id": f"client:{session_id}:{local_id}",
        "seq": seq,
        "event": event_name,
        "data": _decode_data_json(row),
        "raw": row,
    }


def _workflow_event(row: dict[str, Any]) -> dict[str, Any]:
    local_id = row.get("event_id") or 0
    workflow_id = _first_string(row, ("workflow_id",))
    seq = _event_seq(row.get("ts_unix_ms"), local_id)
    return {
        "session_id": workflow_id,
        "runtime_event_id": f"workflow:{workflow_id}:{local_id}",
        "seq": seq,
        "event": _first_string(row, ("type",)) or "workflow.event",
        "data": _decode_data_json(row),
        "raw": row,
    }


def agentd_runtime_events(
    agentd_request: Callable[[str, str, Any | None], Any],
    query: dict[str, list[str]] | None = None,
) -> dict[str, Any]:
    """Return a bounded normalized replay over agentd client/workflow events."""
    query = query or {}
    limit = _clamp_int((query.get("limit") or ["8"])[0], 8, 1, 100)
    after_id_raw = (query.get("after_id") or query.get("last_event_id") or [None])[0]
    after_id = _clamp_int(after_id_raw, 0, 0, 2**63 - 1) if after_id_raw is not None else 0
    session_id = str((query.get("session_id") or [""])[0]).strip()
    event_prefix = str((query.get("event_prefix") or [""])[0]).strip()

    events: list[dict[str, Any]] = []

    def add_client_events(sid: str) -> None:
        if not sid:
            return
        response = agentd_request("GET", f"/api/v1/db/client_events?{_query({'session_id': sid, 'limit': limit})}", None)
        for row in response.get("client_events", []) if isinstance(response, dict) else []:
            if isinstance(row, dict):
                events.append(_client_event(sid, row))

    def add_workflow_events(workflow_id: str) -> None:
        if not workflow_id:
            return
        response = agentd_request("GET", f"/api/v1/db/workflow_events?{_query({'workflow_id': workflow_id, 'limit': limit})}", None)
        for row in response.get("events", []) if isinstance(response, dict) else []:
            if isinstance(row, dict):
                events.append(_workflow_event(row))

    if session_id:
        add_client_events(session_id)
        add_workflow_events(session_id)
    else:
        try:
            response = agentd_request("GET", f"/api/v1/db/sessions?{_query({'limit': min(limit, 10)})}", None)
            for row in response.get("sessions", []) if isinstance(response, dict) else []:
                if isinstance(row, dict):
                    add_client_events(_first_string(row, ("session_id",)))
        except Exception:
            pass
        try:
            response = agentd_request("GET", f"/api/v1/db/workflows?{_query({'limit': min(limit, 10)})}", None)
            for row in response.get("workflows", []) if isinstance(response, dict) else []:
                if isinstance(row, dict):
                    add_workflow_events(_first_string(row, ("workflow_id",)))
        except Exception:
            pass

    if event_prefix:
        events = [ev for ev in events if str(ev.get("event", "")).startswith(event_prefix)]
    if after_id > 0:
        events = [ev for ev in events if int(ev.get("seq") or 0) > after_id]

    events.sort(key=lambda ev: int(ev.get("seq") or 0))
    events = events[-limit:]
    latest = max((int(ev.get("seq") or 0) for ev in events), default=0)
    return {
        "ok": True,
        "runtime_kind": RUNTIME_KIND,
        "session_id": session_id,
        "after_id": after_id if after_id > 0 else None,
        "limit": limit,
        "returned": len(events),
        "latest_event_id": latest if latest > 0 else None,
        "events": events,
    }
