#!/usr/bin/env python3
"""Test helper for seeding voice WebRTC peer runtime records."""

import argparse
import json
import pathlib
import sqlite3
import sys


def write_meta(db_path, session_id, value):
    with sqlite3.connect(db_path) as conn:
        conn.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)",
            (f"session.voice_webrtc_peer.{session_id}", value),
        )
        conn.commit()


def write_artifact(runtime_dir, artifact_json):
    if not runtime_dir:
        return
    path = pathlib.Path(runtime_dir)
    path.mkdir(parents=True, exist_ok=True)
    (path / "stdout.jsonl").write_text(artifact_json + "\n", encoding="utf-8")


def stale_record(args):
    runtime_dir = pathlib.Path(args.state_dir) / "voice_webrtc_peers" / args.session_id
    write_artifact(runtime_dir, '{"stale":"artifact"}')
    record = {
        "schema": "session_voice_webrtc_peer_runtime_v1",
        "runtime_kind": args.runtime_kind,
        "status_source": "memory",
        "session_id": args.session_id,
        "broker_session_id": "stale-broker-session",
        "broker_url": args.broker_url,
        "managed_broker_session": False,
        "sender_tag": "agentd_runtime_peer",
        "tool_path": args.peer_tool,
        "node_bin": "node",
        "ready_file_path": str(runtime_dir / "ready.json"),
        "stdout_log_path": str(runtime_dir / "stdout.jsonl"),
        "stderr_log_path": str(runtime_dir / "stderr.log"),
        "started_unix_ms": 1700000000000,
        "deadline_ms": 15000,
        "poll_interval_ms": 100,
        "tone_hz": 440,
        "ready": True,
        "running": True,
        "pid": 999999,
    }
    write_meta(args.db_path, args.session_id, json.dumps(record))


def planned_record(args):
    runtime_dir = pathlib.Path(args.runtime_dir)
    write_artifact(runtime_dir, '{"planned":"artifact"}')
    record = {
        "schema": "session_voice_webrtc_peer_runtime_v1",
        "runtime_kind": "builtin",
        "status_source": "planned",
        "session_id": args.session_id,
        "broker_url": args.broker_url,
        "managed_broker_session": True,
        "broker_agent_id": "a-1",
        "broker_deployment_id": "lab-planned-runtime-self-heal",
        "sender_tag": "agentd_runtime_peer",
        "tool_path": "@builtin",
        "node_bin": "@builtin",
        "ready_file_path": str(runtime_dir / "ready.json"),
        "stdout_log_path": str(runtime_dir / "stdout.jsonl"),
        "stderr_log_path": str(runtime_dir / "stderr.log"),
        "started_unix_ms": 0,
        "deadline_ms": 15000,
        "poll_interval_ms": 100,
        "tone_hz": 440,
        "ready": False,
        "running": False,
    }
    write_meta(args.db_path, args.session_id, json.dumps(record))


def corrupt_record(args):
    if args.runtime_dir and args.artifact_json:
        write_artifact(args.runtime_dir, args.artifact_json)
    write_meta(args.db_path, args.session_id, args.value)


def delete_session(args):
    with sqlite3.connect(args.db_path) as conn:
        conn.execute("DELETE FROM sessions WHERE session_id = ?", (args.session_id,))
        conn.commit()


def maybe_bool(value):
    if value is None:
        return None
    return value == "true"


def require_equal(actual, expected, label, detail):
    if actual != expected:
        print(f"expected {label} {detail}, got {actual!r}", file=sys.stderr)
        raise SystemExit(1)


def assert_cleared(args):
    raw = args.response_json if args.response_json is not None else sys.stdin.read()
    obj = json.loads(raw)
    cleanup = obj.get(args.cleanup_key) or {}
    label = args.label

    require_equal(obj.get("ok"), True, label, "ok=true")
    expect_session_exists = maybe_bool(args.expect_session_exists)
    if expect_session_exists is not None:
        require_equal(obj.get("session_exists"), expect_session_exists, label, f"session_exists={expect_session_exists}")
    expect_running = maybe_bool(args.expect_running)
    if expect_running is not None:
        require_equal(obj.get("running"), expect_running, label, f"running={expect_running}")
    expect_stopped = maybe_bool(args.expect_stopped)
    if expect_stopped is not None:
        require_equal(obj.get("stopped"), expect_stopped, label, f"stopped={expect_stopped}")
    if args.expect_reason is not None:
        require_equal(obj.get("reason"), args.expect_reason, label, f"reason={args.expect_reason!r}")
    require_equal(obj.get("peer"), None, label, "peer=null")
    require_equal(cleanup.get("persisted_record_cleared"), True, label, "persisted_record_cleared=true")
    require_equal(cleanup.get("runtime_artifacts_deleted"), True, label, "runtime_artifacts_deleted=true")

    runtime_dir = pathlib.Path(args.runtime_dir)
    if runtime_dir.exists():
        print(f"expected {label} artifacts removed: {runtime_dir}", file=sys.stderr)
        raise SystemExit(1)

    with sqlite3.connect(args.db_path) as conn:
        row = conn.execute(
            "SELECT value FROM meta WHERE key = ?",
            (f"session.voice_webrtc_peer.{args.session_id}",),
        ).fetchone()
    if row is None or row[0] != "":
        print(f"expected {label} meta row cleared to empty string: {row!r}", file=sys.stderr)
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)

    stale = sub.add_parser("write-stale")
    stale.add_argument("--db-path", required=True)
    stale.add_argument("--state-dir", required=True)
    stale.add_argument("--session-id", required=True)
    stale.add_argument("--runtime-kind", required=True)
    stale.add_argument("--broker-url", required=True)
    stale.add_argument("--peer-tool", required=True)
    stale.set_defaults(func=stale_record)

    planned = sub.add_parser("write-planned")
    planned.add_argument("--db-path", required=True)
    planned.add_argument("--runtime-dir", required=True)
    planned.add_argument("--session-id", required=True)
    planned.add_argument("--broker-url", required=True)
    planned.set_defaults(func=planned_record)

    corrupt = sub.add_parser("write-corrupt")
    corrupt.add_argument("--db-path", required=True)
    corrupt.add_argument("--session-id", required=True)
    corrupt.add_argument("--value", required=True)
    corrupt.add_argument("--runtime-dir")
    corrupt.add_argument("--artifact-json")
    corrupt.set_defaults(func=corrupt_record)

    delete = sub.add_parser("delete-session")
    delete.add_argument("--db-path", required=True)
    delete.add_argument("--session-id", required=True)
    delete.set_defaults(func=delete_session)

    cleared = sub.add_parser("assert-cleared")
    cleared.add_argument("--db-path", required=True)
    cleared.add_argument("--session-id", required=True)
    cleared.add_argument("--runtime-dir", required=True)
    cleared.add_argument("--cleanup-key", required=True)
    cleared.add_argument("--label", required=True)
    cleared.add_argument("--response-json")
    cleared.add_argument("--expect-session-exists", choices=("true", "false"))
    cleared.add_argument("--expect-running", choices=("true", "false"))
    cleared.add_argument("--expect-stopped", choices=("true", "false"))
    cleared.add_argument("--expect-reason")
    cleared.set_defaults(func=assert_cleared)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
