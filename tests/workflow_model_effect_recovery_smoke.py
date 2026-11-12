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
    prefetch_counts = {}
    prefetch_caps = {}
    counts_lock = threading.Lock()
    provider_waiting = threading.Event()
    release_provider = threading.Event()
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
            case = (self.path.split("/")[2] if "/moonshot/" in self.path else
                    "reasoning_resume" if "/deepseek/" in self.path else next(str(m.get("content")) for m in reversed(messages) if m.get("role") == "user"))
            vision = case.startswith("vision_")
            prefetch = vision and not request.get("tools")
            with counts_lock:
                counts[case] = counts.get(case, 0) + 1
                number = counts[case]
                if prefetch:
                    prefetch_counts[case] = prefetch_counts.get(case, 0) + 1
                    prefetch_caps[case] = request.get("max_completion_tokens", request.get("max_tokens"))
            after_tool = any(m.get("role") == "tool" for m in messages)
            missing_reasoning = case == "reasoning_resume" and after_tool and any(
                m.get("reasoning_content") != "synthetic reasoning checkpoint"
                for m in messages if m.get("role") == "assistant" and m.get("tool_calls"))
            missing_vision = vision and not prefetch and case != "vision_fallback" and (
                "description:" + case + ":1" not in json.dumps(messages))
            if after_tool and ((case in ("crash_receipted", "session_crash", "session_changed") and number == 2) or
                               (case in ("vision_crash", "vision_session_changed") and number == 3)):
                provider_waiting.set()
                release_provider.wait(12)
            fail = ((case == "provider_before" and number == 1) or (case in ("provider_after", "vision_persistent") and after_tool) or
                    (case in ("transient_after", "session_transient", "frontier_budget", "batch_resume", "reasoning_resume") and after_tool and number == 2) or missing_reasoning or missing_vision or
                    (case == "vision_fallback" and prefetch) or
                    (case in ("vision_transient", "vision_fallback") and after_tool and number == 3))
            if fail:
                body = {"error": {"message": "synthetic unavailable", "type": "server_error"}}
            else:
                message = {"role": "assistant", "content": ("description:" + case + ":" + str(prefetch_counts[case])) if prefetch else "completed:" + case}
                if prefetch and case == "vision_oversized":
                    message["content"] = "x" * (2 * 1024 * 1024 + 1)
                if not prefetch and case != "vision_direct" and (not after_tool or (case == "frontier_budget" and number == 3)):
                    pause = 1.5 if case in ("lost", "vision_lost", "cancel", "deadline", "elapsed") else 1 if case == "timeout" else 0
                    arguments = {"argv": [sys.executable, str(helper), str(root / (case + ".count")), str(pause)],
                                 "timeout_ms": 100 if case == "timeout" else 5000}
                    tool = {"id": "action-1", "type": "function",
                            "function": {"name": "proc_exec", "arguments": json.dumps(arguments)}}
                    message = {"role": "assistant", "content": "", "tool_calls": [tool]}
                    if case in ("timeout", "double", "batch_resume", "reasoning_resume"):
                        message["tool_calls"].append({**tool, "id": "action-2"})
                    if case == "reasoning_resume":
                        message["reasoning_content"] = "synthetic reasoning checkpoint"
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

    def submit(case, others=True, budget=None, deadline=None, attempts=8, session=None):
        request = {"prompt": case, "tools": "host", "no_session": True, "no_default_system": True,
                   "max_steps": 4, "max_tool_calls_total": 2, "max_retries": 0, "timeout_ms": 6000,
                   "base_url": f"http://127.0.0.1:{stub.server_port}/v1", "model": "stub", "api_key": "synthetic"}
        if session:
            request["no_session"] = False
            request["session_id"] = session
            # Cover pinned host prompts without persisting them on a failed attempt.
            request["no_default_system"] = case != "session_transient"
        if case == "reasoning_resume":
            request["base_url"] = f"http://127.0.0.1:{stub.server_port}/deepseek/v1"
            request["model"] = "deepseek-reasoner"
        if case.startswith("vision_"):
            image = {"name": "synthetic.png", "mime": "image/png",
                     "b64": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="}
            request["prompt"] = "__AGENT_MM_V1__" + json.dumps({"images": [image]}) + "\n" + case
            request["base_url"] = f"http://127.0.0.1:{stub.server_port}/moonshot/{case}/v1"
            request["max_completion_tokens"] = 17
        if case in ("saved", "session_saved"):
            request["max_steps"] = 2
            request["max_tool_calls_total"] = 1
        payload = {"workflow_id": "model_" + case, "allow_inline_api_keys": True, "tasks": [
            {"task_id": "write", "priority": 100, "max_attempts": attempts, "request": request}]}
        if session:
            payload["session_id"] = session
            payload["allow_sessions"] = True
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

    def session_rows(sid):
        return call("/api/v1/session?session_id=" + sid)["messages"]

    def seed_session(case):
        sid = "conversation-" + case
        call("/api/v1/session/new", {"session_id": sid})
        call("/api/v1/session/ui_event", {"session_id": sid, "type": "prior_context",
             "ts_unix_ms": 1700000000000, "data": {"text": "keep original conversation"}, "append_to_session": True})
        return sid, session_rows(sid), sql("SELECT revision FROM session_revisions WHERE session_id=?", (sid,))[0][0]

    def assert_session_commit(case, sid, original, revision):
        rows = session_rows(sid)
        assert all(row in rows for row in original), rows
        assert sum(row["role"] == "user" and row["content"] == case for row in rows) == 1, rows
        assert sum(row["role"] == "assistant" and row["content"] == "completed:" + case for row in rows) == 1, rows
        assert sql("SELECT revision FROM session_revisions WHERE session_id=?", (sid,))[0][0] == revision + 1
        return rows

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

        # Persistent provider failure exhausts attempts, but retains a verified
        # prefix rather than inventing an uncertain host outcome or repeating it.
        wid = submit("provider_after", attempts=2)
        value = terminal(wid)
        out = result(value)
        assert count("provider_after") == 1 and out["effect"]["resume_available"] is True, value
        assert out.get("needs_input") is not True and out["tool_calls_total"] == 0, value
        assert next(t for t in value["tasks"] if t["task_id"] == "write")["attempt"] == 2, value
        tasks = {t["task_id"]: t for t in value["tasks"]}
        assert tasks["independent"]["status"] == "done" and tasks["after"]["status"] != "done", value
        assert counts["provider_after"] == 3, counts
        metadata = call("/api/v1/workflow?workflow_id=" + wid + "&include_results=0")
        assert metadata["effects"][0]["resume_available"] is True, metadata
        assert all("result" not in t for t in metadata["tasks"]), metadata
        assert "records" not in metadata["effects"][0] and "receipt_json" not in metadata["effects"][0], metadata
        try:
            call("/api/v1/workflow/reconcile", {"workflow_id": wid, "task_id": "write",
                                               "reconcile_url": "https://example.invalid/pretend"})
        except urllib.error.HTTPError as error:
            assert error.code == 409
        else:
            raise AssertionError("unsupported model-run resolver accepted")

        # A provider failure after a receipted host write resumes with the exact
        # original transcript. No fresh tool grant is needed or consumed.
        wid = submit("transient_after", others=False, attempts=2,
                     budget={"max_tool_calls_total": 1, "max_steps_total": 3})
        value = terminal(wid)
        out = result(value)
        assert value["workflow"]["status"] == "done" and count("transient_after") == 1, value
        assert counts["transient_after"] == 3 and out["steps_executed"] == 1 and out["tool_calls_total"] == 0, value
        assert out["effect"]["replayed_provider_calls"] == 1 and out["effect"]["replayed_tool_calls"] == 1, value
        assert value["budget_accounting"]["charged"]["max_tool_calls_total"] == 1, value
        assert value["budget_accounting"]["charged"]["max_steps_total"] == 3, value

        # Two tools from one model decision are both restored, not regenerated.
        wid = submit("batch_resume", others=False, attempts=2,
                     budget={"max_tool_calls_total": 2, "max_steps_total": 3})
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("batch_resume") == 2, value
        assert counts["batch_resume"] == 3 and result(value)["effect"]["replayed_tool_calls"] == 2, value

        # The OpenAI-compatible adapter keeps required reasoning state outside
        # the C transcript. The stub rejects a resumed batch without its original
        # reasoning_content, so core-visible messages alone cannot pass this test.
        wid = submit("reasoning_resume", others=False, attempts=2,
                     budget={"max_tool_calls_total": 2, "max_steps_total": 3})
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("reasoning_resume") == 2, value
        assert counts["reasoning_resume"] == 3 and result(value)["effect"]["replayed_tool_calls"] == 2, value

        # The continuation may spend a new provider step, but it cannot enlarge
        # an exhausted host-call grant when the model requests another effect.
        wid = submit("frontier_budget", others=False, attempts=2,
                     budget={"max_tool_calls_total": 1, "max_steps_total": 3})
        value = terminal(wid)
        out = result(value)
        assert value["workflow"]["status"] == "error" and count("frontier_budget") == 1, value
        assert counts["frontier_budget"] == 3 and out["retryable"] is False and out.get("needs_input") is not True, value
        assert out["effect"]["resume_available"] is True and out["steps_executed"] == 1, value
        assert value["budget_accounting"]["charged"]["max_steps_total"] == 3, value

        # Process death while waiting for the next provider response preserves a
        # completed tool receipt. On restart even a new denial/approval policy
        # must not ask to authorize that old effect again.
        wid = submit("crash_receipted", others=False, attempts=3, budget={"max_tool_calls_total": 1})
        wait_for(provider_waiting.is_set, "provider did not reach receipted frontier")
        rows = sql("SELECT receipt_json FROM workflow_effects WHERE workflow_id=?", (wid,))
        checkpoint = json.loads(rows[0][0])
        assert checkpoint["pending"] is None and [r["kind"] for r in checkpoint["records"]] == ["provider", "tool"], rows
        stop(kill=True)
        release_provider.set()
        start(("--policy-mode", "enforce", "--policy-tool-deny", "proc_exec",
               "--policy-approval-tools", "proc_exec", "--policy-approval-timeout-ms", "100"))
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("crash_receipted") == 1, value
        assert counts["crash_receipted"] == 3 and result(value)["effect"]["replayed_tool_calls"] == 1, value
        assert value["budget_accounting"]["charged"]["max_tool_calls_total"] == 1, value
        stop()
        start()

        # Persistent sessions use the same verified prefix. Failed attempts do
        # not append prompts or replies; completion publishes exactly once.
        sid, original, revision = seed_session("session_transient")
        wid = submit("session_transient", others=False, attempts=2, session=sid,
                     budget={"max_tool_calls_total": 1, "max_steps_total": 3})
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("session_transient") == 1, value
        assert counts["session_transient"] == 3 and result(value)["effect"]["session_committed"] is True, value
        assert_session_commit("session_transient", sid, original, revision)
        assert value["budget_accounting"]["charged"]["max_tool_calls_total"] == 1, value

        # Kill before final provider receipt, then resume cached tools under a
        # restrictive current policy. The original conversation is still intact.
        provider_waiting.clear()
        release_provider.clear()
        sid, original, revision = seed_session("session_crash")
        wid = submit("session_crash", others=False, attempts=3, session=sid, budget={"max_tool_calls_total": 1})
        wait_for(provider_waiting.is_set, "session provider did not reach saved frontier")
        assert session_rows(sid) == original
        checkpoint = json.loads(sql("SELECT receipt_json FROM workflow_effects WHERE workflow_id=?", (wid,))[0][0])
        assert checkpoint["session"] == {"id": sid, "revision": revision} and checkpoint["pending"] is None, checkpoint
        stop(kill=True)
        release_provider.set()
        start(("--policy-mode", "enforce", "--policy-tool-deny", "proc_exec",
               "--policy-approval-tools", "proc_exec", "--policy-approval-timeout-ms", "100"))
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("session_crash") == 1 and counts["session_crash"] == 3, value
        assert result(value)["effect"]["replayed_tool_calls"] == 1, value
        assert_session_commit("session_crash", sid, original, revision)
        stop()
        start()

        # A real authenticated conversation edit wins while a provider is in
        # flight. No stale final result or automatic rebase overwrites it.
        provider_waiting.clear()
        release_provider.clear()
        sid, original, revision = seed_session("session_changed")
        wid = submit("session_changed", session=sid)
        wait_for(provider_waiting.is_set, "session edit window absent")
        assert session_rows(sid) == original
        call("/api/v1/session/ui_event", {"session_id": sid, "type": "newer_context",
             "data": {"text": "owner conversation edit"}, "append_to_session": True})
        newer = session_rows(sid)
        release_provider.set()
        value = terminal(wid)
        assert value["workflow"]["status"] == "error" and count("session_changed") == 1, value
        assert result(value)["blocker"]["kind"] == "model_session_changed" and result(value)["retryable"] is False, value
        assert session_rows(sid) == newer and len(newer) == len(original) + 1
        checkpoint = json.loads(sql("SELECT receipt_json FROM workflow_effects WHERE workflow_id=?", (wid,))[0][0])
        assert checkpoint["session_conflict"] is True and checkpoint["pending"] is None, checkpoint
        assert [r["kind"] for r in checkpoint["records"]] == ["provider", "tool", "provider"], checkpoint
        assert next(t for t in value["tasks"] if t["task_id"] == "independent")["status"] == "done", value

        # The conversation and complete run receipt precede task settlement
        # together. Recovery must not append again or overwrite a later edit.
        sid, original, revision = seed_session("session_saved")
        sql("CREATE TRIGGER stop_session_done BEFORE UPDATE OF status ON workflow_tasks WHEN NEW.workflow_id='model_session_saved' AND NEW.task_id='write' AND NEW.status='done' BEGIN SELECT RAISE(FAIL,'before session completion'); END")
        wid = submit("session_saved", others=False, session=sid, attempts=1,
                     budget={"max_tool_calls_total": 1, "max_steps_total": 2})
        wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='completed'", (wid,)), "atomic session receipt absent")
        assert_session_commit("session_saved", sid, original, revision)
        call("/api/v1/session/ui_event", {"session_id": sid, "type": "after_completion",
             "data": {"text": "preserve newer edit on recovery"}, "append_to_session": True})
        newer = session_rows(sid)
        before = counts["session_saved"]
        stop(kill=True)
        sql("DROP TRIGGER stop_session_done")
        start(("--policy-mode", "enforce", "--policy-tool-deny", "proc_exec"))
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("session_saved") == 1 and counts["session_saved"] == before, value
        assert result(value)["effect"]["replayed"] is True and result(value)["effect"]["session_committed"] is True, value
        assert session_rows(sid) == newer
        stop()
        start()

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

        # The same preparation helper still serves ordinary non-workflow
        # requests. This local basic-tools path needs no durable host guard.
        image = {"name": "synthetic.png", "mime": "image/png",
                 "b64": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="}
        direct = call("/api/v1/run", {"prompt": "__AGENT_MM_V1__" + json.dumps({"images": [image]}) + "\nvision_direct",
                      "tools": "basic", "no_session": True, "no_default_system": True, "max_steps": 2,
                      "max_retries": 0, "timeout_ms": 5000, "api_key": "synthetic", "model": "stub",
                      "base_url": f"http://127.0.0.1:{stub.server_port}/moonshot/vision_direct/v1"})
        assert direct["ok"] is True and direct["assistant_text"] == "completed:vision_direct", direct
        assert direct["total_tokens"] == 8 and counts["vision_direct"] == 2 and count("vision_direct") == 0, direct
        assert next(e["data"] for e in direct["events"] if e["type"] == "vision_prefetch")["replayed"] is False

        # Image preparation is part of the verified prefix. A changed second
        # caption would invalidate the transcript, so the stub labels every one.
        # Current provider-step/token grants include fresh preparation only.
        for case in ("vision_transient", "vision_fallback"):
            wid = submit(case, others=False, attempts=2,
                         budget={"max_tool_calls_total": 1, "max_steps_total": 4, "max_total_tokens": 20})
            value = terminal(wid)
            out = result(value)
            assert value["workflow"]["status"] == "done" and count(case) == 1, value
            assert counts[case] == 4 and prefetch_counts[case] == 1 and prefetch_caps[case] == 17, counts
            assert out["effect"]["replayed_prefetch_calls"] == 1 and out["effect"]["replayed_tool_calls"] == 1, value
            assert out["steps_executed"] == 1 and out["total_tokens"] == 4 and out["tool_calls_total"] == 0, value
            assert value["budget_accounting"]["charged"]["max_steps_total"] == 4, value
            # An unavailable preparation keeps its original fallback. It is not
            # retried later in a way that could reinterpret a receipted action.
            if case == "vision_transient":
                assert value["budget_accounting"]["charged"]["max_total_tokens"] == 12, value
            event = next(e["data"] for e in out["events"] if e["type"] == "vision_prefetch")
            assert event["replayed"] is True and event["ok"] is (case != "vision_fallback"), event

        wid = submit("vision_persistent", others=False, attempts=2)
        value = terminal(wid)
        assert count("vision_persistent") == 1 and prefetch_counts["vision_persistent"] == 1, value
        assert counts["vision_persistent"] == 4 and result(value)["effect"]["resume_available"] is True, value
        assert result(value).get("needs_input") is not True, value
        metadata = call("/api/v1/workflow?workflow_id=" + wid + "&include_results=0")
        assert metadata["effects"][0]["resume_available"] is True and "records" not in metadata["effects"][0], metadata

        # With only one provider step, analysis itself uses that grant. No host
        # call or second model request can be hidden outside the step meter.
        wid = submit("vision_low_steps", others=False, attempts=1, budget={"max_steps_total": 1})
        value = terminal(wid)
        assert value["workflow"]["status"] == "error" and count("vision_low_steps") == 0, value
        assert counts["vision_low_steps"] == 1 and result(value)["steps_executed"] == 1, value
        assert value["budget_accounting"]["charged"]["max_steps_total"] == 1, value

        wid = submit("vision_oversized", others=False, attempts=1)
        value = terminal(wid)
        assert value["workflow"]["status"] == "error" and count("vision_oversized") == 0, value
        assert counts["vision_oversized"] == 1 and result(value)["steps_executed"] == 1, value
        assert result(value)["total_tokens"] == 4, value
        assert not sql("SELECT 1 FROM workflow_effects WHERE workflow_id=?", (wid,)), value

        provider_waiting.clear()
        release_provider.clear()
        sid, original, revision = seed_session("vision_crash")
        wid = submit("vision_crash", others=False, attempts=3, session=sid, budget={"max_tool_calls_total": 1})
        wait_for(provider_waiting.is_set, "vision receipted frontier absent")
        checkpoint = json.loads(sql("SELECT receipt_json FROM workflow_effects WHERE workflow_id=?", (wid,))[0][0])
        assert [r["kind"] for r in checkpoint["records"]] == ["vision_prefetch", "provider", "tool"], checkpoint
        assert session_rows(sid) == original and prefetch_counts["vision_crash"] == 1
        stop(kill=True)
        release_provider.set()
        start(("--policy-mode", "enforce", "--policy-tool-deny", "proc_exec",
               "--policy-approval-tools", "proc_exec", "--policy-approval-timeout-ms", "100"))
        value = terminal(wid)
        assert value["workflow"]["status"] == "done" and count("vision_crash") == 1, value
        assert counts["vision_crash"] == 4 and prefetch_counts["vision_crash"] == 1, counts
        assert result(value)["effect"]["replayed_prefetch_calls"] == 1, value
        assert_session_commit("vision_crash", sid, original, revision)
        stop()
        start()

        provider_waiting.clear()
        release_provider.clear()
        sid, original, revision = seed_session("vision_session_changed")
        wid = submit("vision_session_changed", others=False, session=sid)
        wait_for(provider_waiting.is_set, "vision session edit window absent")
        call("/api/v1/session/ui_event", {"session_id": sid, "type": "newer_vision_context",
             "data": {"text": "preserve newer image conversation"}, "append_to_session": True})
        newer = session_rows(sid)
        release_provider.set()
        value = terminal(wid)
        assert result(value)["blocker"]["kind"] == "model_session_changed" and count("vision_session_changed") == 1, value
        assert session_rows(sid) == newer and prefetch_counts["vision_session_changed"] == 1

        # An unreceipted physical action still forbids any restart work,
        # including another supposedly harmless image-analysis request.
        wid = submit("vision_lost", others=False)
        wait_for(lambda: count("vision_lost") == 1, "vision host action did not begin")
        before = counts["vision_lost"]
        stop(kill=True)
        time.sleep(1.6)
        start()
        value = terminal(wid)
        assert count("vision_lost") == 1 and counts["vision_lost"] == before and prefetch_counts["vision_lost"] == 1, value
        assert result(value)["blocker"]["kind"] == "model_run_outcome_unknown", value

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
        release_provider.set()
        stop()
        stub.shutdown()
        stub.server_close()
        log.close()
print("Model-selected host effects: provider retry, multi-tool run, crash/receipt recovery, atomic session publication, conversation-edit fences, exact image preparation replay, no fresh grants, policy/approval, uncertainty, timeout and owner stops passed")
