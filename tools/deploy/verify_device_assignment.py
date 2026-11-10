#!/usr/bin/env python3
"""Opt-in public API qualification using two synthetic nodes, never real hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path.home() / ".config/agent-c/gateway.json")
    parser.add_argument("--output", type=Path, default=Path("build/device_assignment_live_verification.json"))
    args = parser.parse_args()
    cfg = json.loads(args.config.read_text())
    origin = cfg["public_url"]
    revision = cfg["source_revision"]
    marker = "qualification-" + revision[:12]
    nodes = [marker + "-a", marker + "-b"]
    tool = "qualification.counter"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def call(path, payload=None):
        headers = {"Authorization": "Bearer " + cfg["operator_token"], "Origin": origin, "Content-Type": "application/json"}
        request = urllib.request.Request(origin + path, data=None if payload is None else json.dumps(payload).encode(), headers=headers)
        try:
            with opener.open(request, timeout=15) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, {}

    def message(node, kind, body):
        status, _ = call("/api/v1/edge/message", {"msg_id": str(uuid.uuid4()), "ts_utc_ms": int(time.time() * 1000), "type": kind, "from": "node:" + node, "to": "platform", "body": body})
        assert status == 200, "Synthetic node message was rejected"

    caps = "sha256:" + hashlib.sha256(marker.encode()).hexdigest()
    for node in nodes:
        message(node, "NODE_HELLO", {"node_id": node, "model": "synthetic-qualification", "fw_git_sha": revision, "caps_sha256": caps})
        manifest = {"spec_version": "um-acds/0.1", "manifest_version": "1", "caps_sha256": caps,
                    "node": {"node_id": node}, "runtime": {"agent_core": {"version": "qualification"}}, "hardware": {"presence": {}},
                    "tags": [marker], "tools": [{"name": tool, "kind": "sensor", "description": "Synthetic qualification only",
                    "parameters_schema": {"type": "object", "properties": {"value": {"type": "integer"}}, "required": ["value"], "additionalProperties": False},
                    "idempotent": True, "side_effect_level": "low", "hazards": [], "timeout_ms": 500}]}
        message(node, "NODE_CAPS_RSP", {"node_id": node, "caps_sha256": caps, "manifest": manifest})

    operation = {"task_id": marker, "step_id": "dispatch", "node_id": nodes[0], "idempotency_key": marker + ":dispatch",
                 "mode": "invoke", "deadline_utc_ms": int(time.time() * 1000) + 60000, "payload": {"tool": tool, "args": {"value": 1}}}
    status, first = call("/api/v1/edge/task/assign", operation)
    assert status == 200, "Initial synthetic assignment failed"
    retry = dict(operation)
    retry.pop("node_id")
    retry["match_any"] = {"requires_tools": [tool], "tags_all": [marker]}
    status, repeated = call("/api/v1/edge/task/assign", retry)
    assert status == 200 and repeated["node_id"] == nodes[0] and repeated["deduped"] and repeated["outbox_id"] == 0, "Recovery did not preserve the assigned device"
    changed = dict(operation, payload={"tool": tool, "args": {"value": 2}})
    assert call("/api/v1/edge/task/assign", changed)[0] == 409, "Changed payload was accepted"
    total = 0
    for node in nodes:
        status, outbox = call("/api/v1/edge/outbox?" + urllib.parse.urlencode({"node_id": node, "cursor": 0, "limit": 50}))
        assert status == 200
        total += sum(1 for row in outbox.get("messages", []) if row.get("msg", {}).get("type") == "TASK_ASSIGN" and row["msg"].get("body", {}).get("task_id") == marker)
    assert total == 1, "Retry published more than one device command"
    message(nodes[0], "TASK_DONE", {"task_id": marker, "step_id": "dispatch", "idempotency_key": operation["idempotency_key"], "result": {"ok": True, "value": 1}})
    status, outcome = call("/api/v1/edge/task?" + urllib.parse.urlencode({"task_id": marker, "step_id": "dispatch"}))
    assert status == 200 and outcome["task"]["state"] == "SUCCEEDED" and outcome["task"]["node_id"] == nodes[0], "Synthetic completion was not recorded"
    status, settled = call("/api/v1/edge/task/assign", retry)
    assert status == 200 and settled["deduped"] and settled["outbox_id"] == 0, "Completion reuse republished a command"
    report = {"source_revision": revision, "synthetic_nodes_only": True, "node_pinned": True,
              "payload_conflict_rejected": True, "one_outbound_command": True, "terminal_replay_no_dispatch": True}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
