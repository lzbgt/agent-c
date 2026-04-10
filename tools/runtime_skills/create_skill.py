#!/usr/bin/env python3
"""Create a new runtime skill directory from the sample template."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

from runtime_skill_lib import repo_root


def _abort(msg: str) -> None:
    raise SystemExit(msg)


def _ensure_empty(dst: Path) -> None:
    if dst.exists():
        _abort(f"destination already exists: {dst}")
    dst.parent.mkdir(parents=True, exist_ok=True)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Create a runtime skill from the template")
    parser.add_argument("name", help="Skill name (directory under tools/runtime_skills/local)")
    parser.add_argument(
        "--dest",
        default=None,
        help="Destination directory (default: tools/runtime_skills/local/<name>)",
    )
    args = parser.parse_args(argv)

    root = repo_root(Path.cwd())
    template = root / "tools" / "runtime_skills" / "templates" / "sample-runtime-skill"
    if not template.exists():
        _abort(f"template not found: {template}")

    dest = (
        Path(args.dest).expanduser().resolve()
        if args.dest
        else root / "tools" / "runtime_skills" / "local" / args.name
    )
    _ensure_empty(dest)

    shutil.copytree(template, dest)
    print(f"created: {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
