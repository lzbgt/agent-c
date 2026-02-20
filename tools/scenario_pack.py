#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from typing import List, Optional


def now_ts() -> str:
    return datetime.utcnow().strftime("%Y%m%d_%H%M%S")


def list_scenarios(dir_path: str) -> List[str]:
    out: List[str] = []
    for name in sorted(os.listdir(dir_path)):
        if not name.endswith(".json"):
            continue
        out.append(os.path.join(dir_path, name))
    return out


def read_meta(path: str) -> Optional[dict]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def run_scenario(path: str, out_dir: str) -> str:
    sys.path.insert(0, os.path.dirname(__file__))
    import scenario_runner  # type: ignore

    scenario_runner.run_scenario(path, out_dir)
    return out_dir


def validate_evidence(evidence_dir: str, strict: bool, require_agentd: bool, require_broker: bool) -> int:
    cmd = [sys.executable, os.path.join(os.path.dirname(__file__), "check_agent_evidence_bundle.py"), "--dir", evidence_dir]
    if strict:
        cmd.append("--strict")
    if require_agentd:
        cmd.append("--require-agentd")
    if require_broker:
        cmd.append("--require-broker")
    return subprocess.run(cmd, check=False).returncode


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run multiple scenarios and optionally validate evidence bundles.")
    parser.add_argument("--dir", default=os.path.join(os.path.dirname(__file__), "scenarios"), help="scenario directory")
    parser.add_argument("--file", action="append", default=[], help="scenario file (repeatable)")
    parser.add_argument("--out-dir", help="output root (default: out/scenario_pack_<ts>)")
    parser.add_argument("--validate", action="store_true", help="validate evidence bundles if present")
    parser.add_argument("--strict", action="store_true", help="treat warnings as errors in validation")
    parser.add_argument("--require-agentd", action="store_true", help="require agentd evidence")
    parser.add_argument("--require-broker", action="store_true", help="require broker evidence")
    parser.add_argument("--keep-going", action="store_true", help="continue running scenarios even if one fails")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    scenarios = args.file[:] if args.file else list_scenarios(args.dir)
    if not scenarios:
        print("no scenarios found", file=sys.stderr)
        return 2

    pack_started_at = datetime.utcnow().isoformat() + "Z"
    pack_t0 = time.time()
    out_root = args.out_dir
    if not out_root:
        out_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "out", f"scenario_pack_{now_ts()}"))
    os.makedirs(out_root, exist_ok=True)

    failures: List[str] = []
    results = []

    for path in scenarios:
        name = os.path.splitext(os.path.basename(path))[0]
        run_dir = os.path.join(out_root, name)
        started_at = datetime.utcnow().isoformat() + "Z"
        t0 = time.time()
        try:
            run_scenario(path, run_dir)
            meta = read_meta(os.path.join(run_dir, "meta.json")) or {}
            evidence_dir = meta.get("evidence_dir") or ""
            validate_rc = 0
            if args.validate and evidence_dir:
                validate_rc = validate_evidence(str(evidence_dir), args.strict, args.require_agentd, args.require_broker)
                if validate_rc != 0:
                    raise RuntimeError(f"evidence validation failed: {evidence_dir}")
            results.append({
                "scenario": name,
                "run_dir": run_dir,
                "evidence_dir": evidence_dir,
                "ok": True,
                "started_at": started_at,
                "duration_s": round(time.time() - t0, 3),
            })
        except Exception as exc:
            failures.append(f"{name}: {exc}")
            results.append({
                "scenario": name,
                "run_dir": run_dir,
                "ok": False,
                "error": str(exc),
                "started_at": started_at,
                "duration_s": round(time.time() - t0, 3),
            })
            if not args.keep_going:
                break

    summary_path = os.path.join(out_root, "summary.json")
    pack_finished_at = datetime.utcnow().isoformat() + "Z"
    total = len(results)
    failed_count = len(failures)
    ok_count = total - failed_count
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump({
            "results": results,
            "failed": failures,
            "total": total,
            "ok_count": ok_count,
            "failed_count": failed_count,
            "ok": failed_count == 0,
            "started_at": pack_started_at,
            "finished_at": pack_finished_at,
            "duration_s": round(time.time() - pack_t0, 3),
        }, f, indent=2)

    if failures:
        print("scenario pack failed:")
        for item in failures:
            print(f"- {item}")
        print(f"summary: {summary_path}")
        return 1

    print(f"scenario pack OK: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
