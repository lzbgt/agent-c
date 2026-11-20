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
    provider_mode = "normal"
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
            assert request.get("max_tokens", request.get("max_completion_tokens",0)) <= 2048
            calls.append(request)
            if provider_mode == "oversized":
                raw = b"x" * (2 * 1024 * 1024)
                self.send_response(200); self.send_header("Content-Length",str(len(raw))); self.end_headers()
                try: self.wfile.write(raw)
                except (BrokenPipeError,ConnectionResetError): pass
                return
            if provider_mode == "unavailable":
                self.send_response(503); self.send_header("Content-Length","0"); self.end_headers(); return
            if mutate:
                publish("correction during provider response")
            reply = {"id":"local-fixture", "model":"stub", "choices":[{"index":0, "message":{"role":"assistant", "content":"{\"notes\":\"fixture recap\"}"}, "finish_reason":"length" if provider_mode == "limited" else "stop"}],
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
            manifest = receipt["derivation"]
            assert any(entry["path"] == "STRUCTURED.md" and entry["revision"] == expected for entry in manifest["input_revisions"]), receipt
            assert manifest["evidence"][0]["revision"] == expected, receipt
            assert receipt["model_response"] == '{"notes":"fixture recap"}' and receipt["returned_model"] == "stub" and receipt["response_capture_complete"], receipt
            assert successful["source_state"]["inputs_current"] and not successful["source_state"]["claims_verified"]
            current_recaps = call("/api/v1/memory/recaps?include_summary=true")
            assert len(current_recaps["recaps"]) == 1 and current_recaps["view"] == "current", current_recaps
            before = {path.name: path.read_bytes() for path in written}
            mutate = True
            try:
                call("/api/v1/memory/recaps", {"model":"stub", "write_file":True, "include_daily":False})
                raise AssertionError("stale recap reported complete")
            except urllib.error.HTTPError as error:
                failure = json.load(error)
                assert error.code == 409 and not failure["ok"] and not failure["complete"] and "changed" in failure["error"], failure
                assert failure["source_state"]["state"] == "stale" and failure["original_response_evidence"]["span"]["pointer"] == "/model_response", failure
            assert len(calls) == 2, "provider call replayed"
            assert all((memory / "recaps" / name).read_bytes() == data for name,data in before.items()), "earlier immutable result changed"
            assert len(list((memory / "recaps").glob("recap_*.json"))) == 2, "incomplete provider outcome lost"
            current_recaps = call("/api/v1/memory/recaps?include_summary=true")
            assert not current_recaps["recaps"] and current_recaps["validation"]["excluded_count"] == 2, current_recaps
            history = call("/api/v1/memory/recaps?view=historical&include_summary=true")
            assert len(history["recaps"]) == 2 and all(row["historical"] for row in history["recaps"]), history
            assert {row["source_state"]["state"] for row in history["recaps"]} == {"stale", "unverified"}, history
            assert len(calls) == 2, "reading or invalidating derived results replayed model call"
            mutate = False
            for provider_mode in ("oversized", "unavailable"):
                try:
                    call("/api/v1/memory/recaps", {"model":"stub", "write_file":True, "include_daily":False})
                    raise AssertionError("incomplete provider result reported success")
                except urllib.error.HTTPError as error:
                    failure = json.load(error)
                    assert error.code == 409 and not failure["complete"] and failure["provider_outcome"] == "unknown", failure
                    saved = json.loads((memory / failure["recap_path"]).read_text())
                    assert saved["generation_status"] == "incomplete" and saved["provider_outcome"] == "unknown", saved
                    if provider_mode == "oversized":
                        assert not saved["response_capture_complete"] and not saved["model_response"], saved
                        assert saved["response_capture_sha256"] == hashlib.sha256(b"x"*(1024*1024)).hexdigest(), saved
            assert len(calls) == 4, "budget/failure outcome replayed provider request"
            provider_mode = "limited"
            try:
                call("/api/v1/memory/recaps", {"model":"stub", "write_file":True, "include_daily":False})
                raise AssertionError("output-token limit reported complete")
            except urllib.error.HTTPError as error:
                failure = json.load(error)
                saved = json.loads((memory / failure["recap_path"]).read_text())
                assert error.code == 409 and not failure["complete"] and saved["model_response"] == '{"notes":"fixture recap"}', failure
                assert saved["provider_error"] and saved["generation_status"] == "incomplete", saved
            assert len(calls) == 5, "partial completion replayed"

            assert len(list((memory / "recaps").glob("recap_*.json"))) == 5


        finally:
            daemon.terminate()
            daemon.wait(timeout=8)
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=2)
print("Real API current/historical authority and recap correction fence passed; five local synthetic calls, no replay")
