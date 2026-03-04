#!/usr/bin/env python3
"""Print applied skill history from state/skills_state.json."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List


def _load_state(path: Path) -> List[Dict[str, Any]]:
    if not path.exists():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    if isinstance(data, list):
        return [d for d in data if isinstance(d, dict)]
    return []


def _format_table(entries: List[Dict[str, Any]]) -> str:
    if not entries:
        return "No applied skills found."
    rows = []
    for entry in entries:
        rows.append(
            {
                "skill": str(entry.get("skill", "")),
                "version": str(entry.get("version", "")),
                "timestamp": str(entry.get("timestamp", "")),
                "description": str(entry.get("description", "")),
            }
        )
    widths = {
        key: max(len(key), max(len(r[key]) for r in rows))
        for key in ("skill", "version", "timestamp", "description")
    }
    lines = []
    header = "  ".join(key.ljust(widths[key]) for key in widths)
    lines.append(header)
    lines.append("  ".join("-" * widths[key] for key in widths))
    for row in rows:
        lines.append("  ".join(row[key].ljust(widths[key]) for key in widths))
    return "\n".join(lines)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Show applied skills")
    parser.add_argument(
        "--state",
        default="state/skills_state.json",
        help="Path to skills_state.json",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit JSON instead of a table",
    )
    args = parser.parse_args(argv)

    state_path = Path(args.state).expanduser().resolve()
    entries = _load_state(state_path)

    if args.json:
        print(json.dumps(entries, indent=2))
    else:
        print(_format_table(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
