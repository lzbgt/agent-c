#!/usr/bin/env python3
"""Create a sandbox mount allowlist template under ~/.config/agent/"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def _abort(msg: str) -> None:
    raise SystemExit(msg)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Create mount allowlist template")
    parser.add_argument(
        "--path",
        default=None,
        help="Target allowlist path (default: ~/.config/agent/mount-allowlist.json)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing allowlist",
    )
    args = parser.parse_args(argv)

    default_path = Path("~/.config/agent/mount-allowlist.json").expanduser()
    target = Path(args.path).expanduser().resolve() if args.path else default_path

    if target.exists() and not args.force:
        _abort(f"allowlist already exists: {target} (use --force to overwrite)")

    target.parent.mkdir(parents=True, exist_ok=True)

    payload = {
        "allowed_roots": [],
        "blocked_patterns": [
            ".ssh",
            ".gnupg",
            ".aws",
            ".kube",
            ".docker",
            ".env",
            "id_rsa",
            "id_ed25519",
            "private_key",
        ],
        "non_main_readonly": True,
    }

    target.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"wrote: {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
