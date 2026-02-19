#!/usr/bin/env bash

# Shared docker readiness helpers.
# Usage:
#   source tools/lib/docker_preflight.sh
#   if ! docker_preflight "compose"; then exit 77; fi

docker_info_ready() {
  if ! command -v python3 >/dev/null 2>&1; then
    docker info >/dev/null 2>&1 || return 1
    return 0
  fi
  python3 - <<'PY'
import os
import subprocess
import sys

timeout = float(os.environ.get("AGENT_DOCKER_INFO_TIMEOUT_SEC", "5"))
try:
  subprocess.run(
      ["docker", "info"],
      stdout=subprocess.DEVNULL,
      stderr=subprocess.DEVNULL,
      timeout=timeout,
      check=True,
  )
except subprocess.TimeoutExpired:
  sys.exit(2)
except Exception:
  sys.exit(1)
sys.exit(0)
PY
}

docker_preflight() {
  local label="${1:-docker}"
  if ! command -v docker >/dev/null 2>&1; then
    echo "[${label}] SKIP: docker not found (install Docker Desktop or Colima)" >&2
    return 77
  fi
  if ! docker_info_ready; then
    local rc=$?
    if [[ "${rc}" == "2" ]]; then
      echo "[${label}] SKIP: docker daemon not responding (docker info timed out)" >&2
    else
      echo "[${label}] SKIP: docker daemon not running" >&2
    fi
    echo "[${label}] Hint: start Docker Desktop or Colima, then re-run." >&2
    return 77
  fi
  return 0
}

docker_compose_preflight() {
  local label="${1:-docker}"
  if ! docker_preflight "${label}"; then
    return $?
  fi
  if ! docker compose version >/dev/null 2>&1; then
    echo "[${label}] SKIP: docker compose not available" >&2
    echo "[${label}] Hint: install the docker compose plugin or upgrade Docker Desktop." >&2
    return 77
  fi
  return 0
}
