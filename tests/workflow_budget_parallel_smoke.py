#!/usr/bin/env python3
"""Actual daemon/model-stub concurrency and budget escrow, without provider keys."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait(predicate, reason, timeout=12):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError(reason)


class Stub(BaseHTTPRequestHandler):
    lock = threading.Lock()
    active = {}
    maximum = {}
    requests = {}
    gate = threading.Event()
    effect_posts = 0
    effect_lookups = 0

    def log_message(self, *_):
        pass

    def do_GET(self):
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        Stub.effect_lookups += 1
        proof = {"schema": "agentd.effect.receipt.v1", "effect_id": query["effect_id"][0], "request_hash": query["request_hash"][0], "attempt": int(query["attempt"][0]), "state": "not_applied", "final": True}
        body = json.dumps(proof).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path == "/effect":
            self.rfile.read(int(self.headers["Content-Length"]))
            Stub.effect_posts += 1
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = request.get("messages", [])
        case = next((str(m.get("content")) for m in messages if m.get("role") == "user"), "unknown")
        with self.lock:
            self.active[case] = self.active.get(case, 0) + 1
            self.maximum[case] = max(self.maximum.get(case, 0), self.active[case])
            self.requests[case] = self.requests.get(case, 0) + 1
        try:
            if case == "elapsed":
                time.sleep(.15)
            if case in ("capacity", "bounded"):
                if not self.gate.wait(timeout=10):
                    raise AssertionError("model gate was never released")
            tool_result = any(m.get("role") == "tool" for m in messages)
            message = {"role": "assistant", "content": "DONE"}
            if case == "one-tool" and not tool_result:
                message = {"role": "assistant", "content": "", "tool_calls": [{"id": "tool-1", "type": "function", "function": {"name": "ext_echo", "arguments": '{"text":"synthetic"}'}}]}
            body = {"id": "stub", "object": "chat.completion", "model": "stub", "choices": [{"index": 0, "message": message, "finish_reason": "tool_calls" if "tool_calls" in message else "stop"}]}
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
        finally:
            with self.lock:
                self.active[case] -= 1


def main():
    binary, plugin = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
    stub = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    thread = threading.Thread(target=stub.serve_forever, daemon=True)
    thread.start()
    daemon_port = port()
    base = f"http://127.0.0.1:{daemon_port}"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def call(path, payload=None):
        request = urllib.request.Request(base + path, data=None if payload is None else json.dumps(payload).encode(), headers={"Content-Type": "application/json", "Authorization": "Bearer synthetic-budget-check"})
        with opener.open(request, timeout=4) as response:
            return json.load(response)

    def detail(workflow):
        return call(f"/api/v1/workflow?workflow_id={workflow}&include_tasks=1&include_results=1")

    def submit(case, steps, per_task, pure=False, tools=False):
        tasks = [{"task_id": name, "request": {"prompt": case, "tools": "host" if tools else "none", "max_steps": per_task, "max_tool_calls_total": 1, "no_session": True, "model": "stub", "base_url": f"http://127.0.0.1:{stub.server_port}/v1", "api_key": "synthetic"}} for name in ("a", "b")]
        if pure:
            tasks.append({"task_id": "independent", "kind": "delay", "delay_ms": 0})
        limits = {"max_tool_calls_total": 1} if tools else {"max_steps_total": steps}
        return call("/api/v1/workflow/submit", {"allow_inline_api_keys": True, "workflow_limits": limits, "tasks": tasks})["workflow_id"]

    def final(workflow):
        return wait(lambda: (result if (result := detail(workflow))["workflow"]["status"] in ("done", "cancelled", "error") else None), "workflow never settled")

    with tempfile.TemporaryDirectory(prefix="agent-budget-parallel-") as data:
        with open(Path(data) / "daemon.log", "w") as log:
            process = subprocess.Popen([binary, "--host", "127.0.0.1", "--port", str(daemon_port), "--state-dir", data, "--db-path", str(Path(data) / "state.db"), "--auth-token", "synthetic-budget-check", "--tools", "host", "--no-yolo", "--tool-plugin", plugin, "--workflow-concurrency", "3", "--workflow-max-inflight-per-workflow", "2", "--workflow-poll-ms", "10", "--workflow-enable-http-tasks", "--workflow-http-allow-host", "127.0.0.1"], stdout=log, stderr=log, env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
            try:
                def ready():
                    try:
                        return call("/api/v1/health")["ok"]
                    except Exception:
                        return False
                wait(ready, "daemon did not start")
                workflow = submit("capacity", 2, 2, pure=True)
                wait(lambda: Stub.active.get("capacity") == 1, "first provider call not admitted")
                def independent_ready():
                    result = detail(workflow)
                    return result if any(t["task_id"] == "independent" and t["status"] == "done" for t in result["tasks"]) else None
                result = wait(independent_ready, "capacity waiting stalled independent work")
                account = result["budget_accounting"]
                assert account["held"]["max_steps_total"] == 2 and result["workflow_remaining"]["max_steps_total"] == 0
                assert Stub.requests["capacity"] == 1, "two requests spent one grant"
                Stub.gate.set()
                result = final(workflow)
                assert result["workflow"]["status"] == "done" and Stub.maximum["capacity"] == 1
                assert all(t["attempt"] == 1 for t in result["tasks"]), "waiting consumed retries"
                assert result["budget_accounting"]["active_attempts"] == 0 and result["workflow_usage"]["steps_total_used"] == 2
                Stub.gate.clear()
                workflow = submit("bounded", 4, 1)
                wait(lambda: Stub.active.get("bounded") == 2, "bounded grants did not permit concurrency")
                result = detail(workflow)
                assert result["budget_accounting"]["held"]["max_steps_total"] == 2 and result["budget_accounting"]["active_attempts"] == 2
                Stub.gate.set()
                assert final(workflow)["workflow"]["status"] == "done" and Stub.maximum["bounded"] == 2
                workflow = submit("one-tool", 0, 3, tools=True)
                result = final(workflow)
                assert result["workflow"]["status"] == "cancelled" and result["workflow_usage"]["tool_calls_total_used"] == 1
                assert Stub.requests["one-tool"] == 2, "more than one model/tool roundtrip admitted"
                assert result["budget_accounting"]["active_attempts"] == 0
                workflow = call("/api/v1/workflow/submit", {"allow_inline_api_keys": True, "workflow_limits": {"max_elapsed_ms_total": 1000}, "tasks": [{"task_id": "slow", "max_attempts": 2, "request": {"prompt": "elapsed", "tools": "none", "timeout_ms": 30, "no_session": True, "base_url": f"http://127.0.0.1:{stub.server_port}/v1", "api_key": "synthetic", "model": "stub"}}, {"task_id": "independent", "kind": "delay", "delay_ms": 0}]})["workflow_id"]
                result = final(workflow)
                assert result["workflow"]["status"] == "error" and not result["workflow"]["cancel_requested"], "attempt timeout became a global stop"
                assert any(task["task_id"] == "independent" and task["status"] == "done" for task in result["tasks"])
                effect_base = f"http://127.0.0.1:{stub.server_port}"
                workflow = call("/api/v1/workflow/submit", {"workflow_limits": {"max_tool_calls_total": 2}, "tasks": [{"task_id": "write", "kind": "http_json", "max_attempts": 3, "http_json": {"url": effect_base + "/effect", "body": {"synthetic": True}, "effect": {"reconcile_url": effect_base + "/receipt"}}}]})["workflow_id"]
                result = final(workflow)
                assert Stub.effect_posts == 1 and Stub.effect_lookups == 1, "receipt lookup plus redispatch overspent the grant"
                assert result["workflow_usage"]["tool_calls_total_used"] == 2 and result["workflow"]["status"] == "error"
                assert result["effects"][0]["state"] == "prepared", "capacity refusal created ambiguous dispatch"
            finally:
                Stub.gate.set()
                process.terminate()
                process.wait(timeout=10)
    stub.shutdown()
    stub.server_close()
    print("workflow parallel reservation smoke passed")


if __name__ == "__main__":
    main()
