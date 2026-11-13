"""Read-only submission recovery across namespaces and daemon restart."""
import json, os, socket, sqlite3, subprocess, sys, tempfile, time, urllib.request, urllib.error
from pathlib import Path
binary = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
base = f"http://127.0.0.1:{port}"
def call(path, data=None, expected=200, auth=True):
    req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(),
        headers={"Content-Type": "application/json", **({"Authorization": "Bearer recovery-test"} if auth else {})})
    try:
        with opener.open(req, timeout=3) as r: status, body = r.status, json.load(r)
    except urllib.error.HTTPError as e: status, body = e.code, json.load(e)
    assert status == expected, (status, body)
    return body
def ready():
    for _ in range(200):
        try:
            if call("/api/v1/health")["ok"]: return
        except OSError: pass
        time.sleep(.025)
    raise AssertionError("daemon unavailable")
with tempfile.TemporaryDirectory(prefix="workflow-submission-") as root:
    db = str(Path(root) / "state.db")
    log = open(Path(root) / "daemon.log", "w")
    def start():
        p = subprocess.Popen([binary, "--host", "127.0.0.1", "--port", str(port), "--state-dir", root,
            "--db-path", db, "--auth-token", "recovery-test", "--tools", "none", "--no-yolo"],
            stdout=log, stderr=log, env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
        ready(); return p
    process = start()
    try:
        tasks = [{"task_id": "wait", "kind": "delay", "delay_ms": 600000}]
        ids = {}
        for scope in ("", "scope-a", "scope-b"):
            payload = {"tasks": tasks, "idempotency_key": "same-key"}
            if scope: payload.update(allow_sessions=True, session_id=scope)
            ids[scope] = call("/api/v1/workflow/submit", payload)["workflow_id"]
        assert len(set(ids.values())) == 3
        def check():
            for scope, wid in ids.items():
                r = call("/api/v1/workflow?idempotency_key=same-key&session_id=" + scope + "&include_tasks=0")
                assert r["workflow"]["workflow_id"] == wid
                assert r["workflow"].get("session_id", "") == scope
            call("/api/v1/workflow?idempotency_key=missing", expected=404)
            call("/api/v1/workflow?idempotency_key=same-key&session_id=missing", expected=404)
            call("/api/v1/workflow?idempotency_key=same-key", expected=401, auth=False)
            call("/api/v1/workflow?idempotency_key=same-key&workflow_id=" + ids[""], expected=400)
            call("/api/v1/workflow", expected=400)
            with sqlite3.connect(db) as c: assert c.execute("select count(*) from workflows").fetchone()[0] == 3
        check()
        process.kill(); process.wait()
        process = start()
        check()
        print("Authenticated read-only recovery preserved exact namespaces after crash/restart and created zero duplicate runs.")
    finally:
        process.terminate(); process.wait(timeout=5); log.close()
