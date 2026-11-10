"""Real CLI JSONL/stream/artifact contracts against a local model service."""
import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

agent = str(Path(sys.argv[1]).resolve())
requests = []
publish_conflict = None
long_text = "0123456789abcdef" * 4 * 9500
large_text = "capture-budget-" * 100000
envelope = json.loads((Path(__file__).resolve().parents[1] / "docs" / "cli_event_v1.schema.json").read_text())


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(req)
        last = next(m["content"] for m in reversed(req["messages"]) if m["role"] == "user")
        if "FAIL" == last:
            self.send_response(400)
            body = b'{"error":{"message":"deterministic failure"}}'
        elif req.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            text = long_text if last == "LONG_STREAM" else 'stream: quoted " text\nUnicode: \u4f60\u597d'
            for offset in range(0, len(text), 64):
                chunk = {"choices": [{"index": 0, "delta": {"content": text[offset:offset+64]}}]}
                self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
            self.wfile.flush()
            return
        else:
            self.send_response(200)
            message = {"role": "assistant", "content": large_text if last == "LARGE_CAPTURE" else 'verified: "quoted"\nUnicode: \u4f60\u597d'}
            if last == "CALCULATE" and not any(m["role"] == "tool" for m in req["messages"]):
                message = {"role": "assistant", "content": "", "tool_calls": [{"id": "calc-1", "type": "function", "function": {"name": "calculator", "arguments": '{"expression":"2+2"}'}}]}
            if last == "PUBLISH_CONFLICT":
                publish_conflict.mkdir()
            body = json.dumps({"choices": [{"message": message, "finish_reason": "stop"}]}).encode()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(("127.0.0.1", 0), Model)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix="agent-cli-machine-") as tmp:
        root = Path(tmp)
        options = ["--api-key", "local-test", "--base-url", f"http://127.0.0.1:{server.server_port}/v1", "--model", "stub", "--quiet", "--session-root", str(root / "sessions")]

        def run(*args, expected=0):
            result = subprocess.run([agent, *args, *options], text=True, capture_output=True, timeout=30)
            assert result.returncode == expected, (args, result.returncode, result.stdout[-2000:], result.stderr)
            return result

        def events(result, success=True):
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            assert rows and all(row["schema"] == "agentd.cli.event.v1" for row in rows)
            assert all(set(row) == set(envelope["required"]) and isinstance(row["data"], dict) and re.fullmatch(envelope["properties"]["type"]["pattern"], row["type"]) for row in rows)
            assert [row["sequence"] for row in rows] == list(range(1, len(rows)+1))
            terminal = [r for r in rows if r["type"] in ("run.completed", "run.failed")]
            assert len(terminal) == 1 and terminal[0] is rows[-1]
            assert terminal[0]["type"] == ("run.completed" if success else "run.failed")
            assert terminal[0]["data"]["exit_code"] == result.returncode
            assert len([r for r in rows if r["type"] == "assistant.completed"]) == int(success)
            assert not any(r["type"] in ("loop.llm_request", "loop.llm_response") for r in rows)
            return rows

        target = root / "final.txt"
        target.write_text("previous verified artifact")
        rows = events(run("exec", "inspect", "--json", "--tools", "none", "-C", tmp, "-o", "final.txt"))
        assert rows[0]["type"] == "run.started" and rows[0]["data"]["cwd"] == tmp
        assert rows[0]["data"]["session_id"] == "default"
        assert target.read_text() == rows[-2]["data"]["text"]
        assert rows[-1]["data"]["last_message_file"] == str(target)
        events(run("resume", "default", "continue", "--json", "--tools", "none"))
        rows = events(run("exec", "CALCULATE", "--json", "--tools", "basic", "--no-session"))
        assert any(r["type"] == "loop.tool_result" for r in rows)
        assert rows[-1]["data"]["usage"]["tool_calls_executed"] == 1
        for tools in ("none", "basic"):
            rows = events(run("exec", "STREAM", "--json", "--stream-assistant", "--tools", tools, "--no-session"))
            assert "".join(r["data"]["delta"] for r in rows if r["type"] == "assistant.delta") == rows[-2]["data"]["text"]

        # More than 4,000 live events must not be silenced by the bounded audit
        # capture. The accepted final result still follows all streamed deltas.
        rows = events(run("exec", "LONG_STREAM", "--json", "--stream-assistant", "--tools", "basic", "--session", "long"))
        received = "".join(r["data"]["delta"] for r in rows if r["type"] == "assistant.delta")
        assert received == long_text, (len(received), len(long_text), len(rows), len(rows[-2]["data"]["text"]), received[:120])
        capture = rows[-1]["data"]["capture"]
        assert capture["truncated"] and capture["events"] <= 4000
        assert capture["bytes"] <= 2 * 1024 * 1024
        # A few large events exercise the byte cap separately from event count.
        rows = events(run("exec", "LARGE_CAPTURE", "--json", "--tools", "basic", "--no-session"))
        capture = rows[-1]["data"]["capture"]
        assert rows[-2]["data"]["text"] == large_text
        assert capture["truncated"] and capture["events"] < 4000 and capture["bytes"] <= 2 * 1024 * 1024

        previous = target.read_text()
        events(run("exec", "FAIL", "--json", "--tools", "none", "-o", str(target), expected=1), False)
        assert target.read_text() == previous
        count = len(requests)
        events(run("exec", "inspect", "--json", "--tools", "none", "-o", str(root / "missing" / "file"), expected=2), False)
        assert len(requests) == count, "invalid artifact destination reached the provider"
        events(run("exec", "inspect", "--json", "--typo", expected=2), False)
        events(run("resume", "default", "--json", expected=2), False)
        events(run("exec", "inspect", "--json", "--json", expected=2), False)
        events(run("exec", "inspect", "--json", "-o", expected=2), False)

        publish_conflict = root / "became-a-directory"
        result = run("exec", "PUBLISH_CONFLICT", "--json", "--tools", "none", "-o", str(publish_conflict), expected=1)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        assert rows[-1]["type"] == "run.failed" and rows[-1]["data"]["error"]["code"] == "output_write_failed"
        assert publish_conflict.is_dir() and not list(root.glob("*.agent-tmp-*"))
        plain = run("exec", "inspect", "--tools", "none", "-o", str(target))
        assert plain.stdout == target.read_text() + "\n"
        attachment = root / "note.txt"
        attachment.write_text("attachment evidence")
        events(run("exec", "inspect", "--json", "--tools", "none", "--no-session", "--attach", str(attachment)))
finally:
    server.shutdown()
    server.server_close()
print("CLI machine output contracts passed")
