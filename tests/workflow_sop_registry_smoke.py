"""Exercise saved process versions through a real authenticated daemon, without providers."""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
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
token = "sop-registry-test-token"


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


def sop(version="1", delay=0):
    return {"schema": "agentd.sop.v1", "id": "inspection", "version": version, "title": "Inspection",
            "stages": [{"id": "check", "actions": [{"task_id": "wait", "kind": "delay", "delay_ms": delay}]},
                       {"id": "release", "actions": [{"task_id": "release", "kind": "delay", "delay_ms": 0}]}]}


with tempfile.TemporaryDirectory(prefix="sop-registry-") as temp:
    root = Path(temp)
    database = root / "state.sqlite"
    log = (root / "daemon.log").open("w+")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    process = None

    def call(path, payload=None, expected=200, auth=True):
        headers = {"Content-Type": "application/json"}
        if auth:
            headers["Authorization"] = "Bearer " + token
        data = None if payload is None else json.dumps(payload).encode()
        request = urllib.request.Request(base + path, data=data, headers=headers)
        try:
            response = opener.open(request, timeout=5)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            body = json.load(response)
            assert response.code == expected, (path, response.code, expected, body)
            return body

    def start():
        global process
        process = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(port),
            "--auth-token", token, "--db-path", str(database), "--state-dir", str(root / "state"),
            "--workflow-concurrency", "1"], cwd=root, stdout=log, stderr=log)
        wait_for(lambda: process.poll() is None and call("/api/v1/health").get("ok"), "daemon did not start")

    def stop():
        global process
        if process:
            process.terminate()
            process.wait(timeout=10)
            process = None

    def detail(wid):
        return call("/api/v1/workflow?workflow_id=" + wid + "&include_spec=1&include_tasks=1&include_results=1")

    def submit(ref, **settings):
        return call("/api/v1/workflow/submit", {"sop_ref": ref, **settings})["workflow_id"]

    try:
        start()
        call("/api/v1/workflow/sops", expected=401, auth=False)
        call("/api/v1/workflow/sops", {"sop": sop()}, expected=401, auth=False)
        assert call("/api/v1/workflow/sops")["definitions"] == []
        first = call("/api/v1/workflow/sops", {"sop": sop(delay=30000)}, expected=201)["definition"]
        assert first["sop"] == sop(delay=30000)
        ref = {key: first[key] for key in ("id", "version", "definition_hash")}
        same = call("/api/v1/workflow/sops", {"sop": sop(delay=30000)})
        assert same["created"] is False and same["definition"] == first
        changed = sop(delay=0)
        call("/api/v1/workflow/sops", {"sop": changed}, expected=409)
        for payload in ({}, {"sop": sop(), "run": True}, {"sop": {"schema": "unknown"}}, []):
            call("/api/v1/workflow/sops", payload, expected=400)
        for key in ("api_key", "Authorization", "password", "proxy-authorization"):
            secret = sop("secret-" + key)
            secret["stages"][0]["actions"][0]["request"] = {key: "fixture-do-not-store"}
            response = call("/api/v1/workflow/sops", {"sop": secret}, expected=400)
            assert "fixture-do-not-store" not in json.dumps(response)
        call("/api/v1/workflow/sops", {"sop": sop("too-big", "x" * (128 * 1024))}, expected=400)
        call("/api/v1/workflow/sops?id=inspection", expected=400)
        for query in ("limit=0", "limit=101", "limit=2x", "before_id=-1", "before_id=0", "before_id=99999999999999999999"):
            call("/api/v1/workflow/sops?" + query, expected=400)
        call("/api/v1/workflow/submit", {"sop_ref": {"id": "missing", "version": "1"}}, expected=404)
        call("/api/v1/workflow/submit", {"sop_ref": {**ref, "definition_hash": "sha256:wrong"}}, expected=409)
        for field, value in (("sop", sop()), ("tasks", []), ("sop_compiled", {})):
            call("/api/v1/workflow/submit", {"sop_ref": ref, field: value}, expected=400)

        running = submit(ref, idempotency_key="saved-version-run")
        assert submit(ref, idempotency_key="saved-version-run") == running
        wait_for(lambda: detail(running)["workflow"]["status"] == "running", "saved SOP never ran")
        second = call("/api/v1/workflow/sops", {"sop": sop("2")}, expected=201)["definition"]
        pinned = detail(running)["spec"]
        assert pinned["sop"] == first["sop"] and pinned["sop_ref"] == ref
        assert pinned["sop_compiled"]["definition_hash"] == ref["definition_hash"]
        call("/api/v1/workflow/cancel", {"workflow_id": running})
        wait_for(lambda: detail(running)["workflow"]["status"] == "cancelled", "cancelled SOP resumed")
        assert all(t["status"] != "done" for t in detail(running)["tasks"] if t["task_id"] == "release")

        # Races on a new version are one insert plus idempotent reads.
        def save_race(_):
            request = urllib.request.Request(base + "/api/v1/workflow/sops",
                data=json.dumps({"sop": sop("3")}).encode(),
                headers={"Content-Type": "application/json", "Authorization": "Bearer " + token})
            with opener.open(request, timeout=5) as response:
                return response.code, json.load(response)
        with ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(save_race, range(6)))
        assert sorted(code for code, _ in results) == [200] * 5 + [201]
        assert len({body["definition"]["definition_hash"] for _, body in results}) == 1
        page = call("/api/v1/workflow/sops?limit=1")
        assert page["definitions"][0]["version"] == "3" and "sop" not in page["definitions"][0]
        seen = []
        while True:
            seen.extend(row["version"] for row in page["definitions"])
            cursor = page.get("next_before_id")
            if cursor is None:
                break
            page = call(f"/api/v1/workflow/sops?limit=1&before_id={cursor}")
        assert seen == ["3", "2", "1"]

        stop()
        start()
        assert call("/api/v1/workflow/sops?id=inspection&version=1")["definition"] == first
        assert detail(running)["workflow"]["status"] == "cancelled"
        assert detail(running)["spec"]["sop"] == first["sop"]
        finished = submit({"id": "inspection", "version": "2"})
        done = wait_for(lambda: (d if d["workflow"]["status"] == "done" else None) if (d := detail(finished)) else None, "saved SOP did not complete")
        assert done["spec"]["sop_ref"]["definition_hash"] == second["definition_hash"]
        assert all(t["status"] == "done" for t in done["tasks"])

        # Save validates structure, never grants task execution or loosens normal validation.
        invalid = sop("invalid-action", "not-a-duration")
        call("/api/v1/workflow/sops", {"sop": invalid}, expected=201)
        call("/api/v1/workflow/submit", {"sop_ref": {"id": "inspection", "version": "invalid-action"}}, expected=400)
        stop()
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()[0] == "37"
            assert db.execute("SELECT count(*) FROM workflow_sop_definitions WHERE sop_json LIKE '%fixture-do-not-store%'").fetchone()[0] == 0
            db.execute("UPDATE workflow_sop_definitions SET sop_json='{}' WHERE version='1'")
        start()
        call("/api/v1/workflow/sops?id=inspection&version=1", expected=500)
        call("/api/v1/workflow/submit", {"sop_ref": ref}, expected=500)
        print("Saved SOP API auth, immutable races, pinned runs, pagination, cancellation, restart and integrity checks passed")
    except Exception:
        log.flush()
        log.seek(0)
        print(log.read(), file=sys.stderr)
        raise
    finally:
        stop()
        log.close()
