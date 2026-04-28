#!/usr/bin/env python3
"""Expose an agentd daemon through codexw's external local API contract."""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import platform
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from agentd_codexw_contract import (
    LOCAL_API_VERSION,
    RUNTIME_ACTIONS,
    legacy_capabilities,
    runtime_capabilities,
)


class FacadeState:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.session_id = args.session_id
        self.thread_id = args.thread_id
        self.instance_id = f"agentd-facade-{uuid.uuid4().hex[:12]}"
        self.process_started_at_ms = int(time.time() * 1000)
        self.transcript: list[dict[str, Any]] = []
        self.lock = threading.Lock()

    def append_message(self, role: str, text: str, *, status: str = "completed") -> int:
        with self.lock:
            index = len(self.transcript)
            self.transcript.append(
                {
                    "role": role,
                    "text": text,
                    "kind": "message",
                    "status": status,
                    "created_unix_ms": int(time.time() * 1000),
                }
            )
            return index

    def transcript_page(self, *, before: int | None, limit: int) -> dict[str, Any]:
        limit = max(1, min(limit, 500))
        with self.lock:
            total = len(self.transcript)
            end = total if before is None else max(0, min(before, total))
            start = max(0, end - limit)
            page = list(self.transcript[start:end])
        return {
            "local_api_version": LOCAL_API_VERSION,
            "session_id": self.session_id,
            "total_entries": total,
            "range_start": start,
            "range_end": end,
            "has_older": start > 0,
            "next_before": start if start > 0 else None,
            "transcript": page,
        }


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def json_dumps(obj: Any) -> bytes:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def read_json_body(handler: BaseHTTPRequestHandler) -> dict[str, Any]:
    length = int(handler.headers.get("content-length") or "0")
    if length <= 0:
        return {}
    raw = handler.rfile.read(length)
    try:
        obj = json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON body: {exc}") from exc
    if not isinstance(obj, dict):
        raise ValueError("JSON body must be an object")
    return obj


def bearer_token(headers: Any) -> str:
    auth = headers.get("authorization") or headers.get("Authorization") or ""
    prefix = "Bearer "
    if not auth.startswith(prefix):
        return ""
    return auth[len(prefix) :].strip()


def extract_prompt(body: dict[str, Any]) -> str:
    for key in ("prompt", "text", "message"):
        value = body.get(key)
        if isinstance(value, str) and value.strip():
            return value
    input_obj = body.get("input")
    if isinstance(input_obj, dict):
        text = input_obj.get("text")
        if isinstance(text, str) and text.strip():
            return text
        items = input_obj.get("items")
        if isinstance(items, list):
            parts = []
            for item in items:
                if isinstance(item, dict):
                    text = item.get("text")
                    if isinstance(text, str) and text.strip():
                        parts.append(text.strip())
            if parts:
                return "\n".join(parts)
    return ""


def request_input(body: dict[str, Any]) -> dict[str, Any]:
    for key in ("input", "params", "payload"):
        value = body.get(key)
        if isinstance(value, dict):
            return value
    return {}


def require_string(obj: dict[str, Any], key: str) -> str:
    value = obj.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{key} is required")
    return value.strip()


def query_from_input(input_obj: dict[str, Any], allowed: set[str]) -> str:
    values: dict[str, str] = {}
    for key in allowed:
        value = input_obj.get(key)
        if value is None:
            continue
        if isinstance(value, (str, int, float, bool)):
            text = str(value).strip()
            if text:
                values[key] = text
    return urllib.parse.urlencode(values)


def make_session(state: FacadeState) -> dict[str, Any]:
    return {
        "id": state.session_id,
        "scope": "process",
        "title": "agentd broker session",
        "status": "active",
        "attached_thread_id": state.thread_id,
        "attachment": {
            "id": f"attach:{state.session_id}",
            "client_id": "client_mobile",
            "lease_seconds": 300,
            "attached_thread_id": state.thread_id,
        },
    }


class AgentdClient:
    def __init__(self, base_url: str, token: str, timeout: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(self, method: str, path: str, body: dict[str, Any] | None = None) -> Any:
        data = None if body is None else json_dumps(body)
        headers = {"Accept": "application/json"}
        if body is not None:
            headers["Content-Type"] = "application/json"
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        req = urllib.request.Request(
            f"{self.base_url}{path}",
            data=data,
            headers=headers,
            method=method,
        )
        with self.opener.open(req, timeout=self.timeout) as resp:
            raw = resp.read()
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))


class FacadeHandler(BaseHTTPRequestHandler):
    server_version = "agentd-codexw-local-api-facade/1"

    @property
    def state(self) -> FacadeState:
        return self.server.state  # type: ignore[attr-defined]

    @property
    def agentd(self) -> AgentdClient:
        return self.server.agentd  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args: Any) -> None:
        if self.state.args.quiet:
            return
        super().log_message(fmt, *args)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_common_headers()
        self.end_headers()

    def do_GET(self) -> None:
        self.route("GET")

    def do_POST(self) -> None:
        self.route("POST")

    def route(self, method: str) -> None:
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)
        if not self.authorized():
            self.send_json(401, {"error": {"code": "unauthorized", "message": "invalid bearer token"}})
            return
        try:
            if method == "GET" and path == "/healthz":
                self.send_json(200, self.health())
            elif method == "GET" and path == "/api/v1/runtime":
                self.send_json(200, self.runtime())
            elif method == "POST" and path == "/api/v1/runtime/actions":
                self.send_json(200, self.runtime_action(read_json_body(self)))
            elif method == "GET" and path in ("/api/v1/session", f"/api/v1/session/{self.state.session_id}"):
                self.send_json(200, self.session_snapshot())
            elif method == "POST" and path in ("/api/v1/session/new", "/api/v1/session/attach"):
                self.send_json(200, self.session_operation(path, read_json_body(self)))
            elif method == "POST" and path == "/api/v1/turn/start":
                body = read_json_body(self)
                self.send_json(200, self.turn_start(body.get("session_id") or self.state.session_id, body))
            elif method == "POST" and path == f"/api/v1/session/{self.state.session_id}/turn/start":
                self.send_json(200, self.turn_start(self.state.session_id, read_json_body(self)))
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/transcript":
                limit = int((query.get("limit") or ["100"])[0])
                before_raw = (query.get("before") or [None])[0]
                before = int(before_raw) if before_raw is not None else None
                self.send_json(200, self.state.transcript_page(before=before, limit=limit))
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/orchestration/status":
                self.send_json(200, self.orchestration_status())
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/orchestration/dependencies":
                self.send_json(200, {"ok": True, "session_id": self.state.session_id, "dependencies": []})
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/orchestration/workers":
                self.send_json(200, {"ok": True, "session_id": self.state.session_id, "workers": []})
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/collaboration/capability-report":
                self.send_json(200, self.capability_report())
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/capabilities":
                self.send_json(200, self.capabilities())
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/services":
                self.send_json(200, {"ok": True, "session_id": self.state.session_id, "services": []})
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/shells":
                self.send_json(200, {"ok": True, "session_id": self.state.session_id, "shells": []})
            elif method == "GET" and path.startswith(f"/api/v1/session/{self.state.session_id}/shells/"):
                self.send_json(404, {"error": {"code": "shell_not_found", "message": "no facade shell is active"}})
            elif method == "POST" and path.startswith(f"/api/v1/session/{self.state.session_id}/shells/"):
                self.send_json(501, {"error": {"code": "shell_control_unimplemented", "message": "agentd facade does not own shell sessions yet"}})
            elif method == "GET" and path == f"/api/v1/session/{self.state.session_id}/files/read":
                self.file_read(query)
            else:
                self.send_json(404, {"error": {"code": "not_found", "message": path}})
        except ValueError as exc:
            self.send_json(400, {"error": {"code": "bad_request", "message": str(exc)}})
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            self.send_json(502, {"error": {"code": "agentd_http_error", "status": exc.code, "detail": detail}})
        except Exception as exc:  # noqa: BLE001
            self.send_json(500, {"error": {"code": "facade_error", "message": str(exc)}})

    def authorized(self) -> bool:
        token = self.state.args.token
        return not token or bearer_token(self.headers) == token

    def health(self) -> dict[str, Any]:
        agentd_health = self.try_agentd("GET", "/api/v1/health")
        return {
            "ok": True,
            "service": "agentd-codexw-local-api-facade",
            "local_api_version": LOCAL_API_VERSION,
            "ts": now_iso(),
            "agentd": agentd_health,
        }

    def runtime(self) -> dict[str, Any]:
        agentd_health = self.try_agentd("GET", "/api/v1/health")
        agentd_caps = self.try_agentd("GET", "/api/v1/caps")
        return {
            "ok": True,
            "local_api_version": LOCAL_API_VERSION,
            "session_id": self.state.session_id,
            "runtime": {
                "kind": "agentd",
                "instance_id": self.state.instance_id,
                "process_started_at_ms": self.state.process_started_at_ms,
                "suggested_deployment_id": self.state.args.deployment_id,
                "connection_mode": "service",
                "version": LOCAL_API_VERSION,
                "host_os": platform.system().lower(),
                "host_arch": platform.machine(),
                "preferred_broker_transport": "connector",
                "recommended_remote_clients": ["ios"],
                "agentd_base_url": self.state.args.agentd_base_url.rstrip("/"),
                "agentd_health": agentd_health,
                "agentd_capabilities": agentd_caps,
                "capabilities": legacy_capabilities(),
                "runtime_capabilities": runtime_capabilities(),
                "live_session": {
                    "status": "available",
                    "transport": "broker_signaled_webrtc",
                    "implementation": "external_local_api_facade",
                    "session_mode": "shell_file_control",
                    "transport_ready": True,
                    "requires_broker_auth": True,
                    "requires_peer_mutual_auth": True,
                },
            },
        }

    def runtime_action(self, body: dict[str, Any]) -> dict[str, Any]:
        action = body.get("action")
        if not isinstance(action, str) or not action.strip():
            raise ValueError("runtime action is required")
        action = action.strip()
        if action not in RUNTIME_ACTIONS:
            raise ValueError(f"unsupported runtime action: {action}")
        input_obj = request_input(body)
        result = self.forward_runtime_action(action, input_obj)
        return {
            "ok": True,
            "local_api_version": LOCAL_API_VERSION,
            "runtime_kind": "agentd",
            "instance_id": self.state.instance_id,
            "action": action,
            "result": result,
        }

    def forward_runtime_action(self, action: str, input_obj: dict[str, Any]) -> Any:
        if action == "workflow.submit":
            return self.agentd.request("POST", "/api/v1/workflow/submit", input_obj)
        if action == "workflow.read":
            workflow_id = require_string(input_obj, "workflow_id")
            return self.agentd.request("GET", "/api/v1/workflow?" + urllib.parse.urlencode({"workflow_id": workflow_id}))
        if action == "workflow.cancel":
            workflow_id = require_string(input_obj, "workflow_id")
            return self.agentd.request("POST", "/api/v1/workflow/cancel", {"workflow_id": workflow_id})
        if action == "schedule.list":
            query = query_from_input(input_obj, {"status", "limit", "offset"})
            path = "/api/v1/workflow_schedules" + (f"?{query}" if query else "")
            return self.agentd.request("GET", path)
        if action in ("experience.list", "experience.export"):
            query = query_from_input(
                input_obj,
                {"offset", "limit", "label", "workflow_id", "task_id", "min_reward", "max_reward"},
            )
            path = "/api/v1/rl/experience_records" + (f"?{query}" if query else "")
            result = self.agentd.request("GET", path)
            if action == "experience.export" and isinstance(result, dict):
                return {**result, "export_format": "json"}
            return result
        raise ValueError(f"unsupported runtime action: {action}")

    def session_snapshot(self) -> dict[str, Any]:
        return {
            "ok": True,
            "local_api_version": LOCAL_API_VERSION,
            "session_id": self.state.session_id,
            "thread_id": self.state.thread_id,
            "session": make_session(self.state),
            "working": False,
            "process_scoped": True,
        }

    def session_operation(self, path: str, body: dict[str, Any]) -> dict[str, Any]:
        if isinstance(body.get("thread_id"), str) and body["thread_id"].strip():
            self.state.thread_id = body["thread_id"].strip()
        op = "session.new" if path.endswith("/new") else "session.attach"
        return {
            **self.session_snapshot(),
            "attachment": make_session(self.state)["attachment"],
            "operation": {
                "kind": op,
                "target_thread_id": self.state.thread_id,
                "requested_client_id": body.get("client_id"),
                "requested_lease_seconds": body.get("lease_seconds"),
            },
            "requested_action": "start_thread" if op == "session.new" else "attach_thread",
        }

    def turn_start(self, session_id: str, body: dict[str, Any]) -> dict[str, Any]:
        if session_id != self.state.session_id:
            raise ValueError(f"unknown session_id: {session_id}")
        prompt = extract_prompt(body)
        if not prompt:
            raise ValueError("turn/start requires input.text or prompt")
        turn_id = f"turn_{uuid.uuid4().hex[:12]}"
        self.state.append_message("user", prompt, status="completed")
        if self.state.args.turn_mode == "echo":
            assistant_text = f"agentd facade echo: {prompt}"
            ok = True
            agentd_result: Any = {"ok": True, "assistant_text": assistant_text, "mode": "echo"}
        else:
            agentd_result = self.agentd.request(
                "POST",
                "/api/v1/run",
                {"prompt": prompt, "no_session": True, "tools": self.state.args.agentd_tools},
            )
            ok = bool(agentd_result.get("ok", False)) if isinstance(agentd_result, dict) else True
            assistant_text = assistant_text_from_agentd(agentd_result)
        self.state.append_message("assistant", assistant_text, status="completed" if ok else "error")
        return {
            "ok": ok,
            "accepted": True,
            "queued": False,
            "session": make_session(self.state),
            "operation": {"kind": "turn.start", "turn_id": turn_id},
            "turn": {"id": turn_id, "status": "completed" if ok else "error"},
            "agentd": agentd_result,
        }

    def orchestration_status(self) -> dict[str, Any]:
        return {
            "ok": True,
            "session_id": self.state.session_id,
            "orchestration": {
                "status": "idle",
                "runtime": "agentd",
                "facade": "codexw_external_local_api",
            },
        }

    def capabilities(self) -> dict[str, Any]:
        return {
            "ok": True,
            "session_id": self.state.session_id,
            "capabilities": legacy_capabilities(),
            "runtime_capabilities": runtime_capabilities(),
        }

    def capability_report(self) -> dict[str, Any]:
        return {
            "ok": True,
            "session_id": self.state.session_id,
            "report": {
                "kind": "agentd_codexw_facade_capability_report_v1",
                "turn_mode": self.state.args.turn_mode,
                "capabilities": self.capabilities()["capabilities"],
            },
        }

    def file_read(self, query: dict[str, list[str]]) -> None:
        path = (query.get("path") or [""])[0]
        if not path:
            self.send_json(400, {"error": {"code": "missing_path", "message": "path is required"}})
            return
        root = Path(self.state.args.file_root).resolve()
        target = (root / path.lstrip("/")).resolve()
        if root not in target.parents and target != root:
            self.send_json(403, {"error": {"code": "path_not_allowed", "message": str(target)}})
            return
        if not target.is_file():
            self.send_json(404, {"error": {"code": "file_not_found", "message": path}})
            return
        offset = int((query.get("offset") or ["0"])[0])
        limit = max(1, min(int((query.get("limit") or ["65536"])[0]), 512 * 1024))
        size = target.stat().st_size
        with target.open("rb") as fh:
            fh.seek(offset)
            chunk = fh.read(limit)
        content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        self.send_json(
            200,
            {
                "ok": True,
                "path": str(target.relative_to(root)),
                "offset": offset,
                "size_bytes": size,
                "data_base64": base64.b64encode(chunk).decode("ascii"),
                "has_more": offset + len(chunk) < size,
                "content_type": content_type,
            },
        )

    def try_agentd(self, method: str, path: str) -> Any:
        try:
            return self.agentd.request(method, path)
        except Exception as exc:  # noqa: BLE001
            return {"ok": False, "error": str(exc)}

    def send_common_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "authorization,content-type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("X-Codexw-Local-Api-Version", LOCAL_API_VERSION)

    def send_json(self, status: int, obj: Any) -> None:
        data = json_dumps(obj)
        self.send_response(status)
        self.send_common_headers()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def assistant_text_from_agentd(obj: Any) -> str:
    if not isinstance(obj, dict):
        return str(obj)
    for key in ("assistant_text", "text", "output", "response"):
        value = obj.get(key)
        if isinstance(value, str) and value.strip():
            return value
    result = obj.get("result")
    if isinstance(result, dict):
        for key in ("assistant_text", "text", "output"):
            value = result.get(key)
            if isinstance(value, str) and value.strip():
                return value
    if not obj.get("ok", True):
        return json.dumps(obj.get("error") or obj, ensure_ascii=False)
    return json.dumps(obj, ensure_ascii=False)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8124)
    parser.add_argument("--token", default=os.environ.get("CODEXW_LOCAL_API_TOKEN", ""))
    parser.add_argument("--agentd-base-url", required=True)
    parser.add_argument("--agentd-auth-token", default=os.environ.get("AGENTD_AUTH_TOKEN", ""))
    parser.add_argument("--agentd-tools", default="host", choices=("none", "basic", "host"))
    parser.add_argument("--deployment-id", default="agentd-local")
    parser.add_argument("--session-id", default="agentd")
    parser.add_argument("--thread-id", default="agentd-thread")
    parser.add_argument("--file-root", default=os.getcwd())
    parser.add_argument("--turn-mode", choices=("agentd_run", "echo"), default="agentd_run")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    state = FacadeState(args)
    agentd = AgentdClient(args.agentd_base_url, args.agentd_auth_token, args.timeout)
    server = ThreadingHTTPServer((args.host, args.port), FacadeHandler)
    server.state = state  # type: ignore[attr-defined]
    server.agentd = agentd  # type: ignore[attr-defined]
    print(
        json.dumps(
            {
                "ok": True,
                "service": "agentd-codexw-local-api-facade",
                "url": f"http://{args.host}:{args.port}",
                "turn_mode": args.turn_mode,
            },
            separators=(",", ":"),
        ),
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 130
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
