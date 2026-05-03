#!/usr/bin/env python3
"""Report installed agentd codexw connector service readiness."""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import platform
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any


SENSITIVE_KEY_PARTS = ("TOKEN", "PASSWORD", "SECRET", "KEY")


def env_bool(value: str | None) -> bool:
    return str(value or "").strip().lower() in {"1", "true", "yes", "on"}


def redact_env(env: dict[str, str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for key, value in sorted(env.items()):
        if any(part in key.upper() for part in SENSITIVE_KEY_PARTS) and value:
            result[key] = "<redacted>"
        else:
            result[key] = value
    return result


def read_env_file(path: Path) -> dict[str, str]:
    env: dict[str, str] = {}
    if not path.exists():
        return env
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key:
            env[key] = value
    return env


def run_command(command: list[str], *, env: dict[str, str] | None = None, timeout: float = 15.0) -> dict[str, Any]:
    try:
        proc = subprocess.run(
            command,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        return {
            "ok": proc.returncode == 0,
            "returncode": proc.returncode,
            "stdout": proc.stdout[-12000:],
            "stderr": proc.stderr[-12000:],
        }
    except FileNotFoundError as exc:
        return {"ok": False, "returncode": 127, "stdout": "", "stderr": str(exc)}
    except subprocess.TimeoutExpired as exc:
        return {
            "ok": False,
            "returncode": None,
            "stdout": (exc.stdout or "")[-12000:] if isinstance(exc.stdout, str) else "",
            "stderr": f"command timed out after {timeout}s",
        }


def tail_file(path: Path, max_bytes: int = 6000) -> dict[str, Any]:
    if not path.exists():
        return {"path": str(path), "exists": False, "tail": ""}
    raw = path.read_bytes()
    return {"path": str(path), "exists": True, "tail": raw[-max_bytes:].decode("utf-8", errors="replace")}


def load_plist(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    with path.open("rb") as handle:
        payload = plistlib.load(handle)
    return payload if isinstance(payload, dict) else {}


def launchd_report(args: argparse.Namespace) -> dict[str, Any]:
    label = args.launchd_label
    self_test_label = args.launchd_self_test_label or f"{label}.self-test"
    plist_path = Path(args.launchd_plist_path).expanduser()
    self_test_plist_path = Path(args.launchd_self_test_plist_path).expanduser()
    plist = load_plist(plist_path)
    self_test_plist = load_plist(self_test_plist_path)
    uid = os.getuid()
    supervisor: dict[str, Any] = {
        "kind": "launchd",
        "label": label,
        "plist_path": str(plist_path),
        "plist_exists": plist_path.exists(),
        "self_test_label": self_test_label,
        "self_test_plist_path": str(self_test_plist_path),
        "self_test_plist_exists": self_test_plist_path.exists(),
    }
    if not args.skip_supervisor:
        supervisor["main_state"] = run_command(["launchctl", "print", f"gui/{uid}/{label}"], timeout=args.timeout)
        supervisor["self_test_state"] = run_command(
            ["launchctl", "print", f"gui/{uid}/{self_test_label}"], timeout=args.timeout
        )
    logs = {
        "stdout": tail_file(Path(str(plist.get("StandardOutPath") or ""))) if plist.get("StandardOutPath") else {},
        "stderr": tail_file(Path(str(plist.get("StandardErrorPath") or ""))) if plist.get("StandardErrorPath") else {},
        "self_test_stdout": tail_file(Path(str(self_test_plist.get("StandardOutPath") or "")))
        if self_test_plist.get("StandardOutPath")
        else {},
        "self_test_stderr": tail_file(Path(str(self_test_plist.get("StandardErrorPath") or "")))
        if self_test_plist.get("StandardErrorPath")
        else {},
    }
    env = {}
    env.update(plist.get("EnvironmentVariables") if isinstance(plist.get("EnvironmentVariables"), dict) else {})
    env.update(
        self_test_plist.get("EnvironmentVariables")
        if isinstance(self_test_plist.get("EnvironmentVariables"), dict)
        else {}
    )
    command = self_test_plist.get("ProgramArguments")
    if not isinstance(command, list):
        command = []
    self_test = run_self_test_command(command, env, args)
    return {
        "platform": "launchd",
        "supervisor": supervisor,
        "configuration": {
            "program_arguments": plist.get("ProgramArguments") if isinstance(plist.get("ProgramArguments"), list) else [],
            "self_test_arguments": command,
            "environment": redact_env({str(k): str(v) for k, v in env.items()}),
            "self_test_start_interval": self_test_plist.get("StartInterval"),
            "self_test_keep_alive": bool(self_test_plist.get("KeepAlive")),
        },
        "logs": logs,
        "self_test": self_test,
    }


def systemd_report(args: argparse.Namespace) -> dict[str, Any]:
    env_file = Path(args.systemd_env_file).expanduser()
    env = read_env_file(env_file)
    connector_bin = args.connector_bin
    command = [
        "/usr/bin/env",
        "python3",
        connector_bin,
        "--broker-url",
        env.get("AGENTD_CODEXW_BROKER_URL", ""),
        "--deployment-id",
        env.get("AGENTD_CODEXW_DEPLOYMENT_ID", ""),
        "--display-name",
        env.get("AGENTD_CODEXW_DISPLAY_NAME", env.get("AGENTD_CODEXW_DEPLOYMENT_ID", "")),
        "--identity-dir",
        env.get("AGENTD_CODEXW_IDENTITY_DIR", "/var/lib/agentd/codexw-native"),
        "--agentd-base-url",
        env.get("AGENTD_BASE_URL", "http://127.0.0.1:8123"),
        "--self-test",
        "--require-broker-visible",
    ]
    if env_bool(env.get("AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT")):
        command.append("--require-update-preflight")
    supervisor: dict[str, Any] = {
        "kind": "systemd",
        "service": args.systemd_service,
        "self_test_service": args.systemd_self_test_service,
        "self_test_timer": args.systemd_self_test_timer,
        "env_file": str(env_file),
        "env_file_exists": env_file.exists(),
    }
    if not args.skip_supervisor:
        supervisor["main_state"] = run_command(["systemctl", "show", args.systemd_service, "--no-pager"], timeout=args.timeout)
        supervisor["self_test_state"] = run_command(
            ["systemctl", "show", args.systemd_self_test_service, "--no-pager"], timeout=args.timeout
        )
        supervisor["timer_state"] = run_command(
            ["systemctl", "show", args.systemd_self_test_timer, "--no-pager"], timeout=args.timeout
        )
        supervisor["recent_self_test_logs"] = run_command(
            ["journalctl", "-u", args.systemd_self_test_service, "-n", "80", "--no-pager"],
            timeout=args.timeout,
        )
    self_test_env = os.environ.copy()
    self_test_env.update(env)
    self_test = run_self_test_command(command, self_test_env, args)
    return {
        "platform": "systemd",
        "supervisor": supervisor,
        "configuration": {
            "self_test_arguments": command,
            "environment": redact_env(env),
        },
        "logs": {},
        "self_test": self_test,
    }


def parse_self_test_stdout(stdout: str) -> dict[str, Any] | None:
    stripped = stdout.strip()
    if not stripped:
        return None
    try:
        payload = json.loads(stripped)
        return payload if isinstance(payload, dict) else None
    except json.JSONDecodeError:
        return None


def run_self_test_command(command: list[str], env: dict[str, str], args: argparse.Namespace) -> dict[str, Any]:
    if args.skip_self_test:
        return {"ok": True, "skipped": True}
    if not command:
        return {"ok": False, "error": "self-test command is unavailable"}
    merged_env = os.environ.copy()
    merged_env.update({str(k): str(v) for k, v in env.items()})
    result = run_command([str(part) for part in command], env=merged_env, timeout=args.timeout)
    payload = parse_self_test_stdout(result.get("stdout", ""))
    result["payload"] = payload
    if payload is not None:
        result["ok"] = bool(payload.get("ok")) and bool(result.get("ok"))
    return result


def text_summary(report: dict[str, Any]) -> str:
    lines = [
        f"platform: {report.get('platform')}",
        f"supervisor: {report.get('supervisor', {}).get('kind')}",
        f"self_test_ok: {bool(report.get('self_test', {}).get('ok'))}",
    ]
    payload = report.get("self_test", {}).get("payload")
    if isinstance(payload, dict):
        for check in payload.get("checks", []):
            if isinstance(check, dict):
                lines.append(f"check {check.get('name')}: {'ok' if check.get('ok') else 'failed'}")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    detected = "launchd" if platform.system() == "Darwin" else "systemd"
    parser.add_argument("--platform", choices=("auto", "launchd", "systemd"), default="auto")
    parser.add_argument("--json", action="store_true", help="print JSON report")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--skip-supervisor", action="store_true", help="do not call launchctl/systemctl/journalctl")
    parser.add_argument("--skip-self-test", action="store_true", help="inspect installed profile without running self-test")
    parser.add_argument("--connector-bin", default="/opt/agentd/tools/agentd_codexw_native_broker_connector.py")
    parser.add_argument("--launchd-label", default=os.environ.get("AGENTD_CODEXW_LABEL", "com.agentd.codexw-connector"))
    parser.add_argument("--launchd-self-test-label", default=os.environ.get("AGENTD_CODEXW_SELF_TEST_LABEL", ""))
    parser.add_argument(
        "--launchd-plist-path",
        default=os.environ.get(
            "AGENTD_CODEXW_PLIST_PATH",
            str(Path.home() / "Library/LaunchAgents/com.agentd.codexw-connector.plist"),
        ),
    )
    parser.add_argument(
        "--launchd-self-test-plist-path",
        default=os.environ.get(
            "AGENTD_CODEXW_SELF_TEST_PLIST_PATH",
            str(Path.home() / "Library/LaunchAgents/com.agentd.codexw-connector.self-test.plist"),
        ),
    )
    parser.add_argument("--systemd-service", default="agentd-codexw-connector.service")
    parser.add_argument("--systemd-self-test-service", default="agentd-codexw-connector-self-test.service")
    parser.add_argument("--systemd-self-test-timer", default="agentd-codexw-connector-self-test.timer")
    parser.add_argument("--systemd-env-file", default="/etc/agentd/codexw-connector.env")
    parser.set_defaults(detected_platform=detected)
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    selected = args.detected_platform if args.platform == "auto" else args.platform
    report = launchd_report(args) if selected == "launchd" else systemd_report(args)
    report["ok"] = bool(report.get("self_test", {}).get("ok"))
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(text_summary(report))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
