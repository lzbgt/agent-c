"""Read-only requirement authority through a real authenticated daemon."""
import copy, json, socket, sqlite3, subprocess, sys, tempfile, time
from pathlib import Path
import urllib.error, urllib.request
agentd = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with tempfile.TemporaryDirectory(prefix="agentc-ledger-api-") as temp:
    root = Path(temp); sessions = root / "sessions"; sessions.mkdir()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    token = "isolated-ledger-fixture"
    log = (root / "daemon.log").open("w+")
    process = subprocess.Popen([agentd, "--host", "127.0.0.1", "--port", str(port),
        "--auth-token", token, "--state-dir", str(root / "state"),
        "--db-path", str(root / "daemon.sqlite"), "--sessions-root", str(sessions)],
        cwd=root, stdout=log, stderr=log)
    def call(query="session_id=goal", expected=200, auth=True):
        request = urllib.request.Request(base + "/api/v1/session/work?" + query,
            headers={"Authorization": "Bearer " + token} if auth else {})
        try: response = opener.open(request, timeout=3)
        except urllib.error.HTTPError as error: response = error
        with response:
            body = json.load(response)
            assert response.status == expected, (response.status, expected, body)
            return body
    def write(sid, state):
        path = sessions / ("session_" + sid) / "work" / "work.sqlite"
        path.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(path) as db:
            db.execute("CREATE TABLE IF NOT EXISTS work_state (id INTEGER PRIMARY KEY, value TEXT NOT NULL)")
            db.execute("INSERT OR REPLACE INTO work_state VALUES (1, ?)", (json.dumps(state),))
        return path
    state = {"schema": "agentd.work.v1", "ledger_id": "work_fixture", "revision": 1,
        "requirements": {"a": {"title": "Build", "status": "pending", "depends_on": []},
                         "b": {"title": "Document", "status": "pending", "depends_on": []}},
        "blockers": {"access": {"status": "open", "reason": "Fixture", "required_action": "Resolve",
                               "attempts": ["Observed denial"], "requirement_ids": ["a"]}}}
    try:
        for _ in range(200):
            try:
                call(expected=401, auth=False); break
            except (OSError, urllib.error.URLError): time.sleep(.03)
        else: raise AssertionError("daemon did not start")
        empty = call()
        assert empty["state"]["revision"] == 0 and not empty["all_requirements_done"]
        assert not (sessions / "session_goal").exists(), "read created session state"
        for query in ("", "session_id=..", "session_id=other%2fgoal", "session_id=goal%5cother"):
            call(query, expected=400)
        path = write("goal", state)
        first = call()
        assert first["session_id"] == "goal" and first["state"]["ledger_id"] == "work_fixture"
        assert first["runnable"] == ["b"] and first["waiting"] == ["a"] and first["can_continue"]
        state["requirements"]["b"].update(status="done", evidence="fixture test passes"); state["revision"] += 1
        write("goal", state)
        assert not call()["can_continue"] and not call()["all_requirements_done"]
        state["blockers"]["access"].update(status="resolved", evidence="access verified"); state["revision"] += 1
        write("goal", state)
        assert call()["runnable"] == ["a"]
        state["requirements"]["a"].update(status="done", evidence="build verified"); state["revision"] += 1
        write("goal", state)
        assert call()["all_requirements_done"], "requirement evidence did not project"
        corrupt = copy.deepcopy(state); del corrupt["requirements"]["a"]["evidence"]
        write("corrupt", corrupt); call("session_id=corrupt", expected=409)
        legacy = copy.deepcopy(state); del legacy["ledger_id"]
        legacy_path = write("legacy", legacy)
        before = legacy_path.read_bytes()
        assert "ledger_id" not in call("session_id=legacy")["state"]
        assert legacy_path.read_bytes() == before, "read migrated legacy state"
        outside = root / "outside"; outside.mkdir()
        (sessions / "session_escape").symlink_to(outside, target_is_directory=True)
        call("session_id=escape", expected=409)
        linked = sessions / "session_linked" / "work"; linked.mkdir(parents=True)
        (linked / "work.sqlite").symlink_to(path)
        call("session_id=linked", expected=409)
        (path.parent / "work.sqlite-wal").symlink_to(root / "not-created")
        call(expected=409)
        assert not (root / "not-created").exists()
        print("session work API: auth, empty read, scoped progress, evidence, legacy nonmutation and symlink guards passed")
    finally:
        process.terminate(); process.wait(timeout=10); log.close()
