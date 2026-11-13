#!/usr/bin/env python3
"""Durable delay deadlines, cancellation and worker-free waiting; isolated daemon."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

def wait(fn, reason, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = fn()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError(reason)

def main():
    binary = str(Path(sys.argv[1]).resolve())
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def call(path, data=None):
        request = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(),
            headers={"Content-Type": "application/json", "Authorization": "Bearer synthetic-delay"})
        with opener.open(request, timeout=3) as response:
            return json.load(response)
    def detail(wid):
        return call(f"/api/v1/workflow?workflow_id={wid}&include_tasks=1&include_results=1")
    def final(wid):
        return wait(lambda: (r if (r := detail(wid))["workflow"]["status"] in ("done", "error", "cancelled") else None), "workflow did not finish")
    def submit(tasks, **extra):
        return call("/api/v1/workflow/submit", {"tasks": tasks, **extra})["workflow_id"]
    with tempfile.TemporaryDirectory(prefix="agent-durable-delay-") as root:
        db = str(Path(root) / "state.db")
        process = None
        log = open(Path(root) / "daemon.log", "w")
        def start():
            nonlocal process
            process = subprocess.Popen([binary, "--host", "127.0.0.1", "--port", str(port),
                "--state-dir", root, "--db-path", db, "--auth-token", "synthetic-delay",
                "--tools", "none", "--no-yolo", "--workflow-concurrency", "1",
                "--workflow-max-inflight-per-workflow", "1", "--workflow-poll-ms", "10"],
                stdout=log, stderr=log, env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
            def ready():
                try: return call("/api/v1/health")["ok"]
                except Exception: return False
            wait(ready, "daemon did not start")
        def timer(wid, tid="sleep"):
            with sqlite3.connect(db) as connection:
                return connection.execute("SELECT due_unix_ms FROM workflow_delay_timers WHERE workflow_id=? AND task_id=?", (wid, tid)).fetchone()
        try:
            start()
            wid = submit([{"task_id": "sleep", "kind": "delay", "delay_ms": 1800},
                {"task_id": "child", "kind": "delay", "delay_ms": 200, "depends_on": ["sleep"]}])
            due = wait(lambda: timer(wid), "timer not persisted")[0]
            rows = detail(wid)["tasks"]
            assert all(t["status"] == "queued" and t["attempt"] == 0 for t in rows), rows
            assert timer(wid, "child") is None, "dependent timer armed before parent completed"
            quick = submit([{"task_id": "quick", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "ready"}}])
            assert final(quick)["workflow"]["status"] == "done"
            assert detail(wid)["workflow"]["status"] not in ("done", "error", "cancelled"), "delay finished early"
            assert detail(wid)["budget_accounting"]["active_attempts"] == 0, "waiting held resource reservation"
            # Crash while queued, then stay down past the original deadline.
            process.kill()
            process.wait(timeout=10)
            time.sleep(max(0, (due - time.time() * 1000) / 1000) + .1)
            start()
            started = time.monotonic()
            result = final(wid)
            assert time.monotonic() - started < 1.5, "restart replayed entire delay"
            assert timer(wid)[0] == due, "restart moved persisted deadline"
            assert result["workflow"]["status"] == "done", result
            assert all(t["attempt"] == 1 for t in result["tasks"]), result
            assert timer(wid, "child")[0] >= due + 200, "dependency delay did not start after parent"
            wid = submit([{"task_id": "sleep", "kind": "delay", "delay_ms": 600000}])
            wait(lambda: timer(wid), "cancel timer not armed")
            call("/api/v1/workflow/cancel", {"workflow_id": wid})
            assert final(wid)["workflow"]["status"] == "cancelled"
            wid = submit([{"task_id": "sleep", "kind": "delay", "delay_ms": 600000}],
                deadline_unix_ms=int(time.time() * 1000) + 250)
            wait(lambda: timer(wid), "deadline timer not armed")
            assert final(wid)["workflow"]["status"] == "cancelled", "waiting ignored workflow deadline"
            # Idle delay time is not execution-budget usage.
            wid = submit([{"task_id": "sleep", "kind": "delay", "delay_ms": 250}],
                workflow_limits={"max_elapsed_ms_total": 20})
            assert final(wid)["workflow"]["status"] == "done", "idle wait consumed execution budget"
            print("durable delay smoke passed: worker-free, restart, dependencies, cancellation, budget")
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            log.close()

if __name__ == "__main__":
    main()
