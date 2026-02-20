#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from typing import Any, Dict, List, Optional


TEMPLATE_RE = re.compile(r"\{\{\s*([^}]+?)\s*\}\}")


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("scenario must be a JSON object")
    return data


def now_ts() -> str:
    return datetime.utcnow().strftime("%Y%m%d_%H%M%S")


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def render_template(value: str, ctx: Dict[str, str]) -> str:
    def repl(match: re.Match) -> str:
        key = match.group(1).strip()
        if key.startswith("env."):
            return os.environ.get(key[4:], "")
        if key in ctx:
            return ctx.get(key, "")
        return os.environ.get(key, "")

    return TEMPLATE_RE.sub(repl, value)


def render_obj(obj: Any, ctx: Dict[str, str]) -> Any:
    if isinstance(obj, str):
        return render_template(obj, ctx)
    if isinstance(obj, list):
        return [render_obj(v, ctx) for v in obj]
    if isinstance(obj, dict):
        return {k: render_obj(v, ctx) for k, v in obj.items()}
    return obj


def run_shell(step: Dict[str, Any], ctx: Dict[str, str], logs_dir: str, index: int) -> None:
    cmd_raw = step.get("cmd")
    if not isinstance(cmd_raw, str) or not cmd_raw.strip():
        raise ValueError("shell step missing cmd")
    cmd = render_template(cmd_raw, ctx)
    cwd = step.get("cwd")
    if isinstance(cwd, str) and cwd.strip():
        cwd = render_template(cwd, ctx)
    else:
        cwd = None
    env = os.environ.copy()
    for k, v in (step.get("env") or {}).items():
        env[str(k)] = render_template(str(v), ctx)
    for key, value in ctx.items():
        if key.startswith("env.") and key[4:]:
            env.setdefault(key[4:], value)

    name = step.get("name") or f"shell_{index}"
    log_path = os.path.join(logs_dir, f"{index:02d}_{name}.log")
    with open(log_path, "w", encoding="utf-8") as log:
        log.write(f"$ {cmd}\n")
        log.flush()
        proc = subprocess.run(cmd, shell=True, cwd=cwd, env=env, stdout=log, stderr=log, timeout=step.get("timeout_s"))
    if proc.returncode != 0 and step.get("allow_failure") is not True:
        raise RuntimeError(f"shell step failed: {name} (rc={proc.returncode}) log={log_path}")


def run_sleep(step: Dict[str, Any]) -> None:
    seconds = step.get("seconds")
    if seconds is None:
        seconds = step.get("duration_s", 0)
    try:
        sec = float(seconds)
    except Exception:
        sec = 0
    if sec > 0:
        time.sleep(sec)


def run_capture(step: Dict[str, Any], ctx: Dict[str, str], logs_dir: str, index: int) -> None:
    args = step.get("args") or []
    if not isinstance(args, list):
        raise ValueError("capture_evidence args must be a list")
    args = [render_template(str(a), ctx) for a in args]

    name = step.get("name") or f"capture_{index}"
    log_path = os.path.join(logs_dir, f"{index:02d}_{name}.log")
    cmd = ["/usr/bin/env", "bash", os.path.join(os.path.dirname(__file__), "capture_agent_evidence_bundle.sh"), *args]
    with open(log_path, "w", encoding="utf-8") as log:
        log.write("$ " + " ".join(cmd) + "\n")
        log.flush()
        proc = subprocess.run(cmd, stdout=log, stderr=log)
    if proc.returncode != 0 and step.get("allow_failure") is not True:
        raise RuntimeError(f"capture step failed: {name} (rc={proc.returncode}) log={log_path}")

    # Best-effort parse evidence dir from log.
    try:
        with open(log_path, "r", encoding="utf-8") as log:
            for line in log:
                if "Bundle captured at" in line:
                    parts = line.strip().split("Bundle captured at", 1)
                    if len(parts) == 2:
                        ctx["evidence_dir"] = parts[1].strip()
    except Exception:
        pass


def run_http(step: Dict[str, Any], ctx: Dict[str, str], logs_dir: str, index: int) -> None:
    import ssl
    import urllib.request

    url_raw = step.get("url")
    if not isinstance(url_raw, str) or not url_raw.strip():
        raise ValueError("http step missing url")
    url = render_template(url_raw, ctx)
    method = str(step.get("method") or "GET").upper()
    headers = {str(k): render_template(str(v), ctx) for k, v in (step.get("headers") or {}).items()}
    body = step.get("body")
    data: Optional[bytes] = None
    if body is not None:
        rendered = render_obj(body, ctx)
        if isinstance(rendered, (dict, list)):
            payload = json.dumps(rendered).encode("utf-8")
            headers.setdefault("Content-Type", "application/json")
            data = payload
        else:
            data = str(rendered).encode("utf-8")

    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    name = step.get("name") or f"http_{index}"
    log_path = os.path.join(logs_dir, f"{index:02d}_{name}.log")
    ctx_ssl = None
    if step.get("insecure") is True and url.startswith("https://"):
        ctx_ssl = ssl._create_unverified_context()

    with open(log_path, "w", encoding="utf-8") as log:
        log.write(f"{method} {url}\n")
        for hk, hv in headers.items():
            log.write(f"  {hk}: {hv}\n")
        log.flush()
        try:
            with urllib.request.urlopen(req, context=ctx_ssl, timeout=step.get("timeout_s")) as resp:
                status = resp.status
                body_bytes = resp.read()
                log.write(f"status: {status}\n")
                log.write(body_bytes.decode("utf-8", errors="replace"))
                if step.get("save_as"):
                    save_path = os.path.join(logs_dir, render_template(str(step["save_as"]), ctx))
                    with open(save_path, "wb") as out:
                        out.write(body_bytes)
                if step.get("expect_status"):
                    want = step.get("expect_status")
                    if isinstance(want, list):
                        if status not in want:
                            raise RuntimeError(f"unexpected status {status}")
                    elif status != int(want):
                        raise RuntimeError(f"unexpected status {status}")
        except Exception as exc:
            if step.get("allow_failure") is True:
                log.write(f"error: {exc}\n")
            else:
                raise


def run_set(step: Dict[str, Any], ctx: Dict[str, str]) -> None:
    key = step.get("key")
    value = step.get("value")
    if not key:
        raise ValueError("set step missing key")
    ctx[str(key)] = render_template(str(value or ""), ctx)


def run_scenario(path: str, out_dir: str) -> None:
    data = load_json(path)
    steps = data.get("steps")
    if not isinstance(steps, list) or not steps:
        raise ValueError("scenario missing steps")

    ensure_dir(out_dir)
    logs_dir = os.path.join(out_dir, "logs")
    ensure_dir(logs_dir)

    ctx: Dict[str, str] = {
        "run_dir": out_dir,
        "scenario": str(data.get("name") or "scenario"),
        "evidence_dir": "",
    }
    ctx["env.BROKER_PUBLISHED_PORT"] = os.environ.get("BROKER_PUBLISHED_PORT") or "8443"
    ctx["env.AGENTD_PUBLISHED_PORT"] = os.environ.get("AGENTD_PUBLISHED_PORT") or "8123"

    for idx, raw in enumerate(steps, start=1):
        if not isinstance(raw, dict):
            raise ValueError("step must be an object")
        step = render_obj(raw, ctx)
        stype = str(step.get("type") or "").strip()
        if stype == "shell":
            run_shell(step, ctx, logs_dir, idx)
        elif stype == "sleep":
            run_sleep(step)
        elif stype == "capture_evidence":
            run_capture(step, ctx, logs_dir, idx)
        elif stype == "http":
            run_http(step, ctx, logs_dir, idx)
        elif stype == "set":
            run_set(step, ctx)
        else:
            raise ValueError(f"unknown step type: {stype}")

    meta_path = os.path.join(out_dir, "meta.json")
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump({
            "scenario": data.get("name"),
            "source": os.path.abspath(path),
            "run_dir": out_dir,
            "captured_at": datetime.utcnow().isoformat() + "Z",
            "evidence_dir": ctx.get("evidence_dir"),
        }, f, indent=2)

    print(out_dir)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a JSON scenario (agentd/broker).")
    parser.add_argument("--file", required=True, help="scenario JSON file")
    parser.add_argument("--out-dir", help="output dir (default: out/scenario_<ts>)")
    args = parser.parse_args()

    out_dir = args.out_dir
    if not out_dir:
        out_dir = os.path.join(os.path.dirname(__file__), "..", "out", f"scenario_{now_ts()}")
    out_dir = os.path.abspath(out_dir)

    try:
        run_scenario(args.file, out_dir)
    except Exception as exc:
        print(f"scenario failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
