#!/usr/bin/env python3
"""Test helper for seeding voice WebRTC peer runtime records."""

import argparse
import json
import pathlib
import sqlite3


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

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
