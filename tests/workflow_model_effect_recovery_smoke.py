"""Model-selected real host effects survive daemon retries without being repeated.
Uses only a loopback stub provider, isolated daemon state, and synthetic data.
"""
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
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

agentd = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def wait_for(fn, reason, seconds=18):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, KeyError, sqlite3.Error, urllib.error.URLError):
            pass
        time.sleep(.025)
    raise AssertionError(reason)


with tempfile.TemporaryDirectory(prefix="model-effect-recovery-") as tmp:
    root = Path(tmp)
    database = root / "agentd.sqlite"
    log = (root / "daemon.log").open("w+")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    process = None
    counts = {}
    counts_lock = threading.Lock()
    helper = root / "action.py"
    helper.write_text("import pathlib,sys,time\np=pathlib.Path(sys.argv[1])\n"
                      "with p.open('a') as f: f.write('applied\\n'); f.flush()\n"
                      "time.sleep(float(sys.argv[2]))\nprint('receipt:'+p.name)\n")

    class Stub(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            messages = request.get("messages", [])
            case = next(str(m.get("content")) for m in messages if m.get("role") == "user")
            with counts_lock:
                counts[case] = counts.get(case, 0) + 1
                number = counts[case]
            after_tool = any(m.get("role") == "tool" for m in messages)
            fail = (case == "provider_before" and number == 1) or (case == "provider_after" and after_tool)
            if fail:
                body = {"error": {"message": "synthetic unavailable", "type": "server_error"}}
            else:
                message = {"role": "assistant", "content": "completed:" + case}
                if not after_tool:
                    pause = 1.5 if case in ("lost", "cancel", "deadline", "elapsed") else 1 if case == "timeout" else 0
                    arguments = {"argv": [sys.executable, str(helper), str(root / (case + ".count")), str(pause)],
                                 "timeout_ms": 100 if case == "timeout" else 5000}
                    tool = {"id": "action-1", "type": "function",
                            "function": {"name": "proc_exec", "arguments": json.dumps(arguments)}}
                    message = {"role": "assistant", "content": "", "tool_calls": [tool]}
                    if case in ("timeout", "double"):
                        message["tool_calls"].append({**tool, "id": "action-2"})
                body = {"id": "synthetic", "object": "chat.completion", "model": "stub",
                        "choices": [{"index": 0, "message": message,
                                     "finish_reason": "tool_calls" if "tool_calls" in message else "stop"}],
                        "usage": {"prompt_tokens": 3, "completion_tokens": 1, "total_tokens": 4}}
            data = json.dumps(body).encode()
            self.send_response(503 if fail else 200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

    stub = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=stub.serve_forever, daemon=True).start()

    def call(path, payload=None):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(base + path, data=data, headers={
            "Content-Type": "application/json", "Authorization": "Bearer synthetic-model-guard"})
        with opener.open(req, timeout=5) as response:
            return json.load(response)

    def start(extra=()):
        global process
        process = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(port),
            "--tools", "host", "--yolo", "--db-path", str(database), "--state-dir", str(root / "state"),
            "--auth-token", "synthetic-model-guard", "--workflow-concurrency", "1",
            "--workflow-poll-ms", "10", *extra], cwd=root, stdout=log, stderr=log,
            env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
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

    def submit(case, others=True, budget=None, deadline=None, attempts=8):
        request = {"prompt": case, "tools": "host", "no_session": True, "no_default_system": True,
                   "max_steps": 4, "max_tool_calls_total": 2, "max_retries": 0, "timeout_ms": 6000,
                   "base_url": f"http://127.0.0.1:{stub.server_port}/v1", "model": "stub", "api_key": "synthetic"}
        if case == "saved":
            request["max_steps"] = 2
            request["max_tool_calls_total"] = 1
        payload = {"workflow_id": "model_" + case, "allow_inline_api_keys": True, "tasks": [
            {"task_id": "write", "priority": 100, "max_attempts": attempts, "request": request}]}
        if others:
            payload["tasks"] += [
                {"task_id": "independent", "kind": "delay", "delay_ms": 0, "priority": -100},
                {"task_id": "after", "kind": "delay", "delay_ms": 0, "depends_on": ["write"]}]
        if budget:
            payload["workflow_limits"] = budget
        if deadline:
            payload["deadline_unix_ms"] = deadline
        result = call("/api/v1/workflow/submit", payload)
        assert result["ok"], result
        return result["workflow_id"]

    def detail(wid):
        return call("/api/v1/workflow?workflow_id=" + wid + "&include_tasks=1&include_results=1")

    def terminal(wid):
        return wait_for(lambda: (value if (value := detail(wid))["workflow"]["status"] in
                                ("done", "error", "cancelled") else None), "workflow did not settle: " + wid)

    def result(value):
        return value["result"]["results_by_task"]["write"]

    def no_repeat(value, case):
        out = result(value)
        assert count(case) == 1 and out["retryable"] is False, value
        assert out["blocker"]["kind"] == "model_run_outcome_unknown", value
        assert next(t for t in value["tasks"] if t["task_id"] == "write")["attempt"] == 1, value

    try:
        start()
        wid = submit("provider_before")
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("provider_before") == 1, value
        assert next(t for t in value["tasks"] if t["task_id"] == "write")["attempt"] == 2, value

        wid = submit("double")
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("double") == 2, value
        assert result(value)["tool_calls_total"] == 2, value

        # Failed provider after a real host write is not a request to repeat it.
        wid = submit("provider_after")
        value = terminal(wid)
        no_repeat(value, "provider_after")
        tasks = {t["task_id"]: t for t in value["tasks"]}
        assert tasks["independent"]["status"] == "done" and tasks["after"]["status"] != "done", value
        assert counts["provider_after"] == 2, counts
        try:
            call("/api/v1/workflow/reconcile", {"workflow_id": wid, "task_id": "write",
                                               "reconcile_url": "https://example.invalid/pretend"})
        except urllib.error.HTTPError as error:
            assert error.code == 409
        else:
            raise AssertionError("unsupported model-run resolver accepted")

        # Intent exists, SQL admission has not happened: recovery can still run.
        sql("CREATE TRIGGER stop_dispatch BEFORE UPDATE OF state ON workflow_effects WHEN NEW.workflow_id='model_prepared' AND NEW.state='dispatching' BEGIN SELECT RAISE(FAIL,'before dispatch'); END")
        wid = submit("prepared")
        wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='prepared'", (wid,)), "intent absent")
        assert count("prepared") == 0
        stop(kill=True)
        sql("DROP TRIGGER stop_dispatch")
        start()
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("prepared") == 1, value

        # The actual child writes, then the daemon dies before any run receipt.
        wid = submit("lost")
        wait_for(lambda: count("lost") == 1, "model effect did not start")
        before = counts["lost"]
        stop(kill=True)
        time.sleep(1.6)
        start()
        value = terminal(wid)
        assert count("lost") == 1 and counts["lost"] == before, value
        assert result(value)["blocker"]["kind"] == "model_run_outcome_unknown", value
        assert next(t for t in value["tasks"] if t["task_id"] == "independent")["status"] == "done", value

        # Final run receipt precedes task settlement. Recovery uses no provider,
        # tools, approval or fresh grant, even when interrupted grants spent all.
        sql("CREATE TRIGGER stop_task_done BEFORE UPDATE OF status ON workflow_tasks WHEN NEW.workflow_id='model_saved' AND NEW.task_id='write' AND NEW.status='done' BEGIN SELECT RAISE(FAIL,'before completion'); END")
        wid = submit("saved", others=False, budget={"max_tool_calls_total": 1, "max_steps_total": 2}, attempts=1)
        wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='completed'", (wid,)), "run receipt absent")
        before = counts["saved"]
        stop(kill=True)
        sql("DROP TRIGGER stop_task_done")
        start(("--policy-mode", "enforce", "--policy-tool-deny", "proc_exec",
               "--policy-approval-tools", "proc_exec", "--policy-approval-timeout-ms", "100"))
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("saved") == 1 and counts["saved"] == before, value
        out = result(value)
        assert out["effect"]["replayed"] is True and out["assistant_text"] == "completed:saved", value
        assert out["tool_calls_total"] == 0 and out["steps_executed"] == 0 and out["total_tokens"] == 0, value
        assert value["budget_accounting"]["charged"]["max_tool_calls_total"] == 1 and value["budget_accounting"]["charged"]["max_steps_total"] == 2, value
        assert value["budget_accounting"]["uncertain_attempts"] == 1, value
        assert value["effects"][0]["boundary"] == "model_run", value

        # Policy rejection never admits an effect. No permission expansion.
        wid = submit("denied", others=False, attempts=1)
        value = terminal(wid)
        assert count("denied") == 0 and not sql("SELECT 1 FROM workflow_effects WHERE workflow_id=?", (wid,)), value
        assert result(value).get("needs_input") is not True, value
        stop()
        start(("--policy-mode", "enforce", "--policy-approval-tools", "proc_exec", "--policy-approval-timeout-ms", "100"))
        wid = submit("approval", others=False, attempts=1)
        value = terminal(wid)
        assert count("approval") == 0 and not sql("SELECT 1 FROM workflow_effects WHERE workflow_id=?", (wid,)), value
        stop()
        start()

        # A timed out first tool prevents the second call in the same batch.
        wid = submit("timeout")
        value = terminal(wid)
        no_repeat(value, "timeout")
        assert counts["timeout"] == 1 and result(value)["tool_calls_total"] == 1, value

        # A workflow time-grant expiry must not override the no-repeat result.
        wid = submit("elapsed", others=False, budget={"max_elapsed_ms_total": 350})
        value = terminal(wid)
        no_repeat(value, "elapsed")
        assert counts["elapsed"] == 1, value

        for case in ("cancel", "deadline"):
            wid = submit(case, others=False, deadline=int(time.time() * 1000) + 500 if case == "deadline" else None)
            wait_for(lambda: count(case) == 1, case + " action absent")
            if case == "cancel":
                call("/api/v1/workflow/cancel", {"workflow_id": wid})
            value = terminal(wid)
            assert value["workflow"]["cancel_requested"] and count(case) == 1, value
            before = counts[case]
            stop(kill=True)
            start()
            value = terminal(wid)
            assert value["workflow"]["cancel_requested"] and count(case) == 1 and counts[case] == before, value
    except BaseException:
        log.flush()
        log.seek(0)
        print(log.read()[-9000:], file=sys.stderr)
        raise
    finally:
        stop()
        stub.shutdown()
        stub.server_close()
        log.close()
print("Model-selected host effects: provider retry, multi-tool run, crash/receipt recovery, no fresh grants, policy/approval, uncertainty, timeout and owner stops passed")
