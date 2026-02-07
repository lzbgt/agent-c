#!/usr/bin/env bash
set -euo pipefail

# macOS full-stack verification (agentd + broker + WebUI).
# This script expects Docker Desktop or Colima to be running.
#
# It delegates to tools/verify_compose_stack.sh, which brings up:
# - postgres + keycloak + broker + connector + agentd + webui
# and runs basic end-to-end checks.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[mac] WARNING: not running on macOS; continuing anyway"
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "[mac] SKIP: docker not found (install Docker Desktop or Colima)"
  exit 77
fi

if ! docker info >/dev/null 2>&1; then
  echo "[mac] SKIP: docker daemon not running"
  exit 77
fi

exec "${ROOT}/tools/verify_compose_stack.sh"
