"""Real one-worker decision waits, explicit choices and restart/stop semantics. No providers."""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import json
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
token = "decision-smoke-fixture"


def wait_for(fn, message, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, KeyError, urllib.error.URLError):
            pass
        time.sleep(.04)
    raise AssertionError(message)


def decision(timeout=0):
    return {"task_id": "review", "kind": "decision", "max_attempts": 4,
            "decision": {"prompt": "Release this inspected batch?", "timeout_ms": timeout,
                         "choices": [{"id": "release", "label": "Release batch", "outcome": "continue"},
                                     {"id": "reject", "label": "Reject batch", "outcome": "fail"}]}}


with tempfile.TemporaryDirectory(prefix="workflow-decision-") as temp:
    root = Path(temp)
    database = root / "state.sqlite"
    log = (root / "daemon.log").open("w+")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    process = None

    def call(path, payload=None, expected=200, auth=True, with_status=False):
        headers = {"Content-Type": "application/json"}
        if auth:
            headers["Authorization"] = "Bearer " + token
        data = None if payload is None else json.dumps(payload).encode()
        try:
            response = opener.open(urllib.request.Request(base + path, data=data, headers=headers), timeout=5)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            body = json.load(response)
            allowed = expected if isinstance(expected, tuple) else (expected,)
            assert response.code in allowed, (path, response.code, expected, body)
            return (response.code, body) if with_status else body

    def start():
        global process
        process = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(port),
            "--auth-token", token, "--db-path", str(database), "--state-dir", str(root / "state"),
            "--workflow-concurrency", "1"], cwd=root, stdout=log, stderr=log)
        wait_for(lambda: process.poll() is None and call("/api/v1/health").get("ok"), "daemon did not start")

    def stop(crash=False):
        global process
        if process:
            process.kill() if crash else process.terminate()
            process.wait(timeout=10)
            process = None

    def detail(wid, suffix="&include_tasks=1&include_results=1"):
        return call("/api/v1/workflow?workflow_id=" + wid + suffix)

    def submit(timeout=0, **options):
        return call("/api/v1/workflow/submit", {"tasks": [
            decision(timeout),
            {"task_id": "independent", "kind": "delay", "delay_ms": 0},
            {"task_id": "release", "kind": "delay", "delay_ms": 0, "depends_on": ["review"]}],
            **options})["workflow_id"]

    def waiting(wid):
        return wait_for(lambda: next((d for d in detail(wid)["decisions"] if d["available"]), None),
                        "decision did not become available")

    def answer(row, choice="release", key="answer-1", note="Reviewed evidence"):
        return {"workflow_id": row["workflow_id"], "task_id": row["task_id"],
                "decision_id": row["decision_id"], "choice_id": choice,
                "idempotency_key": key, "note": note}

    def terminal(wid, status):
        return wait_for(lambda: (d if d["workflow"]["status"] == status else None)
                        if (d := detail(wid)) else None, f"workflow did not reach {status}")

    try:
        start()
        call("/api/v1/workflow/decision", {}, expected=401, auth=False)
        for payload in ({}, {"workflow_id": "missing"}, [], {"note": "x" * 9000}):
            call("/api/v1/workflow/decision", payload, expected=400)
        for mutate in ("duplicate", "outcome", "empty", "allow_error", "timeout", "nul", "unknown"):
            task = decision()
            if mutate == "duplicate":
                task["decision"]["choices"][1]["id"] = "release"
            elif mutate == "outcome":
                task["decision"]["choices"][0]["outcome"] = "arbitrary"
            elif mutate == "empty":
                task["decision"]["choices"] = []
            elif mutate == "allow_error":
                task["allow_error"] = True
            elif mutate == "timeout":
                task["decision"]["timeout_ms"] = -1
            elif mutate == "nul":
                task["decision"]["prompt"] = "bad\0prompt"
            else:
                task["decision"]["role"] = "owner"
            call("/api/v1/workflow/submit", {"tasks": [task]}, expected=400)

        wid = submit(workflow_limits={"max_tool_calls_total": 1, "max_steps_total": 1, "max_elapsed_ms_total": 2000, "max_total_tokens": 1})
        row = waiting(wid)
        wait_for(lambda: next(t for t in detail(wid)["tasks"] if t["task_id"] == "independent")["status"] == "done",
                 "one waiting decision blocked unrelated work on one worker")
        other = call("/api/v1/workflow/submit", {"tasks": [{"task_id": "other", "kind": "delay", "delay_ms": 0}]})["workflow_id"]
        terminal(other, "done")
        state = detail(wid)
        review = next(t for t in state["tasks"] if t["task_id"] == "review")
        assert review["status"] == "queued" and review["attempt"] == 0
        assert next(t for t in state["tasks"] if t["task_id"] == "release")["status"] == "queued"
        projection = detail(wid, "&include_tasks=0")["decisions"][0]
        assert projection == row and "spec_json" not in projection and "request_json" not in projection
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT count(*) FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='review'", (wid,)).fetchone()[0] == 0

        # Kill while waiting; same definition, expiry and attempt are durable.
        stop(crash=True)
        start()
        restored = waiting(wid)
        assert restored == row
        assert next(t for t in detail(wid)["tasks"] if t["task_id"] == "review")["attempt"] == 0
        for payload in ({**answer(row), "choice_id": "not-configured"},
                        {**answer(row), "outcome": "continue"},
                        {**answer(row), "actor": "owner"},
                        {**answer(row), "note": "x" * 4097}):
            call("/api/v1/workflow/decision", payload, expected=400)
        call("/api/v1/workflow/decision", {**answer(row), "decision_id": "wrong"}, expected=409)
        call("/api/v1/workflow/decision", {**answer(row), "workflow_id": "missing"}, expected=404)
        saved = call("/api/v1/workflow/decision", answer(row))
        assert saved["replayed"] is False and saved["decision"]["choice_id"] == "release"
        done = terminal(wid, "done")
        assert all(t["status"] == "done" for t in done["tasks"])
        review = next(t for t in done["tasks"] if t["task_id"] == "review")
        assert review["attempt"] == 1 and review["result"]["elapsed_ms"] == 0
        assert review["result"]["choice_id"] == "release"
        assert call("/api/v1/workflow/decision", answer(row))["replayed"] is True
        call("/api/v1/workflow/decision", answer(row, choice="reject", key="changed"), expected=409)
        stop()
        start()
        assert call("/api/v1/workflow/decision", answer(row))["replayed"] is True
        assert detail(wid)["workflow"]["status"] == "done"

        rejected = submit()
        reject_row = waiting(rejected)
        call("/api/v1/workflow/decision", answer(reject_row, choice="reject"))
        failed = terminal(rejected, "error")
        assert next(t for t in failed["tasks"] if t["task_id"] == "release")["status"] != "done"
        assert next(t for t in failed["tasks"] if t["task_id"] == "review")["attempt"] == 1

        race = submit()
        race_row = waiting(race)
        def choose(choice):
            return call("/api/v1/workflow/decision", answer(race_row, choice=choice, key=choice),
                        expected=(200, 409), with_status=True)
        with ThreadPoolExecutor(max_workers=2) as pool:
            receipts = list(pool.map(choose, ["release", "reject"]))
        assert sorted(code for code, _ in receipts) == [200, 409]
        winner = next(body for code, body in receipts if code == 200)["decision"]["choice_id"]
        terminal(race, "done" if winner == "release" else "error")
        assert detail(race)["decisions"][0]["choice_id"] == winner

        # Time starts after dependencies, not at submission; expiry never retries.
        timed_spec = decision(1500)
        timed_spec["depends_on"] = ["prepare"]
        timed = call("/api/v1/workflow/submit", {"tasks": [
            {"task_id": "prepare", "kind": "delay", "delay_ms": 200}, timed_spec]})["workflow_id"]
        timed_row = waiting(timed)
        assert timed_row["expires_unix_ms"] - timed_row["created_unix_ms"] == 1500
        prepare = next(t for t in detail(timed)["tasks"] if t["task_id"] == "prepare")
        assert timed_row["created_unix_ms"] >= prepare["finished_unix_ms"]
        timed_out = terminal(timed, "error")
        assert timed_out["decisions"][0]["state"] == "expired"
        assert next(t for t in timed_out["tasks"] if t["task_id"] == "review")["attempt"] == 1
        call("/api/v1/workflow/decision", answer(timed_row), expected=409)

        cancelled = submit()
        cancel_row = waiting(cancelled)
        call("/api/v1/workflow/cancel", {"workflow_id": cancelled})
        call("/api/v1/workflow/decision", answer(cancel_row), expected=409)
        terminal(cancelled, "cancelled")
        assert detail(cancelled)["decisions"][0]["state"] == "stopped"
        deadline = submit(deadline_unix_ms=int(time.time() * 1000) + 2000)
        deadline_row = waiting(deadline)
        terminal(deadline, "cancelled")
        call("/api/v1/workflow/decision", answer(deadline_row), expected=409)
        stop(crash=True)
        start()
        assert detail(cancelled)["workflow"]["cancel_requested"] is True
        assert detail(cancelled)["decisions"][0]["available"] is False

        # The same capability works through an immutable saved SOP, not just raw DAGs.
        sop = {"schema": "agentd.sop.v1", "id": "review-process", "version": "1",
               "stages": [{"id": "review", "actions": [decision()]},
                          {"id": "release", "actions": [{"task_id": "release", "kind": "delay", "delay_ms": 0}]}]}
        call("/api/v1/workflow/sops", {"sop": sop}, expected=201)
        saved_run = call("/api/v1/workflow/submit", {"sop_ref": {"id": "review-process", "version": "1"}})["workflow_id"]
        sop_row = waiting(saved_run)
        call("/api/v1/workflow/decision", answer(sop_row))
        terminal(saved_run, "done")
        assert detail(saved_run, "&include_spec=1")["spec"]["sop"]["version"] == "1"

        corrupt = submit()
        corrupt_row = waiting(corrupt)
        stop()
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()[0] == "38"
            db.execute("UPDATE workflow_decisions SET spec_json='{}' WHERE workflow_id=?", (corrupt,))
        start()
        call("/api/v1/workflow/decision", answer(corrupt_row), expected=500)
        call("/api/v1/workflow?workflow_id=" + corrupt, expected=500)
        print("One-worker decisions: independent progress, pinned SOP, exact choices, races, kill/restart, expiry, stop, auth and integrity passed")
    except Exception:
        log.flush()
        log.seek(0)
        print(log.read(), file=sys.stderr)
        raise
    finally:
        stop()
        log.close()
