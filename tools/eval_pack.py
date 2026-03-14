#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple


VERSION = "eval_pack_v0"
BASELINE_VERSION = "eval_pack_baseline_v1"


def now_ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("eval pack must be a JSON object")
    return data


def relpath_or_abs(path: str, root: str) -> str:
    try:
        rel = os.path.relpath(path, root)
    except ValueError:
        return os.path.abspath(path)
    if rel.startswith(".."):
        return os.path.abspath(path)
    return rel


def resolve_repo_baseline_path(pack_path: str, repo_root: str) -> str:
    packs_root = os.path.join(repo_root, "tools", "eval_packs")
    pack_abs = os.path.abspath(pack_path)
    if os.path.commonpath([packs_root, pack_abs]) != packs_root:
        raise ValueError("auto baseline resolution only supports packs under tools/eval_packs/")
    stem, ext = os.path.splitext(os.path.basename(pack_abs))
    if ext.lower() != ".json" or not stem:
        raise ValueError("auto baseline resolution requires a .json pack file")
    return os.path.join(repo_root, "ref", "eval_packs", f"{stem}.summary.json")


def resolve_baseline_path(baseline_arg: Optional[str], pack_path: str, repo_root: str, update_baseline: bool) -> Optional[str]:
    if baseline_arg:
        if baseline_arg == "auto":
            return resolve_repo_baseline_path(pack_path, repo_root)
        return os.path.abspath(baseline_arg)
    if update_baseline:
        return resolve_repo_baseline_path(pack_path, repo_root)
    return None


def baseline_result_row(row: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "id": str(row.get("id") or ""),
        "ok": bool(row.get("ok")),
        "score": float(row.get("score") or 0.0),
    }
    checks = row.get("checks")
    if isinstance(checks, list):
        normalized_checks: List[Dict[str, Any]] = []
        for check in checks:
            if not isinstance(check, dict):
                continue
            normalized_checks.append({
                "type": str(check.get("type") or ""),
                "ok": bool(check.get("ok")),
                "message": str(check.get("message") or ""),
            })
        out["checks"] = normalized_checks
    if "error" in row:
        out["error"] = str(row.get("error") or "")
    return out


def make_baseline_payload(summary: Dict[str, Any], pack_path: str, repo_root: str) -> Dict[str, Any]:
    results = summary.get("results") or []
    normalized_results = []
    if isinstance(results, list):
        normalized_results = [baseline_result_row(row) for row in results if isinstance(row, dict)]
    threshold = summary.get("threshold") or {}
    return {
        "baseline_version": BASELINE_VERSION,
        "name": summary.get("name"),
        "version": summary.get("version") or VERSION,
        "pack": relpath_or_abs(pack_path, repo_root),
        "results": normalized_results,
        "total_score": float(summary.get("total_score") or 0.0),
        "max_score": float(summary.get("max_score") or 0.0),
        "pass_rate": float(summary.get("pass_rate") or 0.0),
        "threshold": {
            "min_score": float(threshold.get("min_score") or 0.0),
            "min_pass_rate": float(threshold.get("min_pass_rate") or 0.0),
        },
        "failed": [str(item) for item in (summary.get("failed") or [])],
        "ok": bool(summary.get("ok")),
    }


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


def check_json_len(run_dir: str, check: Dict[str, Any]) -> Tuple[bool, str]:
    rel = str(check.get("path") or "")
    key = str(check.get("key") or "")
    if not rel or not key:
        return False, "json_len requires path + key"
    path = safe_join(run_dir, rel)
    if not os.path.exists(path):
        return False, f"json missing: {rel}"
    with open(path, "r", encoding="utf-8") as f:
        payload = json.load(f)
    try:
        value = get_json_path(payload, key)
    except KeyError:
        return False, f"json path missing: {key}"
    if isinstance(value, (list, dict, str)):
        length = len(value)
    else:
        return False, f"json_len unsupported type: {type(value).__name__}"
    expected = check.get("equals")
    if expected is not None:
        if length == int(expected):
            return True, f"json_len equals ok: {key}"
        return False, f"json_len equals mismatch: {key}"
    min_v = check.get("min")
    max_v = check.get("max")
    if min_v is not None and length < int(min_v):
        return False, f"json_len below min: {key}"
    if max_v is not None and length > int(max_v):
        return False, f"json_len above max: {key}"
    return True, f"json_len ok: {key}"


def check_json_number(run_dir: str, check: Dict[str, Any]) -> Tuple[bool, str]:
    rel = str(check.get("path") or "")
    key = str(check.get("key") or "")
    if not rel or not key:
        return False, "json_number requires path + key"
    path = safe_join(run_dir, rel)
    if not os.path.exists(path):
        return False, f"json missing: {rel}"
    with open(path, "r", encoding="utf-8") as f:
        payload = json.load(f)
    try:
        value = get_json_path(payload, key)
    except KeyError:
        return False, f"json path missing: {key}"
    if not isinstance(value, (int, float)):
        return False, f"json_number unsupported type: {type(value).__name__}"
    expected = check.get("equals")
    if expected is not None:
        if value == expected:
            return True, f"json_number equals ok: {key}"
        return False, f"json_number equals mismatch: {key}"
    min_v = check.get("min")
    max_v = check.get("max")
    if min_v is not None and value < float(min_v):
        return False, f"json_number below min: {key}"
    if max_v is not None and value > float(max_v):
        return False, f"json_number above max: {key}"
    return True, f"json_number ok: {key}"


def check_file_sha256(run_dir: str, check: Dict[str, Any]) -> Tuple[bool, str]:
    import hashlib

    rel = str(check.get("path") or "")
    expected = str(check.get("sha256") or "")
    if not rel or not expected:
        return False, "file_sha256 requires path + sha256"
    path = safe_join(run_dir, rel)
    if not os.path.exists(path):
        return False, f"missing: {rel}"
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    digest = h.hexdigest()
    if digest == expected:
        return True, f"sha256 ok: {rel}"
    return False, f"sha256 mismatch: {rel}"


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
        elif ctype == "json_len":
            ok, msg = check_json_len(run_dir, raw)
        elif ctype == "json_number":
            ok, msg = check_json_number(run_dir, raw)
        elif ctype == "file_sha256":
            ok, msg = check_file_sha256(run_dir, raw)
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
    parser.add_argument("--baseline", help="compare results to a baseline summary.json, or use 'auto' for ref/eval_packs/<pack>.summary.json")
    parser.add_argument("--update-baseline", action="store_true", help="write the current normalized baseline summary to --baseline (defaults to auto)")
    return parser.parse_args()


def compare_baseline(current: Dict[str, Any], baseline: Dict[str, Any]) -> Tuple[bool, List[str]]:
    issues: List[str] = []
    try:
        base_total = float(baseline.get("total_score") or 0.0)
    except (TypeError, ValueError):
        base_total = 0.0
    try:
        cur_total = float(current.get("total_score") or 0.0)
    except (TypeError, ValueError):
        cur_total = 0.0
    if cur_total < base_total:
        issues.append(f"total_score regressed: {cur_total} < {base_total}")
    try:
        base_pass = float(baseline.get("pass_rate") or 0.0)
    except (TypeError, ValueError):
        base_pass = 0.0
    try:
        cur_pass = float(current.get("pass_rate") or 0.0)
    except (TypeError, ValueError):
        cur_pass = 0.0
    if cur_pass < base_pass:
        issues.append(f"pass_rate regressed: {cur_pass} < {base_pass}")

    if baseline.get("ok") and not current.get("ok"):
        issues.append("baseline ok=true but current ok=false")

    base_results = baseline.get("results") or []
    cur_results = current.get("results") or []
    if isinstance(base_results, list) and isinstance(cur_results, list):
        cur_map = {str(r.get("id")): bool(r.get("ok")) for r in cur_results if isinstance(r, dict)}
        for row in base_results:
            if not isinstance(row, dict):
                continue
            sid = str(row.get("id"))
            if sid and row.get("ok") and not cur_map.get(sid, False):
                issues.append(f"scenario regressed: {sid}")

    return len(issues) == 0, issues


def main() -> int:
    args = parse_args()
    pack_path = os.path.abspath(args.file)
    pack_dir = os.path.dirname(pack_path)
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    pack = load_json(pack_path)
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
    started_at = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")

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
        if not os.path.isabs(file_path):
            candidate = os.path.abspath(os.path.join(pack_dir, file_path))
            if os.path.exists(candidate):
                file_path = candidate
            else:
                fallback = os.path.abspath(os.path.join(repo_root, file_path))
                if os.path.exists(fallback):
                    file_path = fallback
                else:
                    file_path = candidate
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
    summary_payload = {
        "name": pack.get("name"),
        "version": pack.get("version") or VERSION,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "results": results,
        "total_score": total_score,
        "max_score": max_score,
        "pass_rate": pass_rate,
        "threshold": {"min_score": min_score, "min_pass_rate": min_pass_rate},
        "failed": failures,
        "ok": ok,
    }
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary_payload, f, indent=2)

    baseline_path = resolve_baseline_path(args.baseline, pack_path, repo_root, args.update_baseline)
    if baseline_path:
        print(f"baseline: {baseline_path}")
        if args.update_baseline:
            os.makedirs(os.path.dirname(baseline_path), exist_ok=True)
            baseline_payload = make_baseline_payload(summary_payload, pack_path, repo_root)
            with open(baseline_path, "w", encoding="utf-8") as f:
                json.dump(baseline_payload, f, indent=2)
        else:
            try:
                with open(baseline_path, "r", encoding="utf-8") as f:
                    baseline_payload = json.load(f)
                same, issues = compare_baseline(summary_payload, baseline_payload)
                if not same:
                    ok = False
                    print("baseline regressions detected:")
                    for issue in issues:
                        print(f"- {issue}")
            except FileNotFoundError:
                ok = False
                print(f"baseline missing: {baseline_path}")
            except Exception as exc:
                ok = False
                print(f"baseline check failed: {exc}")

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
