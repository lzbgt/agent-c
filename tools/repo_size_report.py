#!/usr/bin/env python3
import argparse
import fnmatch
import heapq
import os
from pathlib import Path
from typing import Iterable


def format_bytes(num: int) -> str:
    if num >= 1024 ** 3:
        return f"{num / (1024 ** 3):.2f} GiB"
    if num >= 1024 ** 2:
        return f"{num / (1024 ** 2):.2f} MiB"
    if num >= 1024:
        return f"{num / 1024:.2f} KiB"
    return f"{num} B"


def match_exclude(rel_path: str, patterns: Iterable[str]) -> bool:
    for pat in patterns:
        if fnmatch.fnmatch(rel_path, pat):
            return True
        if rel_path.startswith(pat.rstrip("/") + os.sep):
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Report repo disk usage (top-level and depth-limited sizes)."
    )
    parser.add_argument(
        "--root",
        default=None,
        help="Repo root (defaults to tools/..).",
    )
    parser.add_argument(
        "--depth",
        type=int,
        default=2,
        help="Max directory depth to aggregate (default: 2).",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="Show top N aggregated paths (default: 20).",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Exclude glob patterns (repeatable, matched against repo-relative paths).",
    )
    parser.add_argument(
        "--exclude-defaults",
        action="store_true",
        help="Exclude common bulky paths (git objects, builds, node_modules, venvs).",
    )
    parser.add_argument(
        "--largest-files",
        type=int,
        default=0,
        help="Show top N largest files (default: 0 = disabled).",
    )
    parser.add_argument(
        "--largest-min-bytes",
        type=int,
        default=0,
        help="Only include files >= this size in largest-files output.",
    )
    args = parser.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    depth = max(1, args.depth)
    if args.exclude_defaults:
        args.exclude.extend(
            [
                ".git",
                "ref/**/.git",
                "build",
                "build-*",
                "ui/node_modules",
                ".agent_deps",
                "out",
                "ref/**/venv",
                "ref/**/.venv",
            ]
        )

    total = 0
    sizes = {}
    top_level_sizes = {}
    largest = []
    largest_limit = max(0, args.largest_files)
    largest_min_bytes = max(0, args.largest_min_bytes)

    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir == ".":
            rel_dir = ""
        if rel_dir and match_exclude(rel_dir, args.exclude):
            dirnames[:] = []
            continue

        filtered = []
        for d in dirnames:
            rel = os.path.join(rel_dir, d) if rel_dir else d
            if match_exclude(rel, args.exclude):
                continue
            filtered.append(d)
        dirnames[:] = filtered

        for name in filenames:
            path = Path(dirpath) / name
            try:
                size = path.stat().st_size
            except OSError:
                continue
            total += size
            rel_file = os.path.relpath(path, root)
            parts = rel_file.split(os.sep)
            if parts:
                top_key = parts[0]
                top_level_sizes[top_key] = top_level_sizes.get(top_key, 0) + size
            for d in range(1, min(depth, len(parts)) + 1):
                key = os.path.join(*parts[:d])
                sizes[key] = sizes.get(key, 0) + size
            if largest_limit > 0 and size >= largest_min_bytes:
                entry = (size, rel_file)
                if len(largest) < largest_limit:
                    heapq.heappush(largest, entry)
                else:
                    if entry > largest[0]:
                        heapq.heapreplace(largest, entry)

    print(f"Repo size report: {root}")
    print(f"Total: {format_bytes(total)}")
    if args.exclude:
        print(f"Exclude: {', '.join(args.exclude)}")

    print("\nTop-level:")
    for key, size in sorted(top_level_sizes.items(), key=lambda x: x[1], reverse=True):
        print(f"  {format_bytes(size):>10}  {key}")

    print(f"\nTop {args.top} paths (depth <= {depth}):")
    for key, size in sorted(sizes.items(), key=lambda x: x[1], reverse=True)[: args.top]:
        print(f"  {format_bytes(size):>10}  {key}")

    if largest_limit > 0:
        print(f"\nTop {largest_limit} files:")
        for size, rel_file in sorted(largest, key=lambda x: x[0], reverse=True):
            print(f"  {format_bytes(size):>10}  {rel_file}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
