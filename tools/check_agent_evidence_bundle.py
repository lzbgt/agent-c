#!/usr/bin/env python3
import argparse
import json
import os
import sys
from typing import Any, Dict, List, Tuple


def read_json(path: str) -> Tuple[bool, Any, str]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return True, json.load(f), ""
    except Exception as exc:
        return False, None, str(exc)


def want_file(base_dir: str, name: str) -> str:
    return os.path.join(base_dir, name)


def check_ok_field(path: str, data: Any, errors: List[str], warnings: List[str]) -> None:
    if not isinstance(data, dict):
        warnings.append(f"{path}: json is not an object")
        return
    if "ok" in data and data.get("ok") is not True:
        errors.append(f"{path}: ok=false")


def check_required_files(base_dir: str, names: List[str], errors: List[str]) -> None:
    for name in names:
        path = want_file(base_dir, name)
        if not os.path.isfile(path):
            errors.append(f"missing {name}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate an agentd/broker evidence bundle")
    parser.add_argument("--dir", required=True, help="evidence bundle directory")
    parser.add_argument("--require-agentd", action="store_true", help="fail if agentd captures are missing")
    parser.add_argument("--require-broker", action="store_true", help="fail if broker captures are missing")
    parser.add_argument("--strict", action="store_true", help="treat warnings as errors")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    base_dir = os.path.abspath(args.dir)
    errors: List[str] = []
    warnings: List[str] = []

    if not os.path.isdir(base_dir):
        print(f"dir not found: {base_dir}", file=sys.stderr)
        return 2

    meta_path = want_file(base_dir, "meta.json")
    if not os.path.isfile(meta_path):
        errors.append("missing meta.json")
        meta = {}
    else:
        ok, meta, err = read_json(meta_path)
        if not ok:
            errors.append(f"meta.json: {err}")
            meta = {}
        elif isinstance(meta, dict):
            if not meta.get("captured_at"):
                warnings.append("meta.json: missing captured_at")
        else:
            warnings.append("meta.json: not an object")

    readme_path = want_file(base_dir, "README.txt")
    if not os.path.isfile(readme_path):
        warnings.append("missing README.txt")

    agentd_files = [
        "agentd_health.json",
        "agentd_config.json",
        "agentd_diagnostics.json",
        "agentd_diagnostics_providers.json",
        "agentd_sessions.json",
    ]
    broker_files = [
        "broker_healthz.json",
        "broker_readyz.json",
        "broker_agents.json",
    ]

    if args.require_agentd:
        check_required_files(base_dir, agentd_files, errors)
    if args.require_broker:
        check_required_files(base_dir, broker_files, errors)

    for name in agentd_files + broker_files + [
        "broker_members.json",
        "broker_membership_audit.json",
        "agentd_trace.json",
        "broker_trace.json",
    ]:
        path = want_file(base_dir, name)
        if not os.path.isfile(path):
            continue
        ok, data, err = read_json(path)
        if not ok:
            errors.append(f"{name}: {err}")
            continue
        check_ok_field(name, data, errors, warnings)

    errors_path = want_file(base_dir, "errors.txt")
    if os.path.isfile(errors_path):
        msg = "errors.txt present (capture had failed endpoints)"
        if args.strict:
            errors.append(msg)
        else:
            warnings.append(msg)

    if errors:
        print("Evidence bundle validation failed:")
        for item in errors:
            print(f"- {item}")
        if warnings and not args.strict:
            print("Warnings:")
            for item in warnings:
                print(f"- {item}")
        return 1

    if warnings:
        print("Evidence bundle validation succeeded with warnings:")
        for item in warnings:
            print(f"- {item}")
    else:
        print("Evidence bundle validation succeeded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
