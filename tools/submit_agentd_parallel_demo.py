#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from urllib.error import URLError
from urllib.request import Request, urlopen


DEFAULT_BASE = "http://127.0.0.1:8123"


def read_state_base(state_path: Path) -> str | None:
    if not state_path.exists():
        return None
    try:
        state = json.loads(state_path.read_text())
    except Exception:
        return None
    base = state.get("agentd_base")
    return base if isinstance(base, str) and base else None


def is_reachable(base: str) -> bool:
    url = base.rstrip("/") + "/api/v1/health"
    req = Request(url)
    try:
        with urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except URLError:
        return False


def run_generator(args: argparse.Namespace, output_path: Path) -> None:
    gen_path = Path(__file__).resolve().parent / "gen_agentd_parallel_demo.py"
    cmd = [
        sys.executable,
        str(gen_path),
        "--state",
        str(args.state),
        "--output",
        str(output_path),
        "--goal",
        args.goal,
        "--timeout-ms",
        str(args.timeout_ms),
        "--poll-ms",
        str(args.poll_ms),
        "--bearer-env",
        args.bearer_env,
    ]
    if args.targets:
        cmd.extend(["--targets", args.targets])
    subprocess.run(cmd, check=True)


def submit_workflow(base: str, token: str, payload: bytes) -> bytes:
    url = base.rstrip("/") + "/api/v1/workflow/submit"
    req = Request(
        url,
        data=payload,
        method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        },
    )
    with urlopen(req, timeout=30) as resp:
        return resp.read()


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate + submit agentd_parallel demo workflow JSON.")
    parser.add_argument(
        "--base",
        default="",
        help=f"agentd base URL (default: {DEFAULT_BASE} or from --state)",
    )
    parser.add_argument(
        "--token",
        default="",
        help="agentd auth token (default: AGENTD_AUTH_TOKEN or dev-agentd-token)",
    )
    parser.add_argument(
        "--state",
        default="out/devstack_state.json",
        help="devstack_state.json path (default: out/devstack_state.json)",
    )
    parser.add_argument(
        "--targets",
        default="",
        help="comma-separated agentd base URLs (overrides --state targets)",
    )
    parser.add_argument(
        "--output",
        default="out/workflows/agentd_parallel_demo.json",
        help="output workflow JSON path",
    )
    parser.add_argument(
        "--goal",
        default="Draft a collaborative plan for a multi-agent workflow graph demo.",
        help="workflow input goal",
    )
    parser.add_argument("--timeout-ms", type=int, default=120000, help="agentd_call timeout in ms")
    parser.add_argument("--poll-ms", type=int, default=200, help="agentd_call poll interval in ms")
    parser.add_argument(
        "--bearer-env",
        default="AGENTD_CALL_BEARER",
        help="bearer env var name",
    )
    parser.add_argument("--dry-run", action="store_true", help="generate JSON but do not submit")
    args = parser.parse_args()

    token = args.token or (os.environ.get("AGENTD_AUTH_TOKEN") or "dev-agentd-token")

    base = args.base.strip()
    if not base:
        base = read_state_base(Path(args.state)) or DEFAULT_BASE
    if not is_reachable(base):
        print(f"[submit-demo] warning: {base} not reachable; falling back to {DEFAULT_BASE}", file=sys.stderr)
        base = DEFAULT_BASE

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    run_generator(args, output_path)

    if args.dry_run:
        print(f"[submit-demo] dry-run: {output_path}")
        print(f"[submit-demo] agentd_base: {base}")
        return 0

    payload = output_path.read_bytes()
    resp = submit_workflow(base, token, payload)
    sys.stdout.buffer.write(resp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
