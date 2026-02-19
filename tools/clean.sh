#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

DRY_RUN=0
AGGRESSIVE=0
PURGE_STATE=0
PURGE_DEPS=0
PURGE_REF_GIT=0
KEEP_BUILD=0
KEEP_OUT=0
THRESHOLD_GB=2
OUT_MAX_DAYS=14
MAX_REPO_GB=0

usage() {
  cat <<'USAGE'
Usage: tools/clean.sh [options]

Default behavior:
  - Remove oversized build artifacts (build/, build-nohttp/, build-core/, build-core-espcheck/) when > threshold.
  - Prune old files from out/ (logs) older than OUT_MAX_DAYS.
  - If out/ remains oversized, remove it entirely.

Options:
  --dry-run           Show what would be removed without deleting.
  --aggressive        Remove build*/ and out/ regardless of size; prune UI build caches.
  --purge-state       Also remove stateful data (state/, db/, memory/, session_*).
  --purge-deps        Also remove dependency caches (ui/node_modules, .agent_deps, ref/*/venv).
  --purge-ref-git     Remove nested .git dirs under ref/ (vendored repos) to reduce bloat.
  --keep-build        Skip build*/ cleanup.
  --keep-out          Skip out/ cleanup.
  --threshold-gb N    Size threshold in GiB (default: 2).
  --out-max-days N    Age threshold for out/ pruning (default: 14; 0 = delete all files).
  --max-repo-gb N     Fail if repo size exceeds N GiB after cleanup (0 = disabled).
  -h, --help          Show this help.

Examples:
  tools/clean.sh
  tools/clean.sh --aggressive
  tools/clean.sh --purge-state --threshold-gb 1
  tools/clean.sh --purge-deps
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift 1
      ;;
    --aggressive)
      AGGRESSIVE=1
      shift 1
      ;;
    --purge-state)
      PURGE_STATE=1
      shift 1
      ;;
    --purge-deps)
      PURGE_DEPS=1
      shift 1
      ;;
    --purge-ref-git)
      PURGE_REF_GIT=1
      shift 1
      ;;
    --keep-build)
      KEEP_BUILD=1
      shift 1
      ;;
    --keep-out)
      KEEP_OUT=1
      shift 1
      ;;
    --threshold-gb)
      shift 1
      THRESHOLD_GB="${1:-}"
      shift 1
      ;;
    --out-max-days)
      shift 1
      OUT_MAX_DAYS="${1:-}"
      shift 1
      ;;
    --max-repo-gb)
      shift 1
      MAX_REPO_GB="${1:-}"
      shift 1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

DRY_RUN="${DRY_RUN}" \
AGGRESSIVE="${AGGRESSIVE}" \
PURGE_STATE="${PURGE_STATE}" \
PURGE_DEPS="${PURGE_DEPS}" \
PURGE_REF_GIT="${PURGE_REF_GIT}" \
KEEP_BUILD="${KEEP_BUILD}" \
KEEP_OUT="${KEEP_OUT}" \
THRESHOLD_GB="${THRESHOLD_GB}" \
OUT_MAX_DAYS="${OUT_MAX_DAYS}" \
MAX_REPO_GB="${MAX_REPO_GB}" \
python3 - <<'PY'
import os
import shutil
import time
from pathlib import Path

root = Path.cwd()
drive = root

dry_run = os.environ.get("DRY_RUN", "0") == "1"
aggressive = os.environ.get("AGGRESSIVE", "0") == "1"
purge_state = os.environ.get("PURGE_STATE", "0") == "1"
purge_deps = os.environ.get("PURGE_DEPS", "0") == "1"
purge_ref_git = os.environ.get("PURGE_REF_GIT", "0") == "1"
keep_build = os.environ.get("KEEP_BUILD", "0") == "1"
keep_out = os.environ.get("KEEP_OUT", "0") == "1"

try:
    threshold_gb = float(os.environ.get("THRESHOLD_GB", "2"))
except ValueError:
    threshold_gb = 2.0
try:
    out_max_days = int(os.environ.get("OUT_MAX_DAYS", "14"))
except ValueError:
    out_max_days = 14
try:
    max_repo_gb = float(os.environ.get("MAX_REPO_GB", "0"))
except ValueError:
    max_repo_gb = 0.0

threshold_bytes = int(threshold_gb * (1024 ** 3))
max_repo_bytes = int(max_repo_gb * (1024 ** 3))


def dir_size_bytes(path: Path) -> int:
    total = 0
    for root_dir, _, files in os.walk(path):
        for name in files:
            try:
                total += (Path(root_dir) / name).stat().st_size
            except OSError:
                pass
    return total


def remove_path(path: Path, reason: str) -> None:
    if not path.exists():
        return
    print(f"[clean] remove {path} ({reason})")
    if dry_run:
        return
    if path.is_file() or path.is_symlink():
        path.unlink(missing_ok=True)
        return
    shutil.rmtree(path)


def prune_out(out_dir: Path) -> None:
    if not out_dir.exists() or keep_out:
        return

    if aggressive:
        remove_path(out_dir, "aggressive")
        return

    if out_max_days <= 0:
        for item in out_dir.iterdir():
            remove_path(item, "out prune (all)")
    else:
        cutoff = time.time() - out_max_days * 86400
        for item in out_dir.rglob("*"):
            if item.is_file():
                try:
                    if item.stat().st_mtime < cutoff:
                        remove_path(item, "out prune (age)")
                except OSError:
                    pass

    if out_dir.exists():
        size = dir_size_bytes(out_dir)
        if size > threshold_bytes:
            remove_path(out_dir, f"out oversized ({size} bytes)")


# Build artifacts
if not keep_build:
    for name in ("build", "build-nohttp", "build-core", "build-core-espcheck"):
        p = root / name
        if not p.exists():
            continue
        if aggressive:
            remove_path(p, "aggressive")
            continue
        size = dir_size_bytes(p)
        if size > threshold_bytes:
            remove_path(p, f"oversized ({size} bytes)")

# Logs
prune_out(root / "out")

# UI build caches (aggressive only)
if aggressive:
    for name in ("ui/dist", "ui/.vite", "ui/.npm-cache", "ui/test-results"):
        remove_path(root / name, "ui cache")

# Stateful data (explicit only)
if purge_state:
    for name in ("state", "db", "memory"):
        remove_path(root / name, "purge state")
    for item in root.glob("session_*"):
        remove_path(item, "purge state")

# Dependency caches (explicit only)
if purge_deps:
    remove_path(root / ".agent_deps", "purge deps")
    remove_path(root / "ui" / "node_modules", "purge deps")
    for venv in (root / "ref").rglob("venv"):
        remove_path(venv, "purge deps")

# Vendored repos (explicit only)
if purge_ref_git:
    for git_dir in (root / "ref").rglob(".git"):
        remove_path(git_dir, "purge ref git")

if max_repo_gb > 0:
    repo_size = dir_size_bytes(root)
    if repo_size > max_repo_bytes:
        print(f"[clean] repo size {repo_size} bytes exceeds {max_repo_gb} GiB limit")
        raise SystemExit(2)
print("[clean] done")
PY
