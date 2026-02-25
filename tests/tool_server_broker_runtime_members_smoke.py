#!/usr/bin/env python3
import json
import os
import socketserver
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler

TEAM_ID = "team-alpha"
RUN_ID = "run-123"


class Handler(BaseHTTPRequestHandler):
    def do_PATCH(self):  # noqa: N802
        if self.path != f"/v1/teams/{TEAM_ID}/runs/{RUN_ID}/runtime_members":
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b"{}"
        try:
            payload = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError:
            payload = {}
        response = {
            "ok": True,
            "team_id": TEAM_ID,
            "team_run_id": RUN_ID,
            "status": "succeeded",
            "members": [],
            "runtime_members": payload.get("runtime_members", []),
        }
        out = json.dumps(response).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, format, *args):  # noqa: A003
        return


def read_line(proc):
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("tool server exited without response")
    return json.loads(line)


def main():
    with socketserver.TCPServer(("127.0.0.1", 0), Handler) as httpd:
        port = httpd.server_address[1]
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()

        env = os.environ.copy()
        env["BROKER_BASE_URL"] = f"http://127.0.0.1:{port}"
        cmd = [sys.executable, "-u", "tools/tool_server_broker_runtime_members.py"]
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            text=True,
        )
        try:
            proc.stdin.write(json.dumps({"id": 1, "op": "manifest"}) + "\n")
            proc.stdin.flush()
            manifest = read_line(proc)
            if not manifest.get("ok"):
                raise RuntimeError("manifest failed")

            args = {
                "team_id": TEAM_ID,
                "team_run_id": RUN_ID,
                "mode": "merge",
                "runtime_members": [
                    {"member_id": "rt-1", "agent_id": "agent-a", "role": "executor"},
                    {"member_id": "rt-2", "agent_id": "agent-b", "role": "reviewer"},
                ],
            }
            req = {"id": 2, "op": "execute", "tool_name": "broker_team_runtime_members_update", "arguments": args}
            proc.stdin.write(json.dumps(req) + "\n")
            proc.stdin.flush()
            resp = read_line(proc)
            if not resp.get("ok"):
                raise RuntimeError(f"tool call failed: {resp}")
            result = resp.get("tool_result", {})
            if not result.get("ok"):
                raise RuntimeError(f"tool_result not ok: {result}")
            data = result.get("data", {})
            if data.get("team_id") != TEAM_ID or data.get("team_run_id") != RUN_ID:
                raise RuntimeError("unexpected response payload")
            runtime = data.get("runtime_members", [])
            if len(runtime) != 2:
                raise RuntimeError("runtime_members length mismatch")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    return 0


if __name__ == "__main__":
    sys.exit(main())
