#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "tool_server_esp32_bridge.py"


def write_line(proc, obj):
    proc.stdin.write(json.dumps(obj, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def read_line(proc):
    line = proc.stdout.readline()
    if not line:
        stderr = proc.stderr.read()
        raise RuntimeError(f"tool server exited without response; stderr={stderr!r}")
    return json.loads(line)


def main() -> int:
    manifest = {
        "spec_version": "um-acds/0.1",
        "manifest_version": "0.0.1",
        "caps_sha256": "sha256:" + ("a" * 64),
        "node": {"node_id": "esp32_lab_bridge_1"},
        "runtime": {"agent_core": {"version": "0.0.0"}},
        "hardware": {"presence": {"home.light.switch": "present"}},
        "tools": [
            {
                "name": "home.light.switch",
                "description": "Switch a lab light through an ESP32 bridge.",
                "kind": "actuator",
                "parameters_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "state": {"type": "string", "enum": ["on", "off"]},
                        "reason": {"type": "string"},
                    },
                    "required": ["state"],
                },
                "timeout_ms": 1000,
                "idempotent": False,
                "side_effect_level": "low",
                "hazards": ["visible_light"],
            }
        ],
        "safety": {},
        "tags": ["room:lab"],
    }
    with tempfile.TemporaryDirectory() as td:
        manifest_path = Path(td) / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        proc = subprocess.Popen(
            [
                sys.executable,
                "-u",
                str(SCRIPT),
                "--manifest",
                str(manifest_path),
                "--transport",
                "dry-run",
            ],
            cwd=str(ROOT),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            write_line(proc, {"id": 1, "op": "manifest"})
            resp = read_line(proc)
            if not resp.get("ok"):
                raise RuntimeError(f"manifest failed: {resp}")
            tools = resp.get("tools")
            if not isinstance(tools, list) or len(tools) != 1:
                raise RuntimeError(f"unexpected tools: {tools}")
            tool = tools[0]
            if tool.get("name") != "esp32_home_light_switch":
                raise RuntimeError(f"wrong tool name: {tool}")
            bridge = tool.get("bridge")
            if not isinstance(bridge, dict) or bridge.get("node_id") != "esp32_lab_bridge_1":
                raise RuntimeError(f"missing bridge metadata: {tool}")
            if bridge.get("um_acds_name") != "home.light.switch":
                raise RuntimeError(f"missing UM-ACDS source name in bridge metadata: {tool}")
            params = tool.get("parameters")
            if not isinstance(params, dict) or params.get("additionalProperties") is not False:
                raise RuntimeError(f"parameters schema not preserved: {tool}")

            write_line(
                proc,
                {
                    "id": 2,
                    "op": "execute",
                    "tool_name": "esp32_home_light_switch",
                    "arguments": {"state": "on", "reason": "smoke"},
                },
            )
            resp = read_line(proc)
            if not resp.get("ok"):
                raise RuntimeError(f"execute failed: {resp}")
            result = resp.get("tool_result")
            data = result.get("data") if isinstance(result, dict) else None
            envelope = data.get("task_assign") if isinstance(data, dict) else None
            if not isinstance(envelope, dict) or envelope.get("type") != "TASK_ASSIGN":
                raise RuntimeError(f"missing TASK_ASSIGN envelope: {resp}")
            body = envelope.get("body")
            if not isinstance(body, dict):
                raise RuntimeError(f"missing TASK_ASSIGN body: {resp}")
            if body.get("node_id") != "esp32_lab_bridge_1":
                raise RuntimeError(f"wrong node_id: {body}")
            if body.get("tool") != "home.light.switch":
                raise RuntimeError(f"wrong tool: {body}")
            payload = body.get("payload")
            if not isinstance(payload, dict) or payload.get("args", {}).get("state") != "on":
                raise RuntimeError(f"wrong payload: {body}")
            if payload.get("agentd_tool") != "esp32_home_light_switch":
                raise RuntimeError(f"missing safe agentd tool mapping: {body}")

            write_line(
                proc,
                {
                    "id": 3,
                    "op": "execute",
                    "tool_name": "esp32_home_light_switch",
                    "arguments": {"state": "blink"},
                },
            )
            resp = read_line(proc)
            if resp.get("ok") is not False or "one of" not in str(resp.get("error", "")):
                raise RuntimeError(f"enum validation did not fail deterministically: {resp}")

            write_line(proc, {"id": 4, "op": "ping"})
            resp = read_line(proc)
            if not resp.get("ok") or resp.get("pong") is not True:
                raise RuntimeError(f"ping failed: {resp}")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
