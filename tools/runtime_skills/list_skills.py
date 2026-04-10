#!/usr/bin/env python3
"""List runtime skills from the local v0 catalog search path."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from runtime_skill_lib import default_catalog_roots, discover_skills, repo_root


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="List runtime skills")
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
        "--json",
        action="store_true",
        help="Emit machine-readable JSON",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).expanduser().resolve() if args.root else repo_root(Path.cwd())
    roots = default_catalog_roots(root) + [
        Path(path).expanduser().resolve() for path in args.catalog_root
    ]

    entries = discover_skills(roots)
    payload = [
        {
            "skill_id": entry.skill_id,
            "version": entry.manifest["version"],
            "kind": entry.manifest["kind"],
            "description": entry.manifest["description"],
            "category": entry.manifest.get("ui", {}).get("category", ""),
            "label": entry.manifest.get("ui", {}).get("label", ""),
            "source_manifest": str(entry.manifest_path),
            "root": str(entry.root),
        }
        for entry in entries
    ]

    if args.json:
        print(json.dumps(payload, indent=2, ensure_ascii=False))
        return 0

    for row in payload:
        suffix = f" [{row['category']}]" if row["category"] else ""
        print(
            f"{row['skill_id']} {row['version']} {row['kind']}{suffix} - {row['description']}"
        )
        print(f"  source: {row['source_manifest']}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
