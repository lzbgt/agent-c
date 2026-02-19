#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional


def run_git(args: list[str], root: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *args], stderr=subprocess.STDOUT
    ).decode("utf-8", errors="ignore")


def diff_paths(root: Path, args: list[str]) -> list[str]:
    try:
        output = run_git(args, root)
    except subprocess.CalledProcessError as exc:
        message = exc.output.decode("utf-8", errors="ignore") if exc.output else ""
        raise RuntimeError(message.strip()) from exc
    return [line.strip() for line in output.splitlines() if line.strip()]


def ref_exists(root: Path, ref: str) -> bool:
    try:
        subprocess.check_call(
            ["git", "-C", str(root), "show-ref", "--verify", "--quiet", ref]
        )
        return True
    except subprocess.CalledProcessError:
        return False


def normalize_base_ref(root: Path, ref: str) -> Optional[str]:
    if ref_exists(root, ref):
        return ref
    if not ref.startswith("refs/") and not ref.startswith("origin/"):
        origin_ref = f"refs/remotes/origin/{ref}"
        if ref_exists(root, origin_ref):
            return origin_ref
    return None


def pick_base_ref(root: Path, candidates: Iterable[str]) -> Optional[str]:
    for ref in candidates:
        normalized = normalize_base_ref(root, ref)
        if normalized:
            return normalized
    return None


def bool_env(name: str) -> bool:
    value = os.environ.get(name, "")
    return value.lower() in {"1", "true", "yes", "on"}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail if vendored subtree changes are detected."
    )
    parser.add_argument(
        "--root", default=None, help="Repo root (defaults to tools/..)."
    )
    parser.add_argument("--path", default="ref", help="Vendored subtree path.")
    parser.add_argument("--base", default=None, help="Base ref to diff from.")
    parser.add_argument("--head", default="HEAD", help="Head ref to diff to.")
    parser.add_argument(
        "--require-base",
        action="store_true",
        help="Fail if no suitable base ref can be found.",
    )
    parser.add_argument(
        "--allow-env",
        default="ALLOW_VENDORED_CHANGES",
        help="Env var that allows vendored changes when set truthy.",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress non-error output (still exits non-zero on changes).",
    )
    parser.add_argument(
        "--verbose", action="store_true", help="Print extra diagnostics."
    )
    args = parser.parse_args()

    quiet = args.quiet or bool_env("VENDORED_GUARD_QUIET")
    if args.verbose:
        quiet = False

    def log(message: str) -> None:
        if not quiet:
            print(message)

    def error(message: str) -> None:
        print(message, file=sys.stderr)

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    vendored_path = root / args.path
    if not vendored_path.exists():
        return 0

    if bool_env(args.allow_env):
        log(
            f"[vendored_guard] {args.allow_env} set; skipping vendored change check."
        )
        return 0

    candidates: list[str] = []
    if args.base:
        candidates.append(args.base)
    env_base = os.environ.get("VENDORED_GUARD_BASE")
    if env_base:
        candidates.append(env_base)
    github_base = os.environ.get("GITHUB_BASE_REF")
    if github_base:
        candidates.append(f"origin/{github_base}")
        candidates.append(github_base)
    git_base = os.environ.get("GIT_BASE_REF")
    if git_base:
        candidates.append(git_base)
    candidates.extend(["origin/master", "origin/main"])

    base_ref = pick_base_ref(root, candidates)
    require_base = args.require_base or bool_env("VENDORED_GUARD_REQUIRE_BASE")
    if not base_ref and require_base:
        error(
            "[vendored_guard] No base ref found; failing because require-base is set."
        )
        return 2

    if args.verbose and not base_ref:
        log(
            "[vendored_guard] No base ref found; checking working tree changes only."
        )
    if args.verbose and base_ref:
        log(f"[vendored_guard] Using base ref: {base_ref}")

    changed: set[str] = set()
    try:
        if base_ref:
            changed.update(
                diff_paths(
                    root,
                    [
                        "diff",
                        "--name-only",
                        f"{base_ref}...{args.head}",
                        "--",
                        args.path,
                    ],
                )
            )
        changed.update(
            diff_paths(root, ["diff", "--name-only", "--cached", "--", args.path])
            )
        changed.update(diff_paths(root, ["diff", "--name-only", "--", args.path]))
    except RuntimeError as exc:
        error("[vendored_guard] git diff failed.")
        if str(exc):
            error(str(exc))
        return 2

    if not changed:
        if args.verbose:
            log("[vendored_guard] No vendored changes detected.")
        return 0

    if not quiet:
        print("[vendored_guard] Vendored subtree changes detected:")
    for path in sorted(changed):
        print(f"  {path}")
    if not quiet:
        print(
            f"Set {args.allow_env}=1 to bypass if you intentionally updated vendored code."
        )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
