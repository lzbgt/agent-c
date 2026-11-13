"""Provider-free CLI contract checks against a local deterministic model stub."""
import concurrent.futures
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

agent = str(Path(sys.argv[1]).resolve())
requests = []


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(req)
        last = next(m["content"] for m in reversed(req["messages"]) if m["role"] == "user")
        message = {"role": "assistant", "content": f"verified:{last}"}
        if "TOOL_RESULT_BUDGET" in str(last) and not any(m["role"] == "tool" for m in req["messages"]):
            message = {"role": "assistant", "content": "", "tool_calls": [{
                "id": "read-budget-fixture", "type": "function",
                "function": {"name": "fs_read", "arguments": json.dumps({
                    "path": "large-result.txt", "max_chars": 20000, "max_lines": 2})},
            }]}
        if "OUTPUT_LIMIT" in str(last):
            message["content"] = "" if "THINKING" in str(last) else "incomplete answer"
            if "TOOL" in str(last):
                message["tool_calls"] = [{"id": "truncated-call", "type": "function",
                    "function": {"name": "work_state", "arguments": json.dumps({
                        "op": "requirement", "id": "truncated-effect", "title": "MUST NOT EXECUTE"})}}]
            if req.get("stream"):
                delta = {"content": message["content"]}
                if "tool_calls" in message:
                    delta["tool_calls"] = [dict(message["tool_calls"][0], index=0)]
                chunks = [
                    {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "length"}]},
                ]
                body = ("".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks) +
                        "data: [DONE]\n\n").encode()
                content_type = "text/event-stream"
            else:
                body = json.dumps({"choices": [{"message": message, "finish_reason": "length"}]}).encode()
                content_type = "application/json"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if any(m["role"] == "user" and "AUTONOMY_SENTINEL" in str(m["content"]) for m in req["messages"]):
            tools = sum(m["role"] == "tool" for m in req["messages"])
            stall = any(m["role"] == "user" and "STALL" in str(m["content"]) for m in req["messages"])
            key = "stalled" if stall else "verified-work"
            if tools == 0 or (tools == 1 and req["messages"][-1]["role"] == "user" and not stall):
                args = {"op": "requirement", "id": key, "title": "Verify autonomous continuation"} if tools == 0 else {"op":"requirement", "id":key, "status":"done", "evidence":"Local deterministic check passes"}
                message = {"role":"assistant", "content":"", "tool_calls":[{"id":f"call-{tools}", "type":"function", "function":{"name":"work_state", "arguments":json.dumps(args)}}]}
            else:
                message["content"] = "verified:completed" if tools >= 2 else "premature completion"
        body = json.dumps({"choices": [{"message": message, "finish_reason": "stop"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(("127.0.0.1", 0), Model)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix="agent-cli-harness-") as tmp:
        root = Path(tmp)
        (root / "AGENTS.md").write_text("PROJECT_SENTINEL: verify before claiming completion\n")
        options = ["--api-key", "local-test", "--base-url", f"http://127.0.0.1:{server.server_port}/v1", "--model", "stub", "--quiet", "--session-root", str(root / "sessions")]

        def run(*args, input=None, expected=0):
            result = subprocess.run([agent, *args], input=input, text=True, capture_output=True, timeout=20)
            assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
            return result

        help_output = run("--help")
        assert "--max-tool-result-chars" in help_output.stdout + help_output.stderr
        (root / "large-result.txt").write_text("result-line: " + "R" * 9000 + "TOOL_RESULT_TAIL_MARKER\n")
        tool_result_lengths = []
        for cap in (None, 24000, 0):
            run("exec", "TOOL_RESULT_BUDGET", "-C", tmp, "--no-session", "--tools", "host",
                "--max-chars", "100000", "--max-steps", "3",
                *(["--max-tool-result-chars", str(cap)] if cap is not None else []), *options)
            tool_result = next(m["content"] for m in requests[-1]["messages"] if m["role"] == "tool")
            tool_result_lengths.append(len(tool_result))
            if cap is None:
                assert len(tool_result) <= 12000, "standalone default result cap changed"
            else:
                assert 12000 < len(tool_result) < 24000 and "TOOL_RESULT_TAIL_MARKER" in tool_result, "explicit result budget did not reach the tool loop"
        assert tool_result_lengths[1] == tool_result_lengths[2], "zero must retain the unlimited result option"
        run("exec", "missing result budget", "--max-tool-result-chars", expected=2)
        assert "verified:stdin goal" in run("exec", "-", "--no-session", "--tools", "none", *options, input="stdin goal").stdout
        assert "max_tokens" not in requests[-1], "default provider budget must remain unchanged"
        for mode in ("none", "host"):
            run("exec", "bounded output", "-C", tmp, "--no-session", "--tools", mode,
                "--max-output-tokens", "8192", *options)
            assert requests[-1].get("max_tokens") == 8192, "output cap must reach the real provider request"
        run("exec", "provider default", "--no-session", "--tools", "none",
            "--max-output-tokens", "0", *options)
        assert "max_tokens" not in requests[-1]
        before_invalid = len(requests)
        for invalid in ("", "-1", "8x", "9223372036854775808"):
            run("exec", "invalid cap", "--max-output-tokens", invalid, *options, expected=2)
        run("exec", "missing cap", "--max-output-tokens", expected=2)
        assert len(requests) == before_invalid, "invalid budgets must fail before a provider call"
        limited_final = root / "limited-final.txt"
        for mode, stream, suffix in (
            ("none", False, "PARTIAL"), ("host", False, "PARTIAL"),
            ("host", False, "THINKING"), ("host", False, "TOOL"),
            ("none", True, "PARTIAL"), ("host", True, "PARTIAL"),
            ("host", True, "THINKING"), ("host", True, "TOOL"),
        ):
            limited_final.write_text("previous verified answer")
            before = len(requests)
            result = run("exec", "OUTPUT_LIMIT_" + suffix, "-C", tmp, "--no-session",
                         "--tools", mode, "--max-output-tokens", "8", "--json",
                         "-o", str(limited_final), *(["--stream-assistant"] if stream else []),
                         *options, expected=1)
            events = [json.loads(line) for line in result.stdout.splitlines()]
            assert events[-1]["type"] == "run.failed"
            assert not any(e["type"] == "assistant.completed" for e in events)
            assert limited_final.read_text() == "previous verified answer"
            assert len(requests) == before + 1, "output exhaustion must not silently retry"
            assert requests[-1].get("max_tokens") == 8
        ledger = json.loads(run("work", "-C", tmp).stdout)
        assert "truncated-effect" not in json.dumps(ledger), "truncated tool calls executed"
        run("exec", "first goal", "-C", tmp, "--session", "durable", *options)
        assert any("PROJECT_SENTINEL" in str(m["content"]) for m in requests[-1]["messages"])
        assert any("without permission prompts" in str(m["content"]) for m in requests[-1]["messages"])
        assert "durable" in run("sessions", "--session-root", str(root / "sessions")).stdout
        assert "verified:next goal" in run("resume", "durable", "next goal", "--tools", "none", *options).stdout
        assert any(m["role"] == "user" and m["content"] == "first goal" for m in requests[-1]["messages"])
        run("resume", "missing", "continue", *options, expected=2)
        run("resume", "durable", "--no-session", expected=2)
        run("exec", "goal", "--typo", expected=2)
        run("exec", "-", input="", expected=2)
        first_request = len(requests)
        result = run("exec", "AUTONOMY_SENTINEL", "-C", tmp, "--no-session", "--max-steps", "8", *options)
        assert "verified:completed" in result.stdout and "premature completion" not in result.stdout
        assert len(requests) - first_request == 4, "proposed completion must continue with the original turn"
        run("exec", "AUTONOMY_SENTINEL STALL", "-C", tmp, "--no-session", "--max-steps", "8", *options, expected=1)
        json_root = root / "json-work"
        json_root.mkdir()
        final_file = root / "verified.txt"
        final_file.write_text("previous verified result")
        result = run("exec", "AUTONOMY_SENTINEL STALL", "-C", str(json_root), "--no-session", "--max-steps", "8", "--json", "-o", str(final_file), *options, expected=1)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        assert events[-1]["type"] == "run.failed" and not any(e["type"] == "assistant.completed" for e in events)
        assert final_file.read_text() == "previous verified result", "premature completion replaced a verified artifact"
        work = root / "work.sqlite"
        def update(i):
            payload = {"op": "requirement", "id": f"task{i}", "title": f"Independent requirement {i}"}
            return run("work", json.dumps(payload), "--file", str(work))
        # Independent CLI processes must serialize writes without losing updates.
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(update, range(12)))
        state = json.loads(run("work", "--file", str(work)).stdout)
        assert len(state["runnable"]) == 12 and state["state"]["revision"] == 12
finally:
    server.shutdown()
    server.server_close()
print("CLI harness contracts passed")
