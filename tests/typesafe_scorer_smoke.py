#!/usr/bin/env python3
"""Synthetic loopback only. No real provider key or public network request."""
import http.server
import json
import subprocess
import sys
import threading
import time


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        assert 0 < size <= 32768
        payload = json.loads(self.rfile.read(size))
        if self.path == "/slow":
            time.sleep(1.5)
        body = b"x" * 70000 if self.path == "/oversize" else json.dumps({
            "model": "jev-1.14.0",
            "answers": {key: {"type": "noul", "noul": .8} for key in payload["questions"]},
            "usage": {"input_tokens": 120, "output_tokens": 12},
        }).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
worker = threading.Thread(target=server.serve_forever, daemon=True)
worker.start()
try:
    subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"], check=True, timeout=15)
finally:
    server.shutdown()
    server.server_close()
    worker.join()
