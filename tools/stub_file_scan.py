#!/usr/bin/env python3
import argparse
import subprocess
from pathlib import Path
from typing import Iterable

PLACEHOLDER_DEFAULTS = {
    "<claude-mem-context>\n\n</claude-mem-context>",
    "<claude-mem-context>\r\n\r\n</claude-mem-context>",
    "<claude-mem-context></claude-mem-context>",
}


def list_git_files(root: Path) -> list[Path]:
    try:
        out = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
    except (subprocess.SubprocessError, FileNotFoundError):
        return []
    files = []
    for entry in out.split(b"\0"):
        if not entry:
            continue
        files.append(root / entry.decode("utf-8", errors="ignore"))
    return files


def scan_files(files: Iterable[Path], placeholders: set[str]) -> list[Path]:
    stubs = []
    for path in files:
        if not path.is_file() or path.is_symlink():
            continue
        try:
            data = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        stripped = data.strip()
        if stripped == "" or stripped in placeholders:
            stubs.append(path)
    return stubs


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Scan for stub files (empty or placeholder-only content)."
    )
    parser.add_argument(
        "--root",
        default=None,
        help="Repo root (defaults to tools/..).",
    )
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Extra placeholder strings to treat as stub (repeatable).",
    )
    parser.add_argument(
        "--fail",
        action="store_true",
        help="Exit with code 2 if stub files are found.",
    )
    args = parser.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    files = list_git_files(root)
    if not files:
        # Fallback to scanning all files if git is unavailable.
        files = [p for p in root.rglob("*") if p.is_file()]

    placeholders = set(PLACEHOLDER_DEFAULTS)
    placeholders.update(args.include)

    stubs = scan_files(files, placeholders)
    if stubs:
        print("Stub files detected:")
        for path in stubs:
            print(f"  {path.relative_to(root)}")
        if args.fail:
            return 2
    else:
        print("No stub files detected.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
