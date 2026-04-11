#!/usr/bin/env python3
"""Assertions for agentd voice WebRTC runtime smoke responses."""

import argparse
import json
import sys


def fail(message, obj):
    print(message, obj, file=sys.stderr)
    raise SystemExit(1)


def expect(condition, message, obj):
    if not condition:
        fail(message, obj)


def load_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def response_json(args):
    body_path = getattr(args, "body_path", None)
    if body_path is not None:
        return load_json(body_path)
    raw = args.response_json if args.response_json is not None else sys.stdin.read()
    return json.loads(raw)


def bool_arg(value):
    if value is None:
        return None
    return value == "true"


def value_at_path(obj, path):
    current = obj
    for key in path.split("."):
        if not isinstance(current, dict) or key not in current:
            return None
        current = current[key]
    return current


def print_field(args):
    obj = response_json(args)
    value = value_at_path(obj, args.path)
    if args.require_ok:
        expect(obj.get("ok") is True, f"expected {args.label} ok=true", obj)
    if args.nonempty:
        expect(isinstance(value, str) and value.strip(), f"missing {args.label} {args.path}", obj)
        print(value.strip())
        return
    if args.positive_int:
        expect(isinstance(value, int) and value > 0, f"missing positive {args.label} {args.path}", obj)
        print(value)
        return
    expect(value is not None, f"missing {args.label} {args.path}", obj)
    if isinstance(value, (dict, list)):
        print(json.dumps(value))
    else:
        print(value)


def split_path_value(value):
    if "=" not in value:
        raise SystemExit(f"expected PATH=VALUE argument, got {value!r}")
    path, expected = value.split("=", 1)
    if not path:
        raise SystemExit(f"expected non-empty PATH in {value!r}")
    return path, expected


def parse_expected(value):
    if value == "true":
        return True
    if value == "false":
        return False
    if value == "null":
        return None
    try:
        return int(value)
    except ValueError:
        return value


def assert_fields(args):
    obj = response_json(args)
    if args.require_ok:
        expect(obj.get("ok") is True, f"expected {args.label} ok=true", obj)
    for path in args.true:
        expect(value_at_path(obj, path) is True, f"expected {args.label} {path}=true", obj)
    for path in args.false:
        expect(value_at_path(obj, path) is False, f"expected {args.label} {path}=false", obj)
    for path in args.none:
        expect(value_at_path(obj, path) is None, f"expected {args.label} {path}=null", obj)
    for path in args.nonempty:
        value = value_at_path(obj, path)
        expect(isinstance(value, str) and value.strip(), f"expected {args.label} {path} to be non-empty", obj)
    for item in args.equals:
        path, expected = split_path_value(item)
        parsed = parse_expected(expected)
        expect(value_at_path(obj, path) == parsed, f"expected {args.label} {path}={parsed!r}", obj)
    for item in args.equals_string:
        path, expected = split_path_value(item)
        expect(value_at_path(obj, path) == expected, f"expected {args.label} {path}={expected!r}", obj)
    for item in args.minimum:
        path, expected = split_path_value(item)
        value = value_at_path(obj, path)
        expect(
            isinstance(value, (int, float)) and value >= float(expected),
            f"expected {args.label} {path}>={expected}",
            obj,
        )
    for item in args.contains:
        path, expected = split_path_value(item)
        expect(
            expected in str(value_at_path(obj, path) or ""),
            f"expected {args.label} {path} to contain {expected!r}",
            obj,
        )


def assert_started(args):
    obj = response_json(args)
    peer = obj.get("peer") or {}
    expect(obj.get("ok") is True and obj.get("started") is True, f"{args.label} start failed", obj)
    if args.runtime_kind is not None:
        expect(peer.get("runtime_kind") == args.runtime_kind, f"expected {args.label} runtime_kind={args.runtime_kind}", obj)
    expected_managed = bool_arg(args.managed)
    if expected_managed is not None:
        expect(
            peer.get("managed_broker_session") is expected_managed,
            f"expected {args.label} managed_broker_session={expected_managed}",
            obj,
        )
    if args.broker_agent_id is not None:
        expect(
            peer.get("broker_agent_id") == args.broker_agent_id,
            f"expected {args.label} broker_agent_id={args.broker_agent_id}",
            obj,
        )
    if args.broker_deployment_id is not None:
        expect(
            peer.get("broker_deployment_id") == args.broker_deployment_id,
            f"expected {args.label} broker_deployment_id={args.broker_deployment_id}",
            obj,
        )
    expected_running = bool_arg(args.peer_running)
    if expected_running is not None:
        expect(peer.get("running") is expected_running, f"expected {args.label} peer running={expected_running}", obj)
    expected_broker_defaults = bool_arg(args.broker_defaults)
    if expected_broker_defaults is not None:
        expect(
            obj.get("broker_url_default_configured") is expected_broker_defaults
            and obj.get("broker_token_default_configured") is expected_broker_defaults,
            f"expected {args.label} broker defaults={expected_broker_defaults}",
            obj,
        )
    expected_builtin_available = bool_arg(args.builtin_available)
    if expected_builtin_available is not None:
        expect(
            obj.get("builtin_available") is expected_builtin_available,
            f"expected {args.label} builtin_available={expected_builtin_available}",
            obj,
        )
    expected_bundled_available = bool_arg(args.bundled_available)
    if expected_bundled_available is not None:
        expect(
            obj.get("bundled_available") is expected_bundled_available,
            f"expected {args.label} bundled_available={expected_bundled_available}",
            obj,
        )
    expected_external_available = bool_arg(args.external_available)
    if expected_external_available is not None:
        expect(
            obj.get("external_available") is expected_external_available,
            f"expected {args.label} external_available={expected_external_available}",
            obj,
        )
    if args.default_runtime_kind is not None:
        expect(
            obj.get("default_runtime_kind") == args.default_runtime_kind,
            f"expected {args.label} default_runtime_kind={args.default_runtime_kind}",
            obj,
        )
    if args.default_runtime_kind_source is not None:
        expect(
            obj.get("default_runtime_kind_source") == args.default_runtime_kind_source,
            f"expected {args.label} default_runtime_kind_source={args.default_runtime_kind_source}",
            obj,
        )
    expected_default_available = bool_arg(args.default_runtime_kind_available)
    if expected_default_available is not None:
        expect(
            obj.get("default_runtime_kind_available") is expected_default_available,
            f"expected {args.label} default_runtime_kind_available={expected_default_available}",
            obj,
        )


def assert_stopped(args):
    obj = response_json(args)
    peer = obj.get("peer") or {}
    expect(obj.get("ok") is True, f"{args.label} stop failed", obj)
    if args.peer_absent:
        expect(obj.get("peer") is None, f"expected {args.label} peer=null", obj)
    expected_stopped = bool_arg(args.stopped)
    if expected_stopped is not None:
        expect(obj.get("stopped") is expected_stopped, f"expected {args.label} stopped={expected_stopped}", obj)
    if args.reason is not None:
        expect(obj.get("reason") == args.reason, f"expected {args.label} reason={args.reason}", obj)
    expected_deleted = bool_arg(args.broker_session_deleted)
    if expected_deleted is not None:
        expect(
            obj.get("broker_session_deleted") is expected_deleted,
            f"expected {args.label} broker_session_deleted={expected_deleted}",
            obj,
        )
    if args.broker_session_deleted_absent:
        expect("broker_session_deleted" not in obj, f"expected {args.label} to omit broker_session_deleted", obj)
    for needle in args.broker_session_delete_error_contains:
        expect(
            needle in str(obj.get("broker_session_delete_error", "")),
            f"expected {args.label} broker_session_delete_error to contain {needle!r}",
            obj,
        )
    if args.runtime_kind is not None:
        expect(peer.get("runtime_kind") == args.runtime_kind, f"expected {args.label} runtime_kind={args.runtime_kind}", obj)
    expected_managed = bool_arg(args.managed)
    if expected_managed is not None:
        expect(
            peer.get("managed_broker_session") is expected_managed,
            f"expected {args.label} managed_broker_session={expected_managed}",
            obj,
        )
    expected_running = bool_arg(args.peer_running)
    if expected_running is not None:
        expect(peer.get("running") is expected_running, f"expected {args.label} peer running={expected_running}", obj)
    if args.status_source is not None:
        expect(peer.get("status_source") == args.status_source, f"expected {args.label} status_source={args.status_source}", obj)
    if args.exit_signal is not None:
        expect(peer.get("exit_signal") == args.exit_signal, f"expected {args.label} exit_signal={args.exit_signal}", obj)


def expect_builtin_runtime_preview(obj, planned_runtime):
    peer = obj.get("peer") or {}
    expect(
        peer.get("schema") == "session_voice_webrtc_peer_runtime_v1",
        "expected builtin top-level peer preview schema",
        obj,
    )
    for key in ("runtime_kind", "session_id", "broker_url", "stdout_log_path", "stderr_log_path", "ready_file_path"):
        expect(
            peer.get(key) == planned_runtime.get(key),
            f"expected builtin top-level peer preview to match planned_runtime for {key}",
            obj,
        )
    expect(peer.get("status_source") == "planned", "expected builtin top-level peer preview status_source=planned", obj)
    expect(
        peer.get("tool_path") == "@builtin" and peer.get("node_bin") == "@builtin",
        "expected builtin top-level peer preview builtin execution sentinels",
        obj,
    )
    expect(
        peer.get("managed_broker_session") is True and peer.get("running") is False and peer.get("ready") is False,
        "expected builtin top-level peer preview state",
        obj,
    )
    expect("disabled" in str(peer.get("last_error") or ""), "expected builtin top-level peer preview disabled error", obj)


def assert_builtin_auto_create_contract(args):
    obj = load_json(args.body_path)
    expect(
        obj.get("builtin_available") is False and obj.get("bundled_available") is True,
        "unexpected builtin contract response",
        obj,
    )
    expect(
        obj.get("external_available") is False and obj.get("default_runtime_kind") == "bundled",
        "unexpected builtin contract response",
        obj,
    )
    expect(
        obj.get("default_runtime_kind_source") == "env" and obj.get("default_runtime_kind_available") is True,
        "unexpected builtin contract response",
        obj,
    )
    expect(
        obj.get("broker_url_default_configured") is True and obj.get("broker_token_default_configured") is True,
        "expected broker defaults in builtin contract response",
        obj,
    )
    expect("disabled" in str(obj.get("builtin_unavailable_reason", "")), "expected builtin disabled unavailable reason", obj)
    expect("disabled" in str(obj.get("error", "")), "expected builtin disabled error", obj)

    contract = obj.get("builtin_start_contract") or {}
    expect(
        contract.get("runtime_kind") == "builtin" and contract.get("signaling_surface") == "voice_webrtc_peer",
        "expected builtin start contract metadata",
        obj,
    )
    expect(
        contract.get("mutating_broker_actions_deferred") is True,
        "expected builtin contract to defer mutating broker actions",
        obj,
    )

    sequence = contract.get("startup_sequence") or []
    expect(
        [step.get("stage") for step in sequence]
        == [
            "auto_create_broker_session",
            "launch_runtime",
            "startup_confirmation",
            "startup_failure_cleanup",
        ],
        "expected builtin startup sequence",
        obj,
    )
    expect(
        sequence and sequence[0].get("deferred", False) and len(sequence) > 1 and sequence[1].get("deferred", False),
        "expected builtin startup sequence to remain deferred",
        obj,
    )

    broker_session = contract.get("broker_session") or {}
    expect(
        broker_session.get("mode") == "auto_create" and broker_session.get("agent_id") == args.broker_agent_id,
        "expected builtin auto-create broker contract",
        obj,
    )
    expect(
        broker_session.get("deployment_id") == args.broker_deployment_id,
        "expected builtin deployment contract",
        obj,
    )
    expect(
        broker_session.get("session_id") is None,
        "expected builtin auto-create broker contract to omit session_id",
        obj,
    )

    artifacts = contract.get("runtime_artifacts") or {}
    runtime_dir = artifacts.get("runtime_dir") or ""
    expect(
        runtime_dir.endswith("/voice_webrtc_peers/" + obj.get("session_id", "")),
        "expected builtin runtime_dir contract",
        obj,
    )
    expect(
        artifacts.get("ready_file_path") == runtime_dir + "/ready.json",
        "expected builtin ready_file_path contract",
        obj,
    )
    expect(
        artifacts.get("stdout_log_path") == runtime_dir + "/stdout.jsonl",
        "expected builtin stdout_log_path contract",
        obj,
    )
    expect(
        artifacts.get("stderr_log_path") == runtime_dir + "/stderr.log",
        "expected builtin stderr_log_path contract",
        obj,
    )
    expect(
        artifacts.get("stdout_format") == "jsonl" and artifacts.get("stderr_format") == "text",
        "expected builtin runtime artifact format contract",
        obj,
    )

    media_plan = contract.get("media_runtime_plan") or {}
    expect(
        media_plan.get("schema") == "voice_webrtc_peer_media_runtime_plan_v1",
        "expected builtin media runtime plan schema",
        obj,
    )
    expect(
        media_plan.get("signaling_surface") == "voice_webrtc_peer",
        "expected builtin media runtime signaling surface",
        obj,
    )
    expect(
        media_plan.get("runtime_kind") == "builtin" and media_plan.get("session_id") == obj.get("session_id"),
        "expected builtin media runtime plan identity",
        obj,
    )
    expect(
        media_plan.get("broker_session_id") is None,
        "expected builtin auto-create media runtime plan to omit broker_session_id",
        obj,
    )
    expect(
        media_plan.get("managed_broker_session") is True,
        "expected builtin media runtime plan managed broker session",
        obj,
    )
    expect(
        media_plan.get("broker_agent_id") == args.broker_agent_id
        and media_plan.get("broker_deployment_id") == args.broker_deployment_id,
        "expected builtin media runtime plan broker ownership metadata",
        obj,
    )
    expect(
        media_plan.get("ready_signal") == "ready_file"
        and media_plan.get("ready_file_path") == artifacts.get("ready_file_path"),
        "expected builtin media runtime plan ready contract",
        obj,
    )
    expect(
        media_plan.get("deadline_ms") == contract.get("deadline_ms")
        and media_plan.get("poll_interval_ms") == contract.get("poll_interval_ms")
        and media_plan.get("tone_hz") == contract.get("tone_hz"),
        "expected builtin media runtime timing contract",
        obj,
    )

    planned_runtime = contract.get("planned_runtime") or {}
    expect(
        planned_runtime.get("schema") == "session_voice_webrtc_peer_runtime_v1",
        "expected builtin planned runtime schema",
        obj,
    )
    expect(
        planned_runtime.get("status_source") == "planned",
        "expected builtin planned runtime status_source=planned",
        obj,
    )
    expect(
        planned_runtime.get("runtime_kind") == "builtin" and planned_runtime.get("session_id") == obj.get("session_id"),
        "expected builtin planned runtime identity",
        obj,
    )
    expect(
        planned_runtime.get("tool_path") == "@builtin" and planned_runtime.get("node_bin") == "@builtin",
        "expected builtin planned runtime builtin execution sentinels",
        obj,
    )
    expect(
        planned_runtime.get("managed_broker_session") is True,
        "expected builtin planned runtime managed broker session",
        obj,
    )
    expect(
        planned_runtime.get("running") is False and planned_runtime.get("ready") is False,
        "expected builtin planned runtime to stay inactive",
        obj,
    )
    expect(
        planned_runtime.get("stdout_log_path") == artifacts.get("stdout_log_path"),
        "expected builtin planned runtime stdout_log_path to match artifacts",
        obj,
    )
    expect(
        planned_runtime.get("broker_agent_id") == args.broker_agent_id
        and planned_runtime.get("broker_deployment_id") == args.broker_deployment_id,
        "expected builtin planned runtime broker ownership metadata",
        obj,
    )
    expect_builtin_runtime_preview(obj, planned_runtime)


def assert_builtin_borrowed_contract(args):
    obj = load_json(args.body_path)
    expect(obj.get("ok") is False, "expected builtin borrowed start to fail not-implemented", obj)
    contract = obj.get("builtin_start_contract") or {}
    broker_session = contract.get("broker_session") or {}
    expect(broker_session.get("mode") == "borrowed", "expected builtin borrowed broker contract", obj)
    expect(
        broker_session.get("session_id") == args.broker_session_id and broker_session.get("preflighted") is True,
        "expected builtin borrowed broker session id/preflight",
        obj,
    )
    expect(
        broker_session.get("session_mode") == "webrtc",
        "expected builtin borrowed broker session mode",
        obj,
    )
    expect(
        broker_session.get("agent_id") is None and broker_session.get("deployment_id") is None,
        "expected builtin borrowed broker contract to omit auto-create ownership fields",
        obj,
    )
    media_plan = contract.get("media_runtime_plan") or {}
    expect(
        media_plan.get("broker_session_id") == args.broker_session_id
        and media_plan.get("managed_broker_session") is False,
        "expected builtin borrowed media runtime plan",
        obj,
    )
    expect(
        media_plan.get("broker_agent_id") is None and media_plan.get("broker_deployment_id") is None,
        "expected builtin borrowed media runtime plan to omit ownership fields",
        obj,
    )
    planned_runtime = contract.get("planned_runtime") or {}
    peer = obj.get("peer") or {}
    expect(
        planned_runtime.get("broker_session_id") == args.broker_session_id
        and planned_runtime.get("managed_broker_session") is False,
        "expected builtin borrowed planned runtime",
        obj,
    )
    expect(
        peer.get("broker_session_id") == args.broker_session_id and peer.get("managed_broker_session") is False,
        "expected builtin borrowed top-level peer preview",
        obj,
    )


def assert_error_response(args):
    obj = load_json(args.body_path)
    expect(obj.get("ok") is False, f"expected {args.label} to fail", obj)
    for needle in args.error_contains:
        expect(needle in str(obj.get("error", "")), f"expected {args.label} error to contain {needle!r}", obj)


def assert_no_runtime(args):
    obj = response_json(args)
    expected_session_exists = bool_arg(args.expect_session_exists)
    if expected_session_exists is not None:
        expect(
            obj.get("session_exists") is expected_session_exists,
            f"expected {args.label} session_exists={expected_session_exists}",
            obj,
        )
    expect(
        obj.get("running") is False and obj.get("peer") is None,
        f"expected no runtime after {args.label}",
        obj,
    )


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)

    auto = sub.add_parser("assert-builtin-auto-create-contract")
    auto.add_argument("--body-path", required=True)
    auto.add_argument("--broker-agent-id", default="a-1")
    auto.add_argument("--broker-deployment-id", default="lab-builtin-contract")
    auto.set_defaults(func=assert_builtin_auto_create_contract)

    borrowed = sub.add_parser("assert-builtin-borrowed-contract")
    borrowed.add_argument("--body-path", required=True)
    borrowed.add_argument("--broker-session-id", required=True)
    borrowed.set_defaults(func=assert_builtin_borrowed_contract)

    error = sub.add_parser("assert-error-response")
    error.add_argument("--body-path", required=True)
    error.add_argument("--label", required=True)
    error.add_argument("--error-contains", action="append", required=True)
    error.set_defaults(func=assert_error_response)

    no_runtime = sub.add_parser("assert-no-runtime")
    no_runtime.add_argument("--label", required=True)
    no_runtime.add_argument("--response-json")
    no_runtime.add_argument("--expect-session-exists", choices=("true", "false"))
    no_runtime.set_defaults(func=assert_no_runtime)

    field = sub.add_parser("print-field")
    field.add_argument("--path", required=True)
    field.add_argument("--label", required=True)
    field.add_argument("--body-path")
    field.add_argument("--response-json")
    field.add_argument("--require-ok", action="store_true")
    field.add_argument("--nonempty", action="store_true")
    field.add_argument("--positive-int", action="store_true")
    field.set_defaults(func=print_field)

    fields = sub.add_parser("assert-fields")
    fields.add_argument("--label", required=True)
    fields.add_argument("--body-path")
    fields.add_argument("--response-json")
    fields.add_argument("--require-ok", action="store_true")
    fields.add_argument("--true", action="append", default=[])
    fields.add_argument("--false", action="append", default=[])
    fields.add_argument("--none", action="append", default=[])
    fields.add_argument("--nonempty", action="append", default=[])
    fields.add_argument("--equals", action="append", default=[])
    fields.add_argument("--equals-string", action="append", default=[])
    fields.add_argument("--min", dest="minimum", action="append", default=[])
    fields.add_argument("--contains", action="append", default=[])
    fields.set_defaults(func=assert_fields)

    started = sub.add_parser("assert-started")
    started.add_argument("--label", required=True)
    started.add_argument("--body-path")
    started.add_argument("--response-json")
    started.add_argument("--runtime-kind")
    started.add_argument("--managed", choices=("true", "false"))
    started.add_argument("--broker-agent-id")
    started.add_argument("--broker-deployment-id")
    started.add_argument("--peer-running", choices=("true", "false"))
    started.add_argument("--broker-defaults", choices=("true", "false"))
    started.add_argument("--builtin-available", choices=("true", "false"))
    started.add_argument("--bundled-available", choices=("true", "false"))
    started.add_argument("--external-available", choices=("true", "false"))
    started.add_argument("--default-runtime-kind")
    started.add_argument("--default-runtime-kind-source")
    started.add_argument("--default-runtime-kind-available", choices=("true", "false"))
    started.set_defaults(func=assert_started)

    stopped = sub.add_parser("assert-stopped")
    stopped.add_argument("--label", required=True)
    stopped.add_argument("--body-path")
    stopped.add_argument("--response-json")
    stopped.add_argument("--stopped", choices=("true", "false"))
    stopped.add_argument("--reason")
    stopped.add_argument("--broker-session-deleted", choices=("true", "false"))
    stopped.add_argument("--broker-session-deleted-absent", action="store_true")
    stopped.add_argument("--broker-session-delete-error-contains", action="append", default=[])
    stopped.add_argument("--runtime-kind")
    stopped.add_argument("--managed", choices=("true", "false"))
    stopped.add_argument("--peer-running", choices=("true", "false"))
    stopped.add_argument("--peer-absent", action="store_true")
    stopped.add_argument("--status-source")
    stopped.add_argument("--exit-signal", type=int)
    stopped.set_defaults(func=assert_stopped)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
