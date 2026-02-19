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
  echo "[mac] Hint: start Docker Desktop or Colima, then re-run." >&2
  exit 77
fi

set +e
"${ROOT}/tools/verify_compose_stack.sh"
rc=$?
set -e

if [[ "${rc}" -ne 0 ]]; then
  if [[ "${rc}" -eq 77 ]]; then
    latest_build="$(ls -t "${ROOT}/out"/compose_build_*.log 2>/dev/null | head -n1 || true)"
    latest_up="$(ls -t "${ROOT}/out"/compose_up_*.log 2>/dev/null | head -n1 || true)"
    if [[ -n "${latest_build}" || -n "${latest_up}" ]]; then
      if grep -Eqi "resource temporarily unavailable|unpigz|runc run failed" "${latest_build}" "${latest_up}" 2>/dev/null; then
        echo "[mac] SKIP: docker compose failed due to resource limits (unpigz/runc)." >&2
        echo "[mac] Hint: Docker Desktop -> Settings -> Resources: increase CPU/RAM and disk image size." >&2
        echo "[mac] Hint: Use prebuilt images to skip local builds:" >&2
        echo "[mac]   BROKER_IMAGE=... AGENTD_IMAGE=... CONNECTOR_IMAGE=... WEBUI_IMAGE=..." >&2
        echo "[mac]   COMPOSE_BUILD=0 COMPOSE_PULL=1 tools/verify_compose_stack.sh" >&2
        echo "[mac] Hint: You can also set PIGZ=-p1 GZIP=-p1 to reduce decompression pressure." >&2
      fi
    fi
  fi
  exit "${rc}"
fi
