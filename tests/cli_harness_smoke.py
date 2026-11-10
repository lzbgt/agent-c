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

        run("--help")
        assert "verified:stdin goal" in run("exec", "-", "--no-session", "--tools", "none", *options, input="stdin goal").stdout
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
