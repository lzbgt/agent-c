#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

STRICT="${REPO_GUARD_STRICT:-0}"
MAX_TOTAL_GB="${REPO_GUARD_MAX_GB:-5}"
MAX_FILE_MB="${REPO_GUARD_MAX_FILE_MB:-10}"
MAX_UNTRACKED_MB="${REPO_GUARD_MAX_UNTRACKED_MB:-100}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict)
      STRICT=1
      shift 1
      ;;
    --max-total-gb)
      shift 1
      MAX_TOTAL_GB="${1:-}"
      shift 1
      ;;
    --max-file-mb)
      shift 1
      MAX_FILE_MB="${1:-}"
      shift 1
      ;;
    --max-untracked-mb)
      shift 1
      MAX_UNTRACKED_MB="${1:-}"
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/verify_repo_guards.sh [--strict] [--max-total-gb N] [--max-file-mb N] [--max-untracked-mb N]

Runs repo hygiene guards:
  - repo size guard (max 5 GiB, excludes .git)
  - stub file scan
  - tracked file size guard (10 MiB)

Options:
  --strict   Also fail on nested .git dirs (matches CI guard).
  --max-total-gb N  Override repo size limit in GiB (default: 5).
  --max-file-mb N   Override tracked file size limit in MiB (default: 10).
  --max-untracked-mb N Override untracked file size limit in MiB (default: 100).

Env overrides:
  REPO_GUARD_MAX_GB
  REPO_GUARD_MAX_FILE_MB
  REPO_GUARD_MAX_UNTRACKED_MB
  REPO_GUARD_STRICT
  ALLOW_VENDORED_CHANGES (skip vendored guard)
  VENDORED_GUARD_BASE (base ref for vendored guard)
  VENDORED_GUARD_REQUIRE_BASE (fail if no base ref)
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

args=(--exclude .git --max-total-gb "${MAX_TOTAL_GB}" --depth 2 --top 20)
if [[ "${STRICT}" == "1" ]]; then
  args+=(--fail-on-nested-git)
fi

python3 tools/repo_size_report.py "${args[@]}"
python3 tools/stub_file_scan.py --fail
python3 tools/tracked_file_guard.py --max-mb "${MAX_FILE_MB}"
python3 tools/untracked_file_guard.py --exclude-defaults --max-mb "${MAX_UNTRACKED_MB}"
python3 tools/vendored_guard.py --path ref
