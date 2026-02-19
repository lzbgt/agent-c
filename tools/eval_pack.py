#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from datetime import datetime
from typing import Any, Dict, List, Optional, Tuple


VERSION = "eval_pack_v0"


def now_ts() -> str:
    return datetime.utcnow().strftime("%Y%m%d_%H%M%S")


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("eval pack must be a JSON object")
    return data


def safe_join(base: str, rel: str) -> str:
    if os.path.isabs(rel):
        raise ValueError(f"absolute path not allowed: {rel}")
    root = os.path.abspath(base)
    out = os.path.abspath(os.path.join(root, rel))
    if os.path.commonpath([root, out]) != root:
        raise ValueError(f"path escapes run dir: {rel}")
    return out


def get_json_path(obj: Any, path: str) -> Any:
    cur = obj
    if not path:
        return cur
    for part in path.split("."):
        if part == "":
            continue
        key = part
        idx: Optional[int] = None
        if "[" in part and part.endswith("]"):
            head, tail = part.split("[", 1)
            key = head
            idx_raw = tail[:-1]
            if idx_raw.isdigit():
                idx = int(idx_raw)
        if key:
            if not isinstance(cur, dict) or key not in cur:
                raise KeyError(path)
            cur = cur[key]
        if idx is not None:
            if not isinstance(cur, list) or idx >= len(cur):
                raise KeyError(path)
            cur = cur[idx]
    return cur


def check_file_exists(run_dir: str, check: Dict[str, Any]) -> Tuple[bool, str]:
    rel = str(check.get("path") or "")
    if not rel:
        return False, "file_exists missing path"
    path = safe_join(run_dir, rel)
    if os.path.exists(path):
        return True, f"exists: {rel}"
    return False, f"missing: {rel}"


def check_log_contains(run_dir: str, check: Dict[str, Any]) -> Tuple[bool, str]:
    rel = str(check.get("path") or "")
    if not rel:
        return False, "log_contains missing path"
    pattern = check.get("pattern")
    contains = check.get("contains")
    if pattern is None and contains is None:
        return False, "log_contains requires pattern or contains"
    path = safe_join(run_dir, rel)
    if not os.path.exists(path):
        return False, f"log missing: {rel}"
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        data = f.read()
    if pattern is not None:
        if re.search(str(pattern), data):
            return True, f"pattern matched: {rel}"
        return False, f"pattern not found: {rel}"
    if str(contains) in data:
        return True, f"contains matched: {rel}"
    return False, f"contains not found: {rel}"


def check_json_path(run_dir: str, check: Dict[str, Any]) -> Tuple[bool, str]:
    rel = str(check.get("path") or "")
    key = str(check.get("key") or "")
    if not rel or not key:
        return False, "json_path requires path + key"
    path = safe_join(run_dir, rel)
    if not os.path.exists(path):
        return False, f"json missing: {rel}"
    with open(path, "r", encoding="utf-8") as f:
        payload = json.load(f)
    try:
        value = get_json_path(payload, key)
    except KeyError:
        return False, f"json path missing: {key}"
    if "equals" in check:
        expected = check.get("equals")
        if value == expected:
            return True, f"json equals ok: {key}"
        return False, f"json equals mismatch: {key}"
    return True, f"json path exists: {key}"


def run_checks(run_dir: str, checks: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    results: List[Dict[str, Any]] = []
    for raw in checks:
        ctype = str(raw.get("type") or "").strip()
        if ctype == "file_exists":
            ok, msg = check_file_exists(run_dir, raw)
        elif ctype == "log_contains":
            ok, msg = check_log_contains(run_dir, raw)
        elif ctype == "json_path":
            ok, msg = check_json_path(run_dir, raw)
        else:
            ok, msg = False, f"unknown check type: {ctype}"
        results.append({"type": ctype, "ok": ok, "message": msg})
    return results


def run_scenario(path: str, out_dir: str, env: Dict[str, str]) -> None:
    sys.path.insert(0, os.path.dirname(__file__))
    import scenario_runner  # type: ignore

    prev_env = os.environ.copy()
    try:
        os.environ.update(env)
        scenario_runner.run_scenario(path, out_dir)
    finally:
        os.environ.clear()
        os.environ.update(prev_env)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run an eval pack (scenario + checks + scoring).")
    parser.add_argument("--file", required=True, help="eval pack JSON file")
    parser.add_argument("--out-dir", help="output root (default: out/eval_pack_<ts>)")
    parser.add_argument("--keep-going", action="store_true", help="continue running scenarios even if one fails")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    pack = load_json(args.file)
    if pack.get("version") not in (None, VERSION):
        raise SystemExit(f"unsupported eval pack version: {pack.get('version')}")

    scenarios = pack.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        raise SystemExit("eval pack missing scenarios")

    out_root = args.out_dir
    if not out_root:
        out_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "out", f"eval_pack_{now_ts()}"))
    os.makedirs(out_root, exist_ok=True)

    pack_env = {str(k): str(v) for k, v in (pack.get("env") or {}).items()}
    results: List[Dict[str, Any]] = []
    failures: List[str] = []
    total_score = 0.0
    max_score = 0.0
    started_at = datetime.utcnow().isoformat() + "Z"

    for entry in scenarios:
        if not isinstance(entry, dict):
            failures.append("invalid scenario entry")
            if not args.keep_going:
                break
            continue
        sid = str(entry.get("id") or entry.get("name") or "scenario")
        file_path = str(entry.get("file") or "")
        if not file_path:
            failures.append(f"{sid}: missing file")
            if not args.keep_going:
                break
            continue
        file_path = os.path.abspath(file_path)
        run_dir = os.path.join(out_root, str(entry.get("out_dir") or sid))
        weight = float(entry.get("score_weight") or 1.0)
        max_score += weight
        env = pack_env.copy()
        for k, v in (entry.get("env") or {}).items():
            env[str(k)] = str(v)
        ok = True
        error: Optional[str] = None
        try:
            run_scenario(file_path, run_dir, env)
            checks = entry.get("checks") or []
            if not isinstance(checks, list):
                raise RuntimeError("checks must be a list")
            check_results = run_checks(run_dir, checks)
            ok = all(item.get("ok") for item in check_results)
            if ok:
                total_score += weight
            results.append({
                "id": sid,
                "file": file_path,
                "run_dir": run_dir,
                "ok": ok,
                "score": weight if ok else 0.0,
                "checks": check_results,
            })
        except Exception as exc:
            ok = False
            error = str(exc)
            results.append({
                "id": sid,
                "file": file_path,
                "run_dir": run_dir,
                "ok": False,
                "score": 0.0,
                "error": error,
            })
            failures.append(f"{sid}: {error}")
            if not args.keep_going:
                break

    total = len(results)
    pass_rate = (sum(1 for r in results if r.get("ok")) / total) if total else 0.0
    thresholds = pack.get("threshold") or {}
    min_score = float(thresholds.get("min_score") or 0.0)
    min_pass_rate = float(thresholds.get("min_pass_rate") or 0.0)
    ok = total_score >= min_score and pass_rate >= min_pass_rate and not failures

    summary_path = os.path.join(out_root, "summary.json")
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump({
            "name": pack.get("name"),
            "version": pack.get("version") or VERSION,
            "started_at": started_at,
            "finished_at": datetime.utcnow().isoformat() + "Z",
            "results": results,
            "total_score": total_score,
            "max_score": max_score,
            "pass_rate": pass_rate,
            "threshold": {"min_score": min_score, "min_pass_rate": min_pass_rate},
            "failed": failures,
            "ok": ok,
        }, f, indent=2)

    if not ok:
        print("eval pack failed:")
        for item in failures:
            print(f"- {item}")
        print(f"summary: {summary_path}")
        return 1
    print(f"eval pack OK: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
