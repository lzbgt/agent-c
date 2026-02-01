#!/usr/bin/env bash

# Shared helpers for bash-based agentd smoke tests.
# Intentionally minimal and dependency-light (bash + python3 + curl).

agentd_smoke_project_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  (cd "${script_dir}/.." && pwd)
}

agentd_smoke_log_dir() {
  local root
  root="$(agentd_smoke_project_root)"
  echo "${root}/build"
}

agentd_smoke_pick_port() {
  # Bind to port 0 to get a free port, then close immediately.
  python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

agentd_smoke_start() {
  # Usage:
  #   agentd_smoke_start <agentd_bin> <bind_host> <port> <name> [extra args...]
  #
  # Sets globals:
  #   AGENTD_PID, DAEMON_URL, LOG_DIR
  local bin="${1:-}"
  local bind_host="${2:-127.0.0.1}"
  local port="${3:-}"
  local name="${4:-agentd_smoke}"
  shift 4 || true

  if [[ -z "${bin}" || -z "${port}" ]]; then
    echo "agentd_smoke_start: missing bin/port" >&2
    return 2
  fi

  LOG_DIR="$(agentd_smoke_log_dir)"
  mkdir -p "${LOG_DIR}"

  DAEMON_URL="http://${bind_host}:${port}"

  # Hermetic DB for smoke tests:
  # - agentd now uses SQLite as a primary state store, so always give tests their own DB file
  #   unless they explicitly pass --db-path.
  local has_db="0"
  for a in "$@"; do
    if [[ "${a}" == "--db-path" ]]; then
      has_db="1"
      break
    fi
  done
  if [[ "${has_db}" == "0" ]]; then
    set -- --db-path "${LOG_DIR}/${name}_${port}.sqlite" "$@"
  fi

  # Hermetic state dir for smoke tests:
  # - Avoid polluting ~/.agent (default) and avoid coupling to any existing user memory/session files.
  # - This also keeps memory tools deterministic when tests enable them.
  local has_state="0"
  for a in "$@"; do
    if [[ "${a}" == "--state-dir" ]]; then
      has_state="1"
      break
    fi
  done
  if [[ "${has_state}" == "0" ]]; then
    set -- --state-dir "${LOG_DIR}/${name}_${port}.state" "$@"
  fi

  "${bin}" \
    --host "${bind_host}" \
    --port "${port}" \
    "$@" \
    > "${LOG_DIR}/${name}.stdout.log" 2> "${LOG_DIR}/${name}.stderr.log" &
  AGENTD_PID=$!
}

agentd_smoke_start_bind() {
  # Usage:
  #   agentd_smoke_start_bind <agentd_bin> <bind_host> <connect_host> <port> <name> [extra args...]
  #
  # Useful when binding to 0.0.0.0 but connecting via 127.0.0.1.
  local bin="${1:-}"
  local bind_host="${2:-}"
  local connect_host="${3:-}"
  local port="${4:-}"
  local name="${5:-agentd_smoke}"
  shift 5 || true

  if [[ -z "${bin}" || -z "${bind_host}" || -z "${connect_host}" || -z "${port}" ]]; then
    echo "agentd_smoke_start_bind: missing args" >&2
    return 2
  fi

  LOG_DIR="$(agentd_smoke_log_dir)"
  mkdir -p "${LOG_DIR}"

  DAEMON_URL="http://${connect_host}:${port}"

  # Hermetic DB for smoke tests unless explicitly passed.
  local has_db="0"
  for a in "$@"; do
    if [[ "${a}" == "--db-path" ]]; then
      has_db="1"
      break
    fi
  done
  if [[ "${has_db}" == "0" ]]; then
    set -- --db-path "${LOG_DIR}/${name}_${port}.sqlite" "$@"
  fi

  # Hermetic state dir for smoke tests unless explicitly passed.
  local has_state="0"
  for a in "$@"; do
    if [[ "${a}" == "--state-dir" ]]; then
      has_state="1"
      break
    fi
  done
  if [[ "${has_state}" == "0" ]]; then
    set -- --state-dir "${LOG_DIR}/${name}_${port}.state" "$@"
  fi

  "${bin}" \
    --host "${bind_host}" \
    --port "${port}" \
    "$@" \
    > "${LOG_DIR}/${name}.stdout.log" 2> "${LOG_DIR}/${name}.stderr.log" &
  AGENTD_PID=$!
}

agentd_smoke_stop() {
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill -TERM "${AGENTD_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
      kill -KILL "${AGENTD_PID}" >/dev/null 2>&1 || true
    fi
    wait "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}

agentd_smoke_wait_health() {
  local url="${1:-}"
  if [[ -z "${url}" ]]; then
    echo "agentd_smoke_wait_health: missing url" >&2
    return 2
  fi
  for _ in $(seq 1 60); do
    if curl -fsS --noproxy "*" --max-time 2 "${url}/api/v1/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "agentd did not become healthy: ${url}" >&2
  return 1
}
