"""Real bounded snapshot pages, revision conflicts, restart and deletion fences."""
import json, os, socket, sqlite3, subprocess, sys, tempfile, time, urllib.request, urllib.error
from pathlib import Path
binary = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
base = f"http://127.0.0.1:{port}"
def call(path, data=None, status=200, auth=True, method=None):
    req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(), method=method,
        headers={"Content-Type": "application/json", **({"Authorization": "Bearer paging-test"} if auth else {})})
    try:
        with opener.open(req, timeout=5) as r: code, result = r.status, json.load(r)
    except urllib.error.HTTPError as e: code, result = e.code, json.load(e)
    assert code == status, (code, result)
    return result
def page(extra="", **kwargs):
    return call("/api/v1/session?session_id=history&messages_limit=73" + extra, **kwargs)
with tempfile.TemporaryDirectory(prefix="session-pages-") as root:
    db = str(Path(root) / "state.db")
    log = open(Path(root) / "daemon.log", "w")
    def start():
        p = subprocess.Popen([binary, "--host", "127.0.0.1", "--port", str(port), "--state-dir", root,
            "--db-path", db, "--auth-token", "paging-test", "--tools", "none", "--no-yolo"],
            stdout=log, stderr=log, env={**os.environ, "AGENTD_DOTENV_PATH": "/dev/null"})
        for _ in range(200):
            try:
                if call("/api/v1/health")["ok"]: return p
            except OSError: pass
            time.sleep(.025)
        raise AssertionError("daemon not ready")
    process = start()
    try:
        page(status=404)
        call("/api/v1/session/new", {"session_id": "history"})
        empty = page()
        assert empty["messages"] == [] and empty["page"]["has_more"] is False
        with sqlite3.connect(db) as c:
            c.executemany("INSERT INTO messages(session_id,idx,role,content,mm_json,mm_bytes,mm_truncated,created_unix_ms) VALUES('history',?,'user',?,'',0,0,1)",
                [(i, "message-" + str(i)) for i in range(501)])
            c.execute("UPDATE messages SET mm_json=?,mm_bytes=19 WHERE session_id='history' AND idx=500", ('{"image":"fixture"}',))
            c.execute("UPDATE session_revisions SET revision=revision+1 WHERE session_id='history'")
        first = page()
        assert [m["index"] for m in first["messages"]] == list(range(428, 501))
        assert first["messages"][-1]["mm_json"] == '{"image":"fixture"}'
        revision = first["page"]["revision"]
        cursor = "&messages_before_index=428&messages_revision=" + str(revision)
        process.kill(); process.wait(); process = start()
        assert page()["page"]["revision"] == revision
        pages = [first]; current = first
        while current["page"]["has_more"]:
            current = page("&messages_before_index=" + str(current["page"]["next_before_index"]) + "&messages_revision=" + str(revision))
            assert current["page"]["revision"] == revision and len(current["messages"]) <= 73
            pages.append(current)
        assert [m["index"] for p in reversed(pages) for m in p["messages"]] == list(range(501))
        assert len(call("/api/v1/session?session_id=history")["messages"]) == 501
        # Concurrent commits from a second connection must never mix rows and revision.
        import threading
        stop = threading.Event()
        published = threading.Event()
        writer_errors = []
        def writer():
            try:
                with sqlite3.connect(db, timeout=5) as c:
                    while not stop.is_set():
                        c.execute("BEGIN IMMEDIATE")
                        r = c.execute("SELECT revision FROM session_revisions WHERE session_id='history'").fetchone()[0] + 1
                        c.execute("DELETE FROM messages WHERE session_id='history'")
                        c.executemany("INSERT INTO messages(session_id,idx,role,content,mm_json,mm_bytes,mm_truncated,created_unix_ms) VALUES('history',?,'user',?,'',0,0,1)",
                            [(i, "revision-" + str(r)) for i in range(15)])
                        c.execute("UPDATE session_revisions SET revision=? WHERE session_id='history'", (r,))
                        c.commit(); published.set(); time.sleep(.002)
            except Exception as error:
                writer_errors.append(error); published.set()
        thread = threading.Thread(target=writer)
        thread.start()
        try:
            assert published.wait(5)
            for _ in range(30):
                current = page()
                assert len(current["messages"]) == 15
                assert all(m["content"] == "revision-" + str(current["page"]["revision"]) for m in current["messages"]), current
        finally:
            stop.set(); thread.join(timeout=5)
        assert not thread.is_alive() and not writer_errors, writer_errors
        page(auth=False, status=401)
        for query in ["messages_limit=0", "messages_limit=201", "messages_limit=-1", "messages_limit=1.5",
                      "messages_limit=73&messages_before_index=5", "messages_limit=73&messages_revision=abc",
                      "messages_limit=73&messages_revision=9007199254740992", "messages_before_index=0"]:
            call("/api/v1/session?session_id=history&" + query, status=400)
        # Actual public append advances the conversation generation.
        call("/api/v1/session/ui_event", {"session_id": "history", "type": "paging-test", "append_to_session": True})
        page(cursor, status=409)
        newer = page()["page"]["revision"]
        assert newer > revision
        call("/api/v1/session?session_id=history", method="DELETE")
        page(cursor, status=404)
        call("/api/v1/session/new", {"session_id": "history"})
        page(cursor, status=409)
        assert page()["page"]["revision"] > newer
        # Byte cap yields a continuation without silently truncating individual content.
        with sqlite3.connect(db) as c:
            c.executemany("INSERT INTO messages(session_id,idx,role,content,mm_json,mm_bytes,mm_truncated,created_unix_ms) VALUES('history',?,'user',?,'',0,0,1)",
                [(i, "x" * 1100000) for i in range(3)])
            c.execute("UPDATE session_revisions SET revision=revision+1 WHERE session_id='history'")
        limited = page()
        assert len(limited["messages"]) == 1 and limited["page"]["has_more"]
        assert len(limited["messages"][0]["content"]) == 1100000
        with sqlite3.connect(db) as c:
            c.execute("UPDATE messages SET content=? WHERE session_id='history' AND idx=2", ("x" * 2100000,))
            c.execute("UPDATE session_revisions SET revision=revision+1 WHERE session_id='history'")
        page(status=413)
        print("Bounded paged history passed: 501 messages without gaps/duplicates, restart, auth, revision/edit/delete/recreate fences, media and byte limits; legacy unchanged.")
    finally:
        process.terminate(); process.wait(timeout=5); log.close()
