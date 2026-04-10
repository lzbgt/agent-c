#!/usr/bin/env python3
"""Resolve a runtime skill by skill_id into a materialized JSON document."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict

from runtime_skill_lib import (
    RuntimeSkillError,
    build_resolution_document,
    default_catalog_roots,
    find_skill,
    load_data_file,
    merge_capabilities,
    repo_root,
)


def _load_inputs(args: argparse.Namespace) -> Dict[str, Any]:
    if args.inputs_json and args.inputs_file:
        raise RuntimeSkillError("use either --inputs-json or --inputs-file, not both")
    if args.inputs_json:
        try:
            data = json.loads(args.inputs_json)
        except json.JSONDecodeError as exc:
            raise RuntimeSkillError(f"invalid JSON passed to --inputs-json: {exc}") from exc
    elif args.inputs_file:
        data = load_data_file(Path(args.inputs_file).expanduser().resolve())
    else:
        data = {}

    if data is None:
        data = {}
    if not isinstance(data, dict):
        raise RuntimeSkillError("runtime skill inputs must be an object")
    return dict(data)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Resolve a runtime skill by skill_id")
    parser.add_argument("skill_id", help="Runtime skill identifier")
    parser.add_argument(
        "--root",
        default=None,
        help="Repo root (default: git root of cwd)",
    )
    parser.add_argument(
        "--catalog-root",
        action="append",
        default=[],
        help="Additional catalog root(s) to search after the default roots",
    )
    parser.add_argument(
        "--inputs-json",
        default=None,
        help="Inline JSON object to validate and materialize as skill inputs",
    )
    parser.add_argument(
        "--inputs-file",
        default=None,
        help="JSON or YAML file containing the skill inputs",
    )
    parser.add_argument(
        "--available-capabilities",
        default=None,
        help="JSON or YAML file with tools/plugins/features arrays",
    )
    parser.add_argument("--tool", action="append", default=[], help="Available tool name")
    parser.add_argument(
        "--plugin", action="append", default=[], help="Available plugin name"
    )
    parser.add_argument(
        "--feature", action="append", default=[], help="Available runtime feature"
    )
    parser.add_argument(
        "--out",
        default=None,
        help="Optional path to write the resolved JSON document",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).expanduser().resolve() if args.root else repo_root(Path.cwd())
    roots = default_catalog_roots(root) + [
        Path(path).expanduser().resolve() for path in args.catalog_root
    ]

    try:
        inputs = _load_inputs(args)
        capabilities = merge_capabilities(
            capabilities_file=(
                Path(args.available_capabilities).expanduser().resolve()
                if args.available_capabilities
                else None
            ),
            tools=args.tool,
            plugins=args.plugin,
            features=args.feature,
        )
        entry = find_skill(args.skill_id, roots)
        resolved = build_resolution_document(
            entry,
            inputs=inputs,
            capabilities=capabilities if any(capabilities.values()) else None,
        )
    except RuntimeSkillError as exc:
        print(exc, file=sys.stderr)
        return 1

    payload = json.dumps(resolved, indent=2, ensure_ascii=False) + "\n"
    if args.out:
        out_path = Path(args.out).expanduser().resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(payload, encoding="utf-8")
    sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
