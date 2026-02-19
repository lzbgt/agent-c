#!/usr/bin/env python3
import argparse
import fnmatch
import subprocess
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
    return False


def list_git_files(root: Path) -> list[Path]:
    out = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
    return [root / p.decode("utf-8", errors="ignore") for p in out.split(b"\0") if p]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail if tracked files exceed a size threshold."
    )
    parser.add_argument(
        "--root",
        default=None,
        help="Repo root (defaults to tools/..).",
    )
    parser.add_argument(
        "--max-bytes",
        type=int,
        default=0,
        help="Max allowed file size in bytes (overrides --max-mb).",
    )
    parser.add_argument(
        "--max-mb",
        type=float,
        default=10.0,
        help="Max allowed file size in MiB (default: 10).",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="Print top N largest tracked files (default: 20).",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Exclude glob patterns (repeatable, matched against repo-relative paths).",
    )
    parser.add_argument(
        "--warn-only",
        action="store_true",
        help="Do not fail on oversized files; print warnings only.",
    )
    args = parser.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    max_bytes = int(args.max_bytes) if args.max_bytes > 0 else int(args.max_mb * 1024 * 1024)

    try:
        files = list_git_files(root)
    except (subprocess.SubprocessError, FileNotFoundError) as exc:
        print(f"ERROR: unable to list git files: {exc}")
        return 2

    entries = []
    oversized = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        if match_exclude(rel, args.exclude):
            continue
        if not path.is_file() or path.is_symlink():
            continue
        try:
            size = path.stat().st_size
        except OSError:
            continue
        entries.append((size, rel))
        if size > max_bytes:
            oversized.append((size, rel))

    entries.sort(reverse=True)
    print(f"Tracked file size guard: {root}")
    print(f"Limit: {format_bytes(max_bytes)}")
    if args.exclude:
        print(f"Exclude: {', '.join(args.exclude)}")

    print(f"\nTop {min(args.top, len(entries))} tracked files:")
    for size, rel in entries[: args.top]:
        print(f"  {format_bytes(size):>10}  {rel}")

    if oversized:
        print("\nOversized files:")
        for size, rel in oversized:
            print(f"  {format_bytes(size):>10}  {rel}")
        if not args.warn_only:
            return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
