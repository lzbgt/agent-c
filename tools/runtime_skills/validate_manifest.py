#!/usr/bin/env python3
"""Validate runtime skill manifest(s) against the local v0 rules."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from runtime_skill_lib import RuntimeSkillError, iter_manifest_paths, load_manifest


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Validate runtime skill manifest(s)")
    parser.add_argument(
        "paths", nargs="+", help="Runtime skill dir(s) or manifest.json files"
    )
    args = parser.parse_args(argv)

    manifests = iter_manifest_paths([Path(path).expanduser().resolve() for path in args.paths])
    if not manifests:
        print("no manifests found", file=sys.stderr)
        return 2

    errors = 0
    for manifest in manifests:
        try:
            load_manifest(manifest)
            print(f"OK: {manifest}")
        except RuntimeSkillError as exc:
            errors += 1
            print(f"FAIL: {manifest}: {exc}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
