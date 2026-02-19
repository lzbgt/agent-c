#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/clean_docker_agent_stacks.sh [--apply] [--include-stopped]

Lists compose projects that look like agent_* stacks (from verify_compose_stack.sh).
By default it is a dry run; use --apply to stop and remove the stacks.

Options:
  --apply            Actually run `docker compose -p <project> down -v --remove-orphans`.
  --include-stopped  Include stopped containers when discovering projects.
EOF
}

apply=0
include_stopped=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --apply) apply=1 ;;
    --include-stopped) include_stopped=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
  esac
  shift
done

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found" >&2
  exit 1
fi

ps_args=()
if [[ "${include_stopped}" == "1" ]]; then
  ps_args+=(-a)
fi

projects="$(
  docker ps "${ps_args[@]}" --format '{{.Label "com.docker.compose.project"}}' \
    | rg -N '^agent_' \
    | sort -u
)"

if [[ -z "${projects}" ]]; then
  echo "no agent_* compose projects found"
  exit 0
fi

echo "agent_* compose projects:"
echo "${projects}" | sed 's/^/  - /'

if [[ "${apply}" != "1" ]]; then
  echo "dry run; re-run with --apply to stop/remove these stacks"
  exit 0
fi

while IFS= read -r project; do
  if [[ -z "${project}" ]]; then
    continue
  fi
  echo "down: ${project}"
  docker compose -p "${project}" down -v --remove-orphans
done <<<"${projects}"
