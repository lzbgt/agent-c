"""Validate exact browser-authored SOP on isolated real daemon. Reads JSON on stdin."""
import json, os, socket, subprocess, sys, tempfile, time, urllib.request
from pathlib import Path

source = json.load(sys.stdin)
binary = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
base = f"http://127.0.0.1:{port}"
def call(path, data=None):
    req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer authored-sop-test"})
    with opener.open(req, timeout=3) as response:
        return json.load(response)
def wait(fn):
    end = time.monotonic() + 12
    while time.monotonic() < end:
        try:
            result = fn()
            if result:
                return result
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(.025)
    raise AssertionError("authored process did not reach expected state")
def detail(wid):
    return call("/api/v1/workflow?workflow_id=" + wid + "&include_tasks=1&include_results=1")
def final(wid):
    return wait(lambda: (r if (r := detail(wid))["workflow"]["status"] in ("done", "error", "cancelled") else None))
with tempfile.TemporaryDirectory(prefix="authored-sop-") as root:
    with open(Path(root) / "daemon.log", "w") as log:
        process = subprocess.Popen([binary, "--host", "127.0.0.1", "--port", str(port),
            "--state-dir", root, "--db-path", str(Path(root) / "state.db"),
            "--auth-token", "authored-sop-test", "--tools", "none", "--no-yolo",
            "--workflow-concurrency", "1", "--workflow-poll-ms", "10"],
            stdout=log, stderr=log, env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
        try:
            wait(lambda: call("/api/v1/health")["ok"])
            actions = [a for s in source["sop"]["stages"] for a in s["actions"]]
            timer = next(a for a in actions if a["kind"] == "timer")
            decision = next(a for a in actions if a["kind"] == "decision")
            assert timer["due_unix_ms"] == 978307200123, timer
            assert decision["decision"]["timeout_ms"] == 90000
            assert len(decision["decision"]["choices"]) == 3
            definition = call("/api/v1/workflow/sops", {"sop": source["sop"]})["definition"]
            pinned = {"sop_ref": {k: definition[k] for k in ("id", "version", "definition_hash")}}
            for choice in decision["decision"]["choices"][:2]:
                wid = call("/api/v1/workflow/submit", pinned)["workflow_id"]
                row = wait(lambda: next((r for r in detail(wid).get("decisions", []) if r["available"]), None))
                assert row["choices"] == decision["decision"]["choices"]
                payload = {"workflow_id": wid, "task_id": decision["task_id"],
                    "decision_id": row["decision_id"], "choice_id": choice["id"],
                    "idempotency_key": "authored-" + choice["id"], "note": "browser-authored"}
                call("/api/v1/workflow/decision", payload)
                result = final(wid)
                assert result["workflow"]["status"] == ("done" if choice["outcome"] == "continue" else "error"), result
                assert call("/api/v1/workflow/decision", payload)["replayed"] is True
            print("Exact browser-authored SOP saved, pinned and executed: UTC milliseconds and continue/fail choices verified.")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
