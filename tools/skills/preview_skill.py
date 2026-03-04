#!/usr/bin/env python3
"""Preview a skill by applying it in a temporary git worktree."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional, Sequence


def _abort(msg: str) -> None:
    raise SystemExit(msg)


def _git_root(cwd: Path) -> Path:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=cwd, text=True
        ).strip()
        if out:
            return Path(out)
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return cwd


def _git_output(args: Sequence[str], cwd: Path) -> str:
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()


def _git_run(args: Sequence[str], cwd: Path) -> None:
    result = subprocess.run(["git", *args], cwd=cwd)
    if result.returncode != 0:
        _abort(f"git command failed: {' '.join(args)}")


def _has_tracked_changes(root: Path) -> bool:
    unstaged = _git_output(["diff", "--name-only"], root)
    staged = _git_output(["diff", "--cached", "--name-only"], root)
    return bool(unstaged or staged)


def _warn(msg: str) -> None:
    print(f"[preview] {msg}")


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description="Preview a skill in a temp worktree")
    parser.add_argument("skill_dir", help="Path to skill directory")
    parser.add_argument(
        "--root",
        help="Repo root (default: git root of cwd)",
        default=None,
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="Allow tracked changes in the repo (default: warn only)",
    )
    parser.add_argument(
        "--keep-worktree",
        action="store_true",
        help="Keep the temp worktree for inspection",
    )
    parser.add_argument(
        "--worktree",
        help="Explicit worktree path (default: out/skills/preview/<ts>/worktree)",
        default=None,
    )
    args = parser.parse_args(argv)

    skill_dir = Path(args.skill_dir).expanduser().resolve()
    repo_root = Path(args.root).expanduser().resolve() if args.root else _git_root(Path.cwd())

    if _has_tracked_changes(repo_root):
        msg = "tracked changes detected in repo"
        if args.allow_dirty:
            _warn(msg)
        else:
            _warn(msg + " (proceeding anyway; use --allow-dirty to silence)")

    timestamp = dt.datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    out_dir = repo_root / "out" / "skills" / "preview" / timestamp
    out_dir.mkdir(parents=True, exist_ok=True)

    worktree = (
        Path(args.worktree).expanduser().resolve()
        if args.worktree
        else out_dir / "worktree"
    )

    if worktree.exists():
        _abort(f"worktree path already exists: {worktree}")

    try:
        _git_run(["worktree", "add", "--detach", str(worktree), "HEAD"], repo_root)
        apply_log = out_dir / "apply.log"
        apply_cmd = [
            sys.executable,
            str(repo_root / "tools" / "skills" / "apply_skill.py"),
            str(skill_dir),
            "--root",
            str(worktree),
        ]
        with apply_log.open("w", encoding="utf-8") as log:
            result = subprocess.run(apply_cmd, cwd=repo_root, stdout=log, stderr=log)
        if result.returncode != 0:
            _abort(f"apply failed (see {apply_log})")

        diff_patch = _git_output(["-C", str(worktree), "diff"], repo_root)
        diff_stat = _git_output(["-C", str(worktree), "diff", "--stat"], repo_root)

        (out_dir / "diff.patch").write_text(diff_patch + "\n", encoding="utf-8")
        (out_dir / "diff.stat").write_text(diff_stat + "\n", encoding="utf-8")

        print(f"preview ready: {out_dir}")
        return 0
    finally:
        if worktree.exists() and not args.keep_worktree:
            try:
                _git_run(["worktree", "remove", "--force", str(worktree)], repo_root)
            except SystemExit:
                _warn(f"failed to remove worktree: {worktree}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
