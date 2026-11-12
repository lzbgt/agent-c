"""Exercise real deterministic host effects across daemon crashes, without a provider."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

agentd = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def wait_for(fn, reason, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            result = fn()
            if result:
                return result
        except (OSError, KeyError, sqlite3.Error, urllib.error.URLError):
            pass
        time.sleep(.025)
    raise AssertionError(reason)


with tempfile.TemporaryDirectory(prefix="host-effect-recovery-") as tmp:
    root = Path(tmp)
    database = root / "agentd.sqlite"
    log = (root / "daemon.log").open("w+")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    process = None
    helper = root / "action.py"
    helper.write_text("import pathlib,sys,time\np=pathlib.Path(sys.argv[1])\n"
                      "with p.open('a') as f: f.write('applied\\n'); f.flush()\n"
                      "time.sleep(float(sys.argv[2]))\nprint('receipt:'+p.name)\n"
                      "sys.exit(int(sys.argv[3]))\n")

    def call(path, payload=None):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"})
        with opener.open(req, timeout=5) as response:
            return json.load(response)

    def start():
        global process
        process = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(port),
            "--tools", "host", "--yolo", "--db-path", str(database), "--state-dir", str(root / "state"),
            "--workflow-concurrency", "1"], cwd=root, stdout=log, stderr=log)
        wait_for(lambda: process.poll() is None and call("/api/v1/health").get("ok"), "daemon failed to start")

    def stop(kill=False):
        global process
        if process is not None:
            (process.kill if kill else process.terminate)()
            process.wait(timeout=10)
            process = None

    def sql(text, params=()):
        with sqlite3.connect(database, timeout=5) as db:
            return db.execute(text, params).fetchall()

    def count(case):
        path = root / (case + ".count")
        return len(path.read_text().splitlines()) if path.exists() else 0

    def submit(case, pause=0, code=0, timeout=5000, budget=None):
        payload = {"workflow_id": "host_" + case, "tasks": [
            {"task_id": "write", "kind": "host_tool", "priority": 100, "max_attempts": 10,
             "host_tool": {"name": "proc_exec", "timeout_ms": timeout, "arguments": {
                 "argv": [sys.executable, str(helper), str(root / (case + ".count")), str(pause), str(code)]}}},
            {"task_id": "independent", "kind": "delay", "delay_ms": 0, "priority": -100},
            {"task_id": "after", "kind": "delay", "delay_ms": 0, "depends_on": ["write"]}]}
        if case == "sop":
            actions = payload.pop("tasks")
            for action in actions:
                action.pop("depends_on", None)
            payload["sop"] = {"schema": "agentd.sop.v1", "id": "maintenance", "version": "1",
                              "stages": [{"id": "work", "mode": "sequence", "actions": actions}]}
        if budget:
            payload["workflow_limits"] = budget
        result = call("/api/v1/workflow/submit", payload)
        assert result["ok"], result
        return result["workflow_id"]

    def terminal(wid):
        def poll():
            detail = call("/api/v1/workflow?workflow_id=" + wid + "&include_tasks=1&include_results=1")
            return detail if detail["workflow"]["status"] in ("done", "error", "cancelled") else None
        return wait_for(poll, "workflow did not settle: " + wid)

    def result(detail):
        return detail["result"]["results_by_task"]["write"]

    try:
        start()
        # Intent durable but no dispatch happened: safe recovery executes once.
        sql("CREATE TRIGGER stop_dispatch BEFORE UPDATE OF state ON workflow_effects WHEN NEW.state='dispatching' BEGIN SELECT RAISE(FAIL,'before dispatch'); END")
        wid = submit("prepared")
        wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='prepared'", (wid,)), "intent absent")
        assert count("prepared") == 0
        stop(kill=True)
        sql("DROP TRIGGER stop_dispatch")
        start()
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "done" and count("prepared") == 1, detail

        # Child commits, daemon dies before saving result: do not execute again.
        wid = submit("lost", pause=1.5)
        wait_for(lambda: count("lost") == 1, "host effect not applied")
        stop(kill=True)
        time.sleep(1.6)  # Original bounded fixture child exits independently.
        start()
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "error" and count("lost") == 1, detail
        assert result(detail)["blocker"]["kind"] == "host_tool_outcome_unknown", detail
        tasks = {task["task_id"]: task for task in detail["tasks"]}
        assert tasks["independent"]["status"] == "done" and tasks["after"]["status"] != "done", detail
        try:
            call("/api/v1/workflow/reconcile", {"workflow_id": wid, "task_id": "write",
                                               "reconcile_url": "https://example.invalid/pretend"})
        except urllib.error.HTTPError as error:
            assert error.code == 409
        else:
            raise AssertionError("unsupported host resolver accepted")

        # Saved receipt survives process death before task completion.
        sql("CREATE TRIGGER stop_task_done BEFORE UPDATE OF status ON workflow_tasks WHEN NEW.workflow_id='host_saved' AND NEW.task_id='write' AND NEW.status='done' BEGIN SELECT RAISE(FAIL,'before completion'); END")
        wid = submit("saved")
        wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='completed'", (wid,)), "receipt absent")
        stop(kill=True)
        sql("DROP TRIGGER stop_task_done")
        start()
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "done" and count("saved") == 1, detail
        assert result(detail)["effect"]["replayed"] is True and result(detail)["tool_calls_total"] == 0, detail
        assert "receipt:saved.count" in result(detail)["assistant_text"], detail

        # A nonzero exit can follow partial writes: save failure, never repeat it.
        wid = submit("nonzero", code=4)
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "error" and count("nonzero") == 1, detail
        assert result(detail)["effect"]["state"] == "completed" and result(detail)["retryable"] is False, detail
        assert next(t for t in detail["tasks"] if t["task_id"] == "write")["attempt"] == 1, detail

        wid = submit("timeout", pause=1, timeout=100)
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "error" and count("timeout") == 1, detail
        assert result(detail)["blocker"]["kind"] == "host_tool_outcome_unknown", detail

        # One tool is exactly one admission; pure dependent work needs no tool grant.
        wid = submit("budget", budget={"max_tool_calls_total": 1, "max_steps_total": 5})
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "done" and count("budget") == 1, detail
        assert result(detail)["tool_calls_total"] == 1 and result(detail)["total_tokens"] == 0, detail

        wid = submit("sop")
        detail = terminal(wid)
        assert detail["workflow"]["status"] == "done" and count("sop") == 1, detail

        wid = submit("cancel", pause=1.5)
        wait_for(lambda: count("cancel") == 1, "cancel fixture not started")
        call("/api/v1/workflow/cancel", {"workflow_id": wid})
        detail = terminal(wid)
        assert detail["workflow"]["cancel_requested"] and count("cancel") == 1, detail
        stop(kill=True)
        start()
        detail = terminal(wid)
        assert detail["workflow"]["cancel_requested"] and count("cancel") == 1, detail

        # Reject attempts to assert idempotency or fabricate effect identity.
        try:
            call("/api/v1/workflow/submit", {"workflow_id": "bad_host", "tasks": [
                {"task_id": "bad", "kind": "host_tool", "host_tool": {
                 "name": "proc_exec", "arguments": {}, "effect": {"mode": "idempotent"}}}]})
        except urllib.error.HTTPError as error:
            assert error.code == 400
        else:
            raise AssertionError("unsafe mode accepted")
    except BaseException:
        log.flush()
        log.seek(0)
        print(log.read()[-9000:], file=sys.stderr)
        raise
    finally:
        stop()
        log.close()
print("Host actions: intent/crash/receipt recovery, scoped uncertainty, nonzero exit, timeout, budget and owner stop passed")
