#!/usr/bin/env python3
"""
Fetch and filter OpenRouter's model catalog.

Focus:
- Find models that support multimodal inputs (image/audio/video in input_modalities).
- Filter by pricing range (USD per 1M tokens), default: $0.01 to $0.50 (prompt+completion total).
- Sort by total price (prompt+completion per 1M tokens).

This script is a host/dev tool. It is intentionally NOT part of the portable core library.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import json
import os
import pathlib
import re
import sys
import urllib.request
import urllib.error
from typing import Any, Dict, List, Optional, Tuple


PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_CATALOG_URL = "https://openrouter.ai/api/v1/models"


def read_project_local_md_key(provider: str) -> Optional[str]:
    project_md = PROJECT_ROOT / "project.local.md"
    if not project_md.exists():
        return None
    # Lines are like:
    # - openrouter: sk-or-v1-...
    # - deepseek: sk-...
    pat = re.compile(rf"^\s*-\s*{re.escape(provider)}:\s*(\S+)\s*$")
    for line in project_md.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pat.match(line)
        if m:
            return m.group(1)
    return None


def read_not_in_repo_key(provider: str) -> Optional[str]:
    not_in_repo = PROJECT_ROOT / ".not_in_repo"
    if not_in_repo.exists():
        pat = re.compile(rf"^\s*-\s*{re.escape(provider)}:\s*(\S+)\s*$")
        for line in not_in_repo.read_text(encoding="utf-8", errors="replace").splitlines():
            m = pat.match(line)
            if m:
                return m.group(1)

        env_pat = re.compile(
            r"^\s*(export\s+)?(OPENROUTER_API_KEY|OPENAI_API_KEY)\s*=\s*['\"]?(sk-[A-Za-z0-9_.-]+)['\"]?\s*(#.*)?$"
        )
        for line in not_in_repo.read_text(encoding="utf-8", errors="replace").splitlines():
            m = env_pat.match(line)
            if m:
                return m.group(3)
    return None


def read_home_env_key() -> Optional[str]:
    env_path = pathlib.Path.home() / ".env"
    if not env_path.exists():
        return None
    env_pat = re.compile(
        r"^\s*(export\s+)?(OPENROUTER_API_KEY|OPENAI_API_KEY)\s*=\s*['\"]?(sk-[A-Za-z0-9_.-]+)['\"]?\s*(#.*)?$"
    )
    for line in env_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = env_pat.match(line)
        if m:
            return m.group(3)
    return None


def get_openrouter_key(args_key: Optional[str]) -> Optional[str]:
    if args_key:
        return args_key
    env_key = os.environ.get("OPENROUTER_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if env_key:
        return env_key
    not_in_repo_key = read_not_in_repo_key("openrouter")
    if not_in_repo_key:
        return not_in_repo_key
    project_key = read_project_local_md_key("openrouter")
    if project_key:
        return project_key
    return read_home_env_key()


def fetch_json(url: str, headers: Dict[str, str], timeout_s: int, use_proxy: bool) -> Dict[str, Any]:
    req = urllib.request.Request(url, headers=headers, method="GET")
    # By default urllib uses env proxy variables. Allow disabling for deterministic runs.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}) if not use_proxy else urllib.request.ProxyHandler())
    with opener.open(req, timeout=timeout_s) as resp:
        raw = resp.read()
        encoding = resp.headers.get("Content-Encoding", "")
        if encoding.lower() == "gzip":
            raw = gzip.decompress(raw)
        return json.loads(raw.decode("utf-8"))


def load_cached_catalog() -> Optional[Dict[str, Any]]:
    cached = PROJECT_ROOT / "ref" / "openrouter" / "models_raw.json"
    if not cached.exists():
        return None
    try:
        return json.loads(cached.read_text(encoding="utf-8"))
    except Exception:
        return None


def pricing_to_per_million(pricing_value: Any) -> float:
    # OpenRouter returns pricing as strings like "0.000000075" USD per token.
    # Convert to USD per 1M tokens.
    try:
        per_token = float(pricing_value)
    except Exception:
        per_token = 0.0
    return per_token * 1_000_000.0


def is_multimodal_input(model: Dict[str, Any]) -> bool:
    arch = model.get("architecture") or {}
    inputs = arch.get("input_modalities") or []
    if not isinstance(inputs, list):
        return False
    for m in inputs:
        if m in ("image", "audio", "video"):
            return True
    return False


def supports_tools(model: Dict[str, Any]) -> bool:
    sp = model.get("supported_parameters") or []
    if not isinstance(sp, list):
        return False
    # OpenRouter's model catalog indicates tool calling support via supported_parameters.
    return "tools" in sp


def filter_models(
    models: List[Dict[str, Any]],
    min_total: float,
    max_total: float,
    require_multimodal_input: bool,
    require_tools: bool,
    include_free: bool,
) -> List[Tuple[float, float, float, Dict[str, Any]]]:
    out: List[Tuple[float, float, float, Dict[str, Any]]] = []
    for m in models:
        pricing = m.get("pricing") or {}
        prompt_pm = pricing_to_per_million(pricing.get("prompt", 0))
        completion_pm = pricing_to_per_million(pricing.get("completion", 0))
        total = prompt_pm + completion_pm

        if not include_free and total <= 0.0:
            continue
        if require_multimodal_input and not is_multimodal_input(m):
            continue
        if require_tools and not supports_tools(m):
            continue
        if total < min_total or total > max_total:
            continue
        out.append((total, prompt_pm, completion_pm, m))
    out.sort(key=lambda t: (t[0], t[1], t[2], t[3].get("id", "")))
    return out


def render_markdown(rows: List[Tuple[float, float, float, Dict[str, Any]]], limit: int) -> str:
    lines: List[str] = []
    lines.append("| total $/1M | prompt $/1M | completion $/1M | tools | id | ctx | input | output |")
    lines.append("|---:|---:|---:|:---:|---|---:|---|---|")
    for total, prompt_pm, completion_pm, m in rows[:limit]:
        arch = m.get("architecture") or {}
        inputs = ",".join(arch.get("input_modalities") or [])
        outputs = ",".join(arch.get("output_modalities") or [])
        tools = "yes" if supports_tools(m) else ""
        ctx = m.get("context_length") or 0
        mid = m.get("id", "")
        lines.append(
            f"| {total:.3f} | {prompt_pm:.3f} | {completion_pm:.3f} | {tools} | `{mid}` | {ctx} | {inputs} | {outputs} |"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=DEFAULT_CATALOG_URL)
    ap.add_argument("--key", default=None, help="OpenRouter API key (defaults: env or project.local.md)")
    ap.add_argument("--timeout-s", type=int, default=60)
    ap.add_argument("--min-total", type=float, default=0.01, help="Min total prompt+completion $/1M")
    ap.add_argument("--max-total", type=float, default=0.50, help="Max total prompt+completion $/1M")
    ap.add_argument("--include-free", action="store_true")
    ap.add_argument("--require-multimodal-input", dest="require_multimodal_input", action="store_true", default=True)
    ap.add_argument(
        "--allow-text-only",
        dest="require_multimodal_input",
        action="store_false",
        help="Do not require image/audio/video input modalities",
    )
    ap.add_argument("--require-tools", dest="require_tools", action="store_true", default=True)
    ap.add_argument(
        "--allow-no-tools",
        dest="require_tools",
        action="store_false",
        help="Do not require OpenAI tools/tool_choice support in supported_parameters",
    )
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--write", action="store_true", help="Write ref/openrouter/models_raw.json and ref/openrouter/multimodal_latest.md")
    ap.add_argument("--snapshot", action="store_true", help="Also write timestamped snapshot files under ref/openrouter/")
    ap.add_argument("--offline", action="store_true", help="Use cached ref/openrouter/models_raw.json if available")
    ap.add_argument("--no-proxy", action="store_true", help="Disable env proxy usage for fetching")
    args = ap.parse_args()

    key = get_openrouter_key(args.key)
    if not key:
        print("Missing OpenRouter key: provide --key, set OPENROUTER_API_KEY, or add to project.local.md", file=sys.stderr)
        return 2

    headers = {
        "Authorization": f"Bearer {key}",
        "Accept": "application/json",
        "Accept-Encoding": "gzip",
        "User-Agent": "agent-tools/0.1",
    }

    payload: Optional[Dict[str, Any]] = None
    if args.offline:
        payload = load_cached_catalog()
        if payload is None:
            print("Offline requested but no cached catalog found at ref/openrouter/models_raw.json", file=sys.stderr)
            return 2
    else:
        try:
            payload = fetch_json(args.url, headers=headers, timeout_s=args.timeout_s, use_proxy=(not args.no_proxy))
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as e:
            cached = load_cached_catalog()
            if cached is None:
                print(f"Failed to fetch catalog and no cache available: {e}", file=sys.stderr)
                return 1
            payload = cached
    data = payload.get("data")
    if not isinstance(data, list):
        print("Unexpected response shape (missing .data array).", file=sys.stderr)
        return 1

    rows = filter_models(
        data,
        min_total=args.min_total,
        max_total=args.max_total,
        require_multimodal_input=args.require_multimodal_input,
        require_tools=args.require_tools,
        include_free=args.include_free,
    )

    md = render_markdown(rows, limit=args.limit)
    sys.stdout.write(md)

    if args.write:
        out_dir = PROJECT_ROOT / "ref" / "openrouter"
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "models_raw.json").write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
        (out_dir / "multimodal_latest.md").write_text(md, encoding="utf-8")
        if args.snapshot:
            ts = dt.datetime.now(dt.UTC).strftime("%Y%m%d_%H%M%S")
            raw_path = out_dir / f"models_{ts}.json"
            report_path = out_dir / f"multimodal_{args.min_total:.2f}_to_{args.max_total:.2f}_{ts}.md"
            raw_path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
            report_path.write_text(md, encoding="utf-8")

    # Emit a recommended model id to stderr for scripting:
    # pick the cheapest entry.
    if rows:
        recommended = rows[0][3].get("id", "")
        print(f"RECOMMENDED_MODEL={recommended}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
