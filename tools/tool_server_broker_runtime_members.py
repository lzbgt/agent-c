#!/usr/bin/env python3
import json
import os
import sys
import urllib.request
import urllib.error
import urllib.parse

TOOL_NAME = "broker_team_runtime_members_update"

MANIFEST = {
    "ok": True,
    "tools": [
        {
            "name": TOOL_NAME,
            "description": "Update broker team run runtime members (replace/merge).",
            "parameters": {
                "type": "object",
                "properties": {
                    "team_id": {"type": "string"},
                    "team_run_id": {"type": "string"},
                    "mode": {"type": "string", "enum": ["replace", "merge"]},
                    "runtime_members": {"type": "array", "items": {"type": "object"}},
                    "broker_base": {"type": "string"},
                    "auth_token": {"type": "string"},
                },
                "required": ["team_id", "team_run_id", "runtime_members"],
            },
        }
    ],
}


def emit(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def normalize_token(raw):
    if not raw:
        return ""
    token = str(raw).strip()
    if not token:
        return ""
    if token.lower().startswith("bearer "):
        return token
    return "Bearer " + token


def read_env(name):
    val = os.environ.get(name, "")
    return val.strip()


def handle_execute(msg_id, tool_name, args):
    if tool_name != TOOL_NAME:
        emit({"id": msg_id, "ok": False, "error": "unknown tool"})
        return
    if not isinstance(args, dict):
        emit({"id": msg_id, "ok": False, "error": "arguments must be object"})
        return
    team_id = str(args.get("team_id", "")).strip()
    run_id = str(args.get("team_run_id", "")).strip()
    runtime_members = args.get("runtime_members")
    mode = str(args.get("mode", "replace") or "replace").strip().lower()
    if mode not in ("replace", "merge"):
        emit({"id": msg_id, "ok": False, "error": "invalid mode"})
        return
    if not team_id:
        emit({"id": msg_id, "ok": False, "error": "team_id required"})
        return
    if not run_id:
        emit({"id": msg_id, "ok": False, "error": "team_run_id required"})
        return
    if not isinstance(runtime_members, list):
        emit({"id": msg_id, "ok": False, "error": "runtime_members must be array"})
        return

    base = str(args.get("broker_base", "") or read_env("BROKER_BASE_URL") or read_env("AGENTD_BROKER_BASE_URL")).strip()
    if not base:
        emit({"id": msg_id, "ok": False, "error": "broker_base missing (set broker_base or BROKER_BASE_URL)"})
        return
    base = base.rstrip("/")
    auth_token = str(args.get("auth_token", "") or read_env("BROKER_AUTH_TOKEN")).strip()
    headers = {
        "Content-Type": "application/json",
    }
    auth = normalize_token(auth_token)
    if auth:
        headers["Authorization"] = auth

    payload = {"mode": mode, "runtime_members": runtime_members}
    body = json.dumps(payload).encode("utf-8")
    url = f"{base}/v1/teams/{urllib.parse.quote(team_id)}/runs/{urllib.parse.quote(run_id)}/runtime_members"
    req = urllib.request.Request(url, data=body, headers=headers, method="PATCH")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read().decode("utf-8")
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                data = {"raw": raw}
            emit({"id": msg_id, "ok": True, "tool_result": {"ok": True, "status": resp.status, "data": data}})
    except urllib.error.HTTPError as err:
        raw = err.read().decode("utf-8") if err.fp else ""
        emit({
            "id": msg_id,
            "ok": True,
            "tool_result": {"ok": False, "status": err.code, "error": raw or str(err)},
        })
    except Exception as err:  # noqa: BLE001
        emit({"id": msg_id, "ok": True, "tool_result": {"ok": False, "error": str(err)}})


def main():
    for line in sys.stdin:
        raw = line.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            emit({"id": None, "ok": False, "error": "invalid json"})
            continue
        msg_id = msg.get("id")
        op = msg.get("op")
        if op == "manifest":
            emit({"id": msg_id, **MANIFEST})
            continue
        if op == "execute":
            tool_name = msg.get("tool_name", "")
            args = msg.get("arguments", {})
            handle_execute(msg_id, tool_name, args)
            continue
        emit({"id": msg_id, "ok": False, "error": "unsupported op"})


if __name__ == "__main__":
    main()
