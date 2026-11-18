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
            if after_tool and (((case in ("crash_receipted", "session_crash", "session_changed") or case.startswith("grant_")) and number == 2) or
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
                if not prefetch and case != "vision_direct" and (not after_tool or (case in ("frontier_budget", "grant_denied") and number == 3)):
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

    def submit(case, others=True, budget=None, deadline=None, attempts=8, session=None, children=None):
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
            {"task_id": "write", "priority": 100, "max_attempts": attempts, "kind": "delegate",
             "delegate": {"stop_on_ok": False, "attempts": [
               {"id": "child-" + str(i), "request": {**request, "prompt": name}}
               for i, name in enumerate(children or [case])]}}]}
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


    def checkpoint_grant_cases():
        # A real crash leaves a receipted active child but conservatively loses
        # its parent grant. Completed children must remain free while that exact
        # active frontier either finishes or refuses fresh work without fallback.
        for mode in ("tools", "denied", "steps", "later", "missing", "corrupt", "stopped", "session_changed", "elapsed", "tokens"):
            case = "grant_" + mode
            first, last = "prefix_" + mode, "later_" + mode
            provider_waiting.clear(); release_provider.clear()
            sid = seed_session(case)[0] if mode == "session_changed" else None
            names = [first, case] + ([last] if mode != "tools" else [])
            limits = {"max_tool_calls_total": 2}
            if mode == "steps": limits["max_steps_total"] = 4
            if mode == "elapsed": limits["max_elapsed_ms_total"] = 10000
            if mode == "tokens": limits["max_total_tokens"] = 1000
            wid = submit(case, children=names, budget=limits, session=sid)
            wait_for(provider_waiting.is_set, "active delegated child did not reach saved frontier: " + mode)
            assert count(first) == count(case) == 1 and count(last) == 0
            before = {name: counts.get(name, 0) for name in names}
            old_grant = sql("SELECT tools,steps FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='write' ORDER BY attempt DESC LIMIT 1", (wid,))[0]
            assert old_grant[0] == 2
            if mode == "session_changed":
                call("/api/v1/session/ui_event", {"session_id": sid, "type": "newer_context",
                    "ts_unix_ms": 1700000002000, "data": {"text": "owner edit with no fresh tool grant"},
                    "append_to_session": True})
                newer = session_rows(sid)
            stop(kill=True); release_provider.set()
            with sqlite3.connect(database) as db:
                if mode == "missing":
                    db.execute("DELETE FROM workflow_effects WHERE workflow_id=? AND effect_slot='delegate.v1:1'", (wid,))
                if mode == "corrupt":
                    db.execute("UPDATE workflow_effects SET receipt_json='{}' WHERE workflow_id=? AND effect_slot='delegate.v1:1'", (wid,))
                if mode == "stopped":
                    db.execute("UPDATE workflows SET cancel_requested=1 WHERE workflow_id=?", (wid,))
            start()
            value = terminal(wid)
            assert count(first) == count(case) == 1 and count(last) == 0, (mode, value, counts)
            assert counts[first] == before[first] and counts.get(last, 0) == 0, (mode, value, counts)
            if mode in ("tools", "denied", "later"):
                assert counts[case] == before[case] + 1, (mode, value, counts)
                grants = sql("SELECT tools,steps FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='write' ORDER BY attempt DESC LIMIT 1", (wid,))[0]
                assert grants[0] == 0, (mode, grants)
            else:
                assert counts[case] == before[case], (mode, value, counts)
            if mode == "tools":
                assert value["workflow"]["status"] == "done", value
                out = result(value)
                assert out["tool_calls_total"] == 0 and out["steps_executed"] == 1, out
                assert out["delegate"]["attempts"][0]["effect"]["replayed"] is True, out
                assert out["delegate"]["attempts"][1]["effect"]["replayed_tool_calls"] == 1, out
                cumulative = sql("SELECT tool_calls_total_cum,steps_executed_cum FROM workflow_tasks WHERE workflow_id=? AND task_id='write'", (wid,))
                assert cumulative == [(0, 1)], (cumulative, old_grant, out)
                # Observed usage and conservative crash charges are deliberately
                # separate. The lost tool reservation must not be refunded.
                accounting = value["budget_accounting"]
                assert accounting["charged"]["max_tool_calls_total"] == old_grant[0], accounting
                assert accounting["uncertain"]["max_tool_calls_total"] == old_grant[0], accounting
                assert accounting["active_attempts"] == 0, accounting
            else:
                assert value["workflow"]["status"] != "done", (mode, value)
            if mode in ("denied", "steps"):
                assert result(value)["retryable"] is False and result(value)["effect"]["resume_available"], value
                assert not value["workflow"]["cancel_requested"], value
            if mode == "steps":
                assert sql("SELECT tools,steps FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='write' ORDER BY attempt DESC LIMIT 1", (wid,)) == [(0, 0)]
            if mode == "later":
                assert result(value)["delegate"]["attempts_run"] == 2 and "budget exceeded" in result(value)["error"], value
            if mode == "session_changed":
                assert result(value)["blocker"]["kind"] == "model_session_changed" and session_rows(sid) == newer, value
            if mode == "stopped":
                assert value["workflow"]["cancel_requested"], value

        # Fault the final child receipt transaction after the actual final
        # provider response has been checkpointed. Removing the injected fault
        # and simulating the parent crash tail must finish from cached records
        # with no remaining provider steps or tool grants and no new I/O.
        case, names = "cached_tail", ["tail_prefix", "tail_active"]
        with sqlite3.connect(database) as db:
            db.execute("""CREATE TRIGGER stop_final_child_receipt BEFORE UPDATE ON workflow_effects
              WHEN NEW.workflow_id='model_cached_tail' AND NEW.effect_slot='delegate.v1:1' AND NEW.state='completed'
              BEGIN SELECT RAISE(ABORT, 'synthetic final receipt fault'); END""")
        wid = submit(case, children=names, others=False,
                     budget={"max_tool_calls_total": 2, "max_steps_total": 4})
        value = terminal(wid)
        assert value["workflow"]["status"] == "error" and all(count(name) == 1 for name in names), value
        stored = sql("SELECT state,receipt_json FROM workflow_effects WHERE workflow_id=? AND effect_slot='delegate.v1:1'", (wid,))[0]
        checkpoint = json.loads(stored[1])
        assert stored[0] == "dispatching" and checkpoint["schema"] == "agentd.model_run.checkpoint.v1"
        assert [record["kind"] for record in checkpoint["records"]] == ["provider", "tool", "provider"]
        before = {name: counts[name] for name in names}
        stop()
        with sqlite3.connect(database) as db:
            db.execute("DROP TRIGGER stop_final_child_receipt")
            db.execute("UPDATE workflow_tasks SET status='running',result_json=NULL,finished_unix_ms=NULL WHERE workflow_id=? AND task_id='write'", (wid,))
            db.execute("UPDATE workflows SET status='running',result_json=NULL WHERE workflow_id=?", (wid,))
        start()
        value = terminal(wid)
        assert value["workflow"]["status"] == "done", value
        assert all(counts[name] == before[name] and count(name) == 1 for name in names), (value, counts)
        assert result(value)["tool_calls_total"] == result(value)["steps_executed"] == 0, value
        assert sql("SELECT tools,steps FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='write' ORDER BY attempt DESC LIMIT 1", (wid,)) == [(0, 0)]
        assert sql("SELECT tool_calls_total_cum,steps_executed_cum FROM workflow_tasks WHERE workflow_id=? AND task_id='write'", (wid,)) == [(2, 4)]
        print("PASS delegated partial checkpoint grants, cached terminal frontier and fail-closed boundaries")

    try:
        start()
        checkpoint_grant_cases()
        provider_waiting.clear(); release_provider.clear()
        wid = submit("chain", children=["first_child", "crash_receipted", "last_child"])
        wait_for(provider_waiting.is_set, "second child did not reach post-tool provider")
        assert count("first_child") == count("crash_receipted") == 1
        assert count("last_child") == 0
        before_first = counts["first_child"]
        stop(kill=True)
        release_provider.set()
        start()
        value = terminal(wid)
        assert value["workflow"]["status"] == "done", value
        assert counts["first_child"] == before_first and count("first_child") == 1, value
        assert count("crash_receipted") == count("last_child") == 1, value
        effects = value["effects"]
        assert {e["effect_slot"] for e in effects} == {"delegate.v1:0","delegate.v1:1","delegate.v1:2"}, effects
        assert len({e["effect_id"] for e in effects}) == 3 and all(e["task_id"] == "write" for e in effects)
        # Prior completed child replay has zero fresh charge.
        assert result(value)["delegate"]["attempts"][0]["tool_calls_total"] == 0, value

        wid = submit("retry_chain", children=["retry_first", "transient_after", "retry_last"])
        value = terminal(wid)
        assert value["workflow"]["status"] == "done", value
        assert count("retry_first") == count("transient_after") == count("retry_last") == 1, value
        assert counts["retry_first"] == 2, counts

        wid = submit("lost_chain", children=["lost_first", "lost", "must_not_run"])
        wait_for(lambda: count("lost") == 1, "unreceipted effect not started")
        stop(kill=True);time.sleep(2);start()
        value=terminal(wid)
        assert value["workflow"]["status"] == "error", value
        assert result(value)["needs_input"] and result(value)["retryable"] is False, value
        assert count("lost") == 1 and counts.get("must_not_run",0) == 0, counts

        wid=submit("bounded", children=["bounded_first","bounded_second"],
                   budget={"max_tool_calls_total":2,"max_steps_total":4})
        value=terminal(wid)
        assert count("bounded_first") == 1 and count("bounded_second") <= 1, value
        assert result(value).get("tool_calls_total",0) <= 2, value

        sid, original, revision = seed_session("session_transient")
        wid = submit("session_transient", session=sid)
        value = terminal(wid)
        assert value["workflow"]["status"] == "done", value
        assert count("session_transient") == 1
        assert_session_commit("session_transient", sid, original, revision)

        # Two children share a real persistent conversation. Crash in child two
        # must not recommit child one; an intervening edit must stop the chain.
        for case,edited in (("session_crash",False),("session_changed",True)):
            provider_waiting.clear();release_provider.clear()
            sid, original, revision=seed_session("chain_"+case)
            first="first_"+case
            last="last_"+case
            wid=submit("chain_"+case, session=sid, children=[first,case,last])
            wait_for(provider_waiting.is_set,"second session child did not reach checkpoint")
            assert count(first)==1 and count(case)==1 and count(last)==0
            rows=session_rows(sid)
            assert sum(row["role"]=="user" and row["content"]==first for row in rows)==1,rows
            before_first=counts[first]
            if edited:
                call("/api/v1/session/ui_event", {"session_id":sid,"type":"newer_context",
                    "ts_unix_ms":1700000001000,"data":{"text":"owner edit between children"},"append_to_session":True})
                newer=session_rows(sid)
            stop(kill=True);release_provider.set();start()
            value=terminal(wid)
            assert count(first)==1 and counts[first]==before_first and count(case)==1, (value,counts)
            if edited:
                assert value["workflow"]["status"]=="error",value
                assert result(value)["blocker"]["kind"]=="model_session_changed",value
                assert count(last)==0 and session_rows(sid)==newer,value
            else:
                assert value["workflow"]["status"]=="done" and count(last)==1,value
                rows=session_rows(sid)
                for name in (first,case,last):
                    assert sum(row["role"]=="user" and row["content"]==name for row in rows)==1,rows
                    assert sum(row["role"]=="assistant" and row["content"]=="completed:"+name for row in rows)==1,rows
                assert sql("SELECT revision FROM session_revisions WHERE session_id=?",(sid,))[0][0]==revision+3

        # Simulate the narrow crash tail: durable children completed, parent
        # publication lost, and every fresh resource dimension already charged.
        limits={"max_tool_calls_total":8,"max_steps_total":16,"max_elapsed_ms_total":60000,"max_total_tokens":1000}
        for mode in ("complete","missing","corrupt","stopped"):
            names=["zero_"+mode+"_first","zero_"+mode+"_last"]
            wid=submit("zero_"+mode, others=False, budget=limits, children=names)
            value=terminal(wid)
            assert value["workflow"]["status"] == "done", value
            before={name:counts[name] for name in names}
            stop()
            with sqlite3.connect(database) as db:
                db.execute("UPDATE workflow_tasks SET status='running',result_json=NULL,finished_unix_ms=NULL,tool_calls_total_cum=8,steps_executed_cum=16,elapsed_ms_cum=60000,total_tokens_cum=1000 WHERE workflow_id=? AND task_id='write'",(wid,))
                db.execute("UPDATE workflows SET status='running',result_json=NULL,cancel_requested=? WHERE workflow_id=?",(int(mode=="stopped"),wid))
                if mode=="missing":
                    db.execute("DELETE FROM workflow_effects WHERE workflow_id=? AND effect_slot='delegate.v1:1'",(wid,))
                if mode=="corrupt":
                    db.execute("UPDATE workflow_effects SET receipt_json='{}' WHERE workflow_id=? AND effect_slot='delegate.v1:1'",(wid,))
            start()
            value=terminal(wid)
            assert all(counts[name]==before[name] and count(name)==1 for name in names), (mode,value,counts)
            if mode=="complete":
                assert value["workflow"]["status"]=="done", value
                outcome=result(value)
                assert all(outcome[k]==0 for k in ("tool_calls_total","steps_executed","elapsed_ms","total_tokens")),outcome
                assert sql("SELECT tool_calls_total_cum,steps_executed_cum,elapsed_ms_cum,total_tokens_cum FROM workflow_tasks WHERE workflow_id=? AND task_id='write'",(wid,))==[(8,16,60000,1000)]
                grants=sql("SELECT tools,steps,elapsed_ms,tokens FROM workflow_budget_reservations WHERE workflow_id=? ORDER BY attempt DESC LIMIT 1",(wid,))
                assert grants==[(0,0,0,0)],grants
            else:
                assert value["workflow"]["status"]!="done", (mode,value)
            if mode=="stopped": assert value["workflow"]["cancel_requested"],value

        # Legacy slot identity remains readable through an actual v40 migration.
        stop()
        with sqlite3.connect(database) as db:
            db.execute("DELETE FROM workflow_effects WHERE effect_slot != ''")
            db.execute("INSERT INTO workflow_effects(effect_id,workflow_id,task_id,request_hash,mode,state,created_unix_ms,updated_unix_ms,receipt_json) VALUES(\'legacy-fixture\',\'model_chain\',\'write\',\'original-hash\',\'guarded\',\'completed\',1,2,\'original-receipt\')")
            db.executescript("""
              ALTER TABLE workflow_effects RENAME TO effects_v41_fixture;
              CREATE TABLE workflow_effects AS SELECT effect_id,workflow_id,task_id,request_hash,mode,state,revision,attempt,created_unix_ms,updated_unix_ms,receipt_json,reconcile_url FROM effects_v41_fixture;
              DROP TABLE effects_v41_fixture;
              UPDATE meta SET value='40' WHERE key='schema_version';
            """)
        start()
        assert sql("SELECT value FROM meta WHERE key='schema_version'")[0][0] == "42"
        assert sql("SELECT request_hash,receipt_json,effect_slot FROM workflow_effects WHERE effect_id='legacy-fixture'") == [("original-hash","original-receipt","")]
        print("PASS delegated distinct child journals, crash replay, transient retry, stop-on-uncertainty, bounded fresh usage and v40 migration")
    except BaseException:
        log.flush();log.seek(0);print(log.read()[-9000:],file=sys.stderr);raise
    finally:
        release_provider.set();stop();stub.shutdown();stub.server_close();log.close()
