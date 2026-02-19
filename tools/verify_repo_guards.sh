#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

STRICT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict)
      STRICT=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/verify_repo_guards.sh [--strict]

Runs repo hygiene guards:
  - repo size guard (max 5 GiB, excludes .git)
  - stub file scan
  - tracked file size guard (10 MiB)

Options:
  --strict   Also fail on nested .git dirs (matches CI guard).
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

args=(--exclude .git --max-total-gb 5 --depth 2 --top 20)
if [[ "${STRICT}" == "1" ]]; then
  args+=(--fail-on-nested-git)
fi

python3 tools/repo_size_report.py "${args[@]}"
python3 tools/stub_file_scan.py --fail
python3 tools/tracked_file_guard.py --max-mb 10
