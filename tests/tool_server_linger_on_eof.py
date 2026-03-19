#!/usr/bin/env python3

import json
import signal
import sys
import time


TOOLS = [
    {
        "name": "server_echo",
        "description": "Echoes input text (tool-server linger-on-eof stub).",
        "parameters": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
            "additionalProperties": False,
        },
    }
]


def _handle_term(_signum, _frame):
    raise SystemExit(0)


def _write(resp):
    sys.stdout.write(json.dumps(resp) + "\n")
    sys.stdout.flush()


def main() -> int:
    signal.signal(signal.SIGTERM, _handle_term)
    signal.signal(signal.SIGINT, _handle_term)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            _write({"ok": False, "error": f"invalid json: {e}"})
            continue

        rid = req.get("id")
        op = req.get("op")
        if op == "manifest":
            _write({"id": rid, "ok": True, "tools": TOOLS})
            continue

        if op == "ping":
            _write({"id": rid, "ok": True, "pong": True})
            continue

        if op == "execute":
            name = req.get("tool_name")
            args = req.get("arguments") if isinstance(req.get("arguments"), dict) else {}
            if name != "server_echo":
                _write({"id": rid, "ok": False, "error": "unknown tool", "tool_result": {"ok": False, "error": "unknown tool"}})
                continue

            text = args.get("text")
            if not isinstance(text, str):
                _write({"id": rid, "ok": False, "error": "missing text", "tool_result": {"ok": False, "error": "missing text"}})
                continue

            _write({"id": rid, "ok": True, "tool_result": {"ok": True, "data": {"echo": text}}})
            continue

        _write({"id": rid, "ok": False, "error": "unknown op"})

    # Deliberately linger after stdin closes so an ungraceful daemon death leaves a live child.
    while True:
        time.sleep(1)


if __name__ == "__main__":
    raise SystemExit(main())
