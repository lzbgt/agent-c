#!/usr/bin/env python3

import json
import sys


TOOLS = [
    {
        "name": "server_echo",
        "description": "Echoes input text (tool-server stub).",
        "parameters": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
            "additionalProperties": False,
        },
    }
]

PING_COUNT = 0


def main() -> int:
    global PING_COUNT
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            sys.stdout.write(json.dumps({"ok": False, "error": f"invalid json: {e}"}) + "\n")
            sys.stdout.flush()
            continue

        rid = req.get("id")
        op = req.get("op")
        if op == "manifest":
            resp = {"id": rid, "ok": True, "tools": TOOLS}
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
            continue

        if op == "ping":
            PING_COUNT += 1
            resp = {"id": rid, "ok": True, "pong": True}
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
            continue

        if op == "execute":
            name = req.get("tool_name")
            args = req.get("arguments") if isinstance(req.get("arguments"), dict) else {}
            if name != "server_echo":
                resp = {"id": rid, "ok": False, "error": "unknown tool", "tool_result": {"ok": False, "error": "unknown tool"}}
                sys.stdout.write(json.dumps(resp) + "\n")
                sys.stdout.flush()
                continue

            text = args.get("text")
            if not isinstance(text, str):
                tr = {"ok": False, "error": "missing text"}
                resp = {"id": rid, "ok": False, "error": "missing text", "tool_result": tr}
                sys.stdout.write(json.dumps(resp) + "\n")
                sys.stdout.flush()
                continue

            tool_result = {"ok": True, "data": {"echo": text, "ping_count": PING_COUNT}}
            resp = {"id": rid, "ok": True, "tool_result": tool_result}
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
            continue

        resp = {"id": rid, "ok": False, "error": "unknown op"}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
