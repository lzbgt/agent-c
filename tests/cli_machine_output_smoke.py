"""Real CLI JSONL/stream/artifact contracts against a local model service."""
import ctypes
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
output_cases = {
    "EMPTY_OUTPUT": {"content": ""},
    "BLANK_OUTPUT": {"content": " \t\r\n"},
    "REASONING_ONLY_OUTPUT": {"content": None, "reasoning_content": "fixture, not a reply"},
    "PADDED_OUTPUT": {"content": " \nFixture answer\t\n"},
}
envelope = json.loads((Path(__file__).resolve().parents[1] / "docs" / "cli_event_v1.schema.json").read_text())


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(req)
        last = next(m["content"] for m in reversed(req["messages"]) if m["role"] == "user")
        # Attachments can wrap the user message in content parts.
        prompt_text = last if isinstance(last, str) else json.dumps(last)
        output = next((v for key, v in output_cases.items() if key in prompt_text), None)
        fallback = last == "JSON_FALLBACK_STREAM" or prompt_text.endswith(":fallback")
        if "FAIL" == last:
            self.send_response(400)
            body = b'{"error":{"message":"deterministic failure"}}'
        elif req.get("stream") and not fallback:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            text = long_text if last == "LONG_STREAM" else 'stream: quoted " text\nUnicode: \u4f60\u597d'
            deltas = (output,) if output is not None else (
                {"content": text[offset:offset+64]} for offset in range(0, len(text), 64)
            )
            for delta in deltas:
                chunk = {"choices": [{"index": 0, "delta": delta}]}
                self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
            if last != "INCOMPLETE_STREAM":
                reason = " " if last == "BLANK_FINISH_STREAM" else "stop"
                finish = {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]}
                self.wfile.write(("data: " + json.dumps(finish) + "\n\n").encode())
            if last not in {"INCOMPLETE_STREAM", "BLANK_FINISH_STREAM", "FINISH_ONLY_STREAM"}:
                self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
            return
        else:
            self.send_response(200)
            message = {"role": "assistant", "content": large_text if last == "LARGE_CAPTURE" else 'verified: "quoted"\nUnicode: \u4f60\u597d'}
            if output is not None:
                message = {"role": "assistant", **output}
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
            opts = ("--json", "--stream-assistant", "--tools", tools, "--no-session")
            for prompt in ("STREAM", "FINISH_ONLY_STREAM"):
                rows = events(run("exec", prompt, *opts))
                assert "".join(r["data"]["delta"] for r in rows if r["type"] == "assistant.delta") == rows[-2]["data"]["text"]
            rows = events(run("exec", "JSON_FALLBACK_STREAM", *opts))
            assert rows[-2]["data"]["text"].startswith("verified:")

            previous = target.read_text()
            for prompt in ("INCOMPLETE_STREAM", "BLANK_FINISH_STREAM"):
                count = len(requests)
                rows = events(run("exec", prompt, *opts, "-o", str(target), expected=1), False)
                assert any(r["type"] == "assistant.delta" for r in rows)
                assert "completion marker" in rows[-1]["data"]["error"]["message"], (tools, prompt, rows[-1])
                assert target.read_text() == previous
                assert len(requests) == count + 1, "partial streamed requests must not be replayed"

        # A valid completion without an answer is not a successful run or file publication.
        for tools in ("none", "basic"):
            for mode in ("raw", "sse", "fallback"):
                opts = ("--json", "--tools", tools, "--no-session", "-o", str(target))
                if mode != "raw":
                    opts += ("--stream-assistant",)
                suffix = ":fallback" if mode == "fallback" else ""
                previous = target.read_bytes()
                for prompt in ("EMPTY_OUTPUT", "BLANK_OUTPUT", "REASONING_ONLY_OUTPUT"):
                    count = len(requests)
                    rows = events(run("exec", prompt + suffix, *opts, expected=1), False)
                    assert "without an assistant answer" in rows[-1]["data"]["error"]["message"], (tools, mode, prompt, rows[-1])
                    assert target.read_bytes() == previous
                    assert len(requests) == count + 1, "completed-but-empty requests must not be replayed"
                rows = events(run("exec", "PADDED_OUTPUT" + suffix, *opts))
                text = output_cases["PADDED_OUTPUT"]["content"]
                assert rows[-2]["data"]["text"] == text and target.read_bytes() == text.encode()

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

        # Bad numeric limits are caller errors, not adjusted/defaulted limits on a real request.
        too_long_for_timeout = str(1 << (8 * ctypes.sizeof(ctypes.c_long) - 1))
        bad_numeric = (
            ("--timeout-ms", "2500ms"),
            ("--timeout-ms", too_long_for_timeout),
            ("--max-steps", "-1"),
            ("--keep-last", " 3"),
            ("--max-tool-calls-total", ""),
            ("--max-tool-calls-per-tool", "+3"),
            ("--max-output-tokens", str(1 << 63)),
            ("--tool-call-limit", "calculator=2calls"),
        )
        previous = target.read_bytes()
        count = len(requests)
        for flag, value in bad_numeric:
            rows = events(run("exec", "inspect", "--json", "--tools", "none", "-o", str(target),
                              flag, value, expected=2), False)
            assert not any(r["type"] == "run.started" for r in rows), (flag, value, rows)
            assert flag in rows[-1]["data"]["error"]["message"], (flag, value, rows[-1])
        assert len(requests) == count and target.read_bytes() == previous
        events(run("exec", "inspect", "--json", "--tools", "none", "--no-session",
                   "--timeout-ms", "2500", "--max-steps", "0", "--max-output-tokens", "16",
                   "--tool-call-limit", "calculator=2"))
        assert len(requests) == count + 1
        assert any(requests[-1].get(key) == 16 for key in ("max_tokens", "max_completion_tokens"))

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
        previous = target.read_bytes()
        rows = events(run("exec", "BLANK_OUTPUT", "--json", "--tools", "none", "--no-session",
                          "--attach", str(attachment), "-o", str(target), expected=1), False)
        assert "without an assistant answer" in rows[-1]["data"]["error"]["message"]
        assert target.read_bytes() == previous
finally:
    server.shutdown()
    server.server_close()
print("CLI machine output contracts passed")
