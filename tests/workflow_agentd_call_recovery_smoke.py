#!/usr/bin/env python3
"""Actual parent/remote daemons and a bounded fault proxy; no real provider."""
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

agentd = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def wait(check, message, seconds=12):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        try:
            value = check()
            if value:
                return value
        except (OSError, ValueError, KeyError, sqlite3.Error, urllib.error.URLError):
            pass
        time.sleep(.025)
    raise AssertionError(message)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


with tempfile.TemporaryDirectory(prefix="remote-workflow-recovery-") as tmp:
    root = Path(tmp)
    parent_port, remote_port = free_port(), free_port()
    parent_base, remote_base = f"http://127.0.0.1:{parent_port}", f"http://127.0.0.1:{remote_port}"
    parent_db, remote_db = root / "parent.sqlite", root / "remote.sqlite"
    parent, remote_process = None, None
    logs = {name: (root / (name + ".log")).open("w+") for name in ("parent", "remote")}
    lock = threading.Lock()
    posts, reads, submitted, modes, entered, gates = {}, {}, {}, {}, {}, {}
    helper = root / "write.py"
    helper.write_text("import pathlib,sys,time\np=pathlib.Path(sys.argv[1])\n"
                      "with p.open('a') as f: f.write('applied\\n'); f.flush()\n"
                      "time.sleep(float(sys.argv[2]))\nprint('original-receipt')\n")

    def request(base, path, value=None, token="parent-token", status=False):
        data = None if value is None else json.dumps(value).encode()
        req = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json", "Authorization": "Bearer " + token})
        try:
            with opener.open(req, timeout=8) as response:
                body = json.load(response)
                return (response.status, body) if status else body
        except urllib.error.HTTPError as error:
            body = json.load(error)
            if status:
                return error.code, body
            raise AssertionError((error.code, body)) from error

    def remote_request(path, value=None):
        return request(remote_base, path, value, "remote-token", True)

    class Proxy(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def respond(self, code, value):
            data = value if isinstance(value, bytes) else json.dumps(value).encode()
            try:
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_POST(self):
            value = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            case = value["session_id"]
            if self.headers.get("Authorization") != "Bearer remote-token":
                return self.respond(401, {"ok": False})
            with lock:
                posts[case] = posts.get(case, 0) + 1
                submitted[case] = value
            mode = modes.get(case)
            if mode == "missing":
                return self.respond(503, {"ok": False})
            if mode == "late-admission":
                entered[case].set()
                gates[case].wait(8)
            code, body = remote_request("/api/v1/workflow/submit", value)
            if mode in ("lost", "late-admission"):
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                return
            return self.respond(code, body)

        def do_GET(self):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            case = query.get("session_id", [""])[0]
            with lock:
                reads[case] = reads.get(case, 0) + 1
                number = reads[case]
            if self.headers.get("Authorization") != "Bearer remote-token":
                return self.respond(401, {"ok": False})
            mode = modes.get(case)
            if mode == "hold-read" and number >= 2:
                entered[case].set()
                gates[case].wait(8)
            code, body = remote_request(self.path)
            if code == 200 and mode in ("key", "scope", "trace", "state", "changed-id"):
                key = {"key": "idempotency_key", "scope": "session_id", "trace": "trace_id", "state": "status", "changed-id": "workflow_id"}[mode]
                if mode != "changed-id" or number >= 2:
                    body["workflow"][key] = "wrong"
            if mode == "oversized":
                return self.respond(200, b" " * 2048)
            return self.respond(code, body)

    proxy = ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
    threading.Thread(target=proxy.serve_forever, daemon=True).start()

    def start_parent():
        global parent
        parent = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(parent_port), "--db-path", str(parent_db),
            "--state-dir", str(root / "parent-state"), "--auth-token", "parent-token", "--tools", "none",
            "--workflow-enable-http-tasks", "--workflow-concurrency", "1", "--workflow-poll-ms", "10"],
            cwd=root, stdout=logs["parent"], stderr=logs["parent"], env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null", "AGENTC_TEST_REMOTE_TOKEN": "remote-token"})
        wait(lambda: parent.poll() is None and request(parent_base, "/api/v1/health").get("ok"), "parent start")

    def stop_parent(kill=True):
        global parent
        if parent is not None:
            (parent.kill if kill else parent.terminate)()
            parent.wait(timeout=10)
            parent = None

    def sql(statement, params=()):
        with sqlite3.connect(parent_db, timeout=5) as db:
            return db.execute(statement, params).fetchall()

    def row(case):
        values = sql("SELECT state,receipt_json FROM workflow_effects WHERE workflow_id=? AND task_id='P'", ("parent-" + case,))
        return (values[0][0], json.loads(values[0][1])) if values else ("", {})

    def detail(case):
        return request(parent_base, "/api/v1/workflow?workflow_id=parent-" + case + "&include_tasks=1&include_results=1")

    def terminal(case):
        def finished():
            value = detail(case)
            return value if value["workflow"]["status"] in ("done", "error", "cancelled") else None
        return wait(finished, "parent terminal: " + case)

    def count(case):
        path = root / (case + ".count")
        return len(path.read_text().splitlines()) if path.exists() else 0

    def submit(case, mode=None, pause=0, attempts=1, limits=None, timeout=3000, max_bytes=1048576):
        if mode:
            modes[case] = mode
            entered[case], gates[case] = threading.Event(), threading.Event()
        call = {"base_url": f"http://127.0.0.1:{proxy.server_port}", "bearer_env": "AGENTC_TEST_REMOTE_TOKEN",
                "timeout_ms": timeout, "poll_ms": 20, "max_bytes": max_bytes,
                "workflow": {"allow_sessions": True, "session_id": case, "tasks": [{"task_id": "APPLY", "kind": "host_tool", "host_tool": {
                    "name": "proc_exec", "arguments": {"argv": [sys.executable, str(helper), str(root / (case + ".count")), str(pause)], "timeout_ms": 5000}}}]}}
        value = {"workflow_id": "parent-" + case, "tasks": [
            {"task_id": "P", "kind": "agentd_call", "priority": 10, "max_attempts": attempts, "agentd_call": call},
            {"task_id": "INDEPENDENT", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "independent"}}]}
        if limits:
            value["workflow_limits"] = limits
        result = request(parent_base, "/api/v1/workflow/submit", value)
        assert result["ok"], result
        return value

    def assert_done(case):
        value = terminal(case)
        assert value["workflow"]["status"] == "done", value
        assert posts.get(case) == 1 and count(case) == 1, (case, posts, count(case))
        result = value["result"]["results_by_task"]["P"]
        assert result["effect"]["boundary"] == "agentd_call" and row(case)[0] == "completed", result
        return value

    try:
        remote_process = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(remote_port), "--db-path", str(remote_db),
            "--state-dir", str(root / "remote-state"), "--auth-token", "remote-token", "--tools", "host", "--yolo",
            "--workflow-poll-ms", "10"], cwd=root, stdout=logs["remote"], stderr=logs["remote"], env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
        wait(lambda: remote_process.poll() is None and request(remote_base, "/api/v1/health", token="remote-token").get("ok"), "remote start")
        start_parent()
        submit("lost", "lost")
        assert_done("lost")

        submit("late", "late-admission", attempts=4)
        wait(lambda: entered["late"].is_set() and row("late")[0] == "dispatching", "admission precedes forward")
        stop_parent()
        gates["late"].set()
        wait(lambda: count("late") == 1, "late original effect did not run")
        start_parent()
        assert_done("late")

        submit("observed", "hold-read", pause=1.5, attempts=4)
        wait(lambda: entered["observed"].is_set() and row("observed")[1].get("remote_workflow_id"), "remote identity checkpoint")
        stop_parent()
        modes["observed"] = None
        gates["observed"].set()
        start_parent()
        assert_done("observed")

        sql("CREATE TRIGGER hold_parent_settlement BEFORE UPDATE ON workflow_tasks WHEN NEW.workflow_id='parent-saved' AND NEW.task_id='P' AND NEW.status='done' BEGIN SELECT RAISE(ABORT,'fixture settlement'); END")
        submit("saved", attempts=4, limits={"max_tool_calls_total": 64, "max_steps_total": 64})
        wait(lambda: row("saved")[0] == "completed", "terminal receipt before parent task settlement")
        before = dict(reads)
        stop_parent()
        sql("DROP TRIGGER hold_parent_settlement")
        spec = json.loads(sql("SELECT spec_json FROM workflows WHERE workflow_id='parent-saved'")[0][0])
        spec["workflow_limits"] = {"max_tool_calls_total": 1, "max_steps_total": 1}
        sql("UPDATE workflows SET spec_json=? WHERE workflow_id='parent-saved'", (json.dumps(spec),))
        start_parent()
        saved = assert_done("saved")
        assert reads.get("saved") == before.get("saved"), (reads, before)
        assert saved["result"]["results_by_task"]["P"]["effect"]["replayed"] is True, saved

        submit("missing", "missing")
        missing = terminal("missing")
        assert missing["workflow"]["status"] == "error" and posts["missing"] == 1 and reads["missing"] == 3, missing
        assert missing["result"]["results_by_task"]["P"]["blocker"]["kind"] == "remote_workflow_outcome_unknown", missing
        assert next(task for task in missing["tasks"] if task["task_id"] == "INDEPENDENT")["status"] == "done", missing
        code, _ = request(parent_base, "/api/v1/workflow/reconcile", {"workflow_id": "parent-missing", "task_id": "P", "reconcile_url": "http://127.0.0.1:1"}, status=True)
        assert code == 400
        modes["missing"] = None
        assert remote_request("/api/v1/workflow/submit", submitted["missing"])[0] == 200  # late destination acceptance, outside the parent
        request(parent_base, "/api/v1/workflow/reconcile", {"workflow_id": "parent-missing", "task_id": "P"})
        assert_done("missing")

        for mode in ("key", "scope", "trace", "state", "changed-id", "oversized"):
            case = "invalid-" + mode
            submit(case, mode, pause=.3 if mode == "changed-id" else 0, max_bytes=1024 if mode == "oversized" else 1048576)
            value = terminal(case)
            assert value["workflow"]["status"] == "error" and posts[case] == 1, value
            assert value["result"]["results_by_task"]["P"]["needs_input"] is True, value

        submit("cancel", "hold-read", pause=1.5, attempts=4)
        wait(lambda: entered["cancel"].is_set(), "cancel during remote read")
        request(parent_base, "/api/v1/workflow/cancel", {"workflow_id": "parent-cancel"})
        cancelled = terminal("cancel")
        assert cancelled["workflow"]["status"] == "cancelled" and posts["cancel"] == 1, cancelled
        modes["cancel"] = None
        gates["cancel"].set()
        code, _ = request(parent_base, "/api/v1/workflow/reconcile", {"workflow_id": "parent-cancel", "task_id": "P"}, status=True)
        assert code == 409
        print("Remote workflow loss/restart, exact identity, saved-result budgets, scoped blockers, independent progress and owner-stop recovery passed")
    except Exception:
        for name, log in logs.items():
            log.flush()
            print(name + " daemon log:", Path(log.name).read_text()[-5000:], file=sys.stderr)
        raise
    finally:
        for gate in gates.values():
            gate.set()
        stop_parent(False)
        if remote_process is not None:
            remote_process.terminate()
            remote_process.wait(timeout=10)
        proxy.shutdown()
        proxy.server_close()
        for log in logs.values():
            log.close()
