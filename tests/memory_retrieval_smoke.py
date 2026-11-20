"""Real daemon current/historical reads and recap revision fences; local synthetic provider only."""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with tempfile.TemporaryDirectory(prefix="memory-retrieval-") as temporary:
    root = Path(temporary)
    memory = root / "state" / "memory"
    memory.mkdir(parents=True)
    source = memory / "STRUCTURED.md"
    def publish(value):
        doc = {"schema":"agent_memory_v2", "items":{
            "current": {"kind":"fact", "status":"active", "value":value},
            "retired": {"kind":"fact", "status":"deprecated", "value":"retiredmustneverenterprompt"}}}
        stage = memory / "source.stage"
        stage.write_text("<!-- AGENT_MEMORY_V1_BEGIN -->\n" + json.dumps(doc) + "\n<!-- AGENT_MEMORY_V1_END -->\n")
        stage.replace(source)
    publish("current authoritative value")
    expected = "sha256:" + hashlib.sha256(source.read_bytes()).hexdigest()
    checkpoints = memory / "checkpoints"
    checkpoints.mkdir()
    (checkpoints / "structured_fixture.json").write_text(json.dumps({"path":"STRUCTURED.md", "ts_utc":"2026-01-01T00:00:00Z",
        "doc":{"items":{"current":{"kind":"fact", "status":"active", "value":"older checkpoint value"}}}}))
    mutate = False
    calls = []
    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass
        def do_POST(self):
            assert self.path == "/v1/chat/completions", self.path
            size = int(self.headers.get("Content-Length", "0"))
            assert 0 < size <= 128000
            request = json.loads(self.rfile.read(size))
            text = json.dumps(request)
            assert "retiredmustneverenterprompt" not in text and "older checkpoint value" not in text
            calls.append(request)
            if mutate:
                publish("correction during provider response")
            reply = {"id":"local-fixture", "model":"stub", "choices":[{"index":0, "message":{"role":"assistant", "content":"{\"notes\":\"fixture recap\"}"}, "finish_reason":"stop"}],
                     "usage":{"prompt_tokens":8,"completion_tokens":4,"total_tokens":12}}
            raw = json.dumps(reply).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
    provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    def call(path, payload=None):
        req = urllib.request.Request(base + path, data=None if payload is None else json.dumps(payload).encode(),
                                     headers={"Content-Type":"application/json"})
        with opener.open(req, timeout=8) as response:
            return json.load(response)
    environment = {"PATH":os.environ.get("PATH", "/usr/bin:/bin"), "AGENT_DOTENV_PATH":os.devnull, "DOTENV_PATH":os.devnull}
    with (root / "daemon.log").open("w+") as log:
        daemon = subprocess.Popen([binary, "--host","127.0.0.1", "--port",str(port), "--state-dir",str(root / "state"),
            "--db-path",str(root / "agentd.sqlite"), "--base-url",f"http://127.0.0.1:{provider.server_port}/v1",
            "--api-key","synthetic-fixture-key", "--model","stub"], cwd=root, env=environment, stdout=log, stderr=log)
        try:
            end = time.monotonic() + 10
            while time.monotonic() < end:
                assert daemon.poll() is None, "daemon stopped"
                try:
                    if call("/api/v1/health").get("ok"):
                        break
                except (OSError, urllib.error.URLError):
                    time.sleep(.025)
            else:
                raise AssertionError("daemon health deadline")
            current = call("/api/v1/memory/query")
            assert current["view"] == "current" and current["revision"] == expected, current
            assert [entry["record"]["value"] for entry in current["entries"]] == ["current authoritative value"], current
            historical = call("/api/v1/memory/query?view=historical")
            assert historical["entries"][0]["record"]["value"] == "older checkpoint value", historical
            assert historical["entries"][0]["historical"] and historical["entries"][0]["evidence"]["span"]["kind"] == "json_pointer"
            successful = call("/api/v1/memory/recaps", {"model":"stub", "write_file":True, "include_daily":False})
            assert successful["ok"] and len(calls) == 1, successful
            written = list((memory / "recaps").glob("recap_*.json"))
            assert len(written) == 1
            receipt = json.loads(written[0].read_text())
            assert any(entry["path"] == "STRUCTURED.md" and entry["revision"] == expected for entry in receipt["input_revisions"]), receipt
            assert receipt["structured_items"][0]["evidence"]["revision"] == expected, receipt
            before = {path.name: path.read_bytes() for path in written}
            mutate = True
            try:
                call("/api/v1/memory/recaps", {"model":"stub", "write_file":True, "include_daily":False})
                raise AssertionError("stale recap published")
            except urllib.error.HTTPError as error:
                failure = json.load(error)
                assert not failure["ok"] and "changed" in failure["error"], failure
            assert len(calls) == 2, "provider call replayed"
            assert before == {path.name: path.read_bytes() for path in (memory / "recaps").glob("recap_*.json")}, "stale response changed recap files"
        finally:
            daemon.terminate()
            daemon.wait(timeout=8)
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=2)
print("Real API current/historical authority and recap correction fence passed; two local synthetic calls, no replay")
