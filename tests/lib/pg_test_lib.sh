#!/usr/bin/env bash
set -euo pipefail

PG_TEST_LAST_ERROR=""

pg_test_has_local_pg() {
  PG_TEST_LAST_ERROR=""
  if ! command -v initdb >/dev/null 2>&1; then
    PG_TEST_LAST_ERROR="initdb not found"
    return 1
  fi
  if ! command -v pg_ctl >/dev/null 2>&1; then
    PG_TEST_LAST_ERROR="pg_ctl not found"
    return 1
  fi
  if command -v pg_config >/dev/null 2>&1; then
    local sharedir
    sharedir="$(pg_config --sharedir 2>/dev/null || true)"
    if [[ -n "${sharedir}" && ! -f "${sharedir}/postgres.bki" ]]; then
      PG_TEST_LAST_ERROR="postgres.bki missing in ${sharedir}"
      return 1
    fi
  fi
  return 0
}

pg_test_unavailable_reason() {
  if [[ -n "${PG_TEST_LAST_ERROR:-}" ]]; then
    printf "%s" "${PG_TEST_LAST_ERROR}"
  else
    printf "local Postgres unavailable"
  fi
}

pg_test_pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

pg_test_start_local() {
  if ! pg_test_has_local_pg; then
    return 1
  fi

  local data_dir
  data_dir="$(mktemp -d "${TMPDIR:-/tmp}/agent_pg_XXXXXX")"

  if ! initdb -D "${data_dir}" -U postgres --auth=trust >/dev/null; then
    PG_TEST_LAST_ERROR="initdb failed"
    rm -rf "${data_dir}" || true
    return 1
  fi

  local port
  port="$(pg_test_pick_port)"
  if ! pg_ctl -D "${data_dir}" -o "-F -h 127.0.0.1 -p ${port}" -w start >/dev/null; then
    PG_TEST_LAST_ERROR="pg_ctl start failed"
    rm -rf "${data_dir}" || true
    return 1
  fi

  PG_TEST_DATA_DIR="${data_dir}"
  PG_TEST_PORT="${port}"
  PG_TEST_DSN="postgres://postgres@127.0.0.1:${port}/postgres?sslmode=disable"
}

pg_test_stop_local() {
  if [[ -z "${PG_TEST_DATA_DIR:-}" ]]; then
    return 0
  fi
  pg_ctl -D "${PG_TEST_DATA_DIR}" -m fast -w stop >/dev/null 2>&1 || true
  rm -rf "${PG_TEST_DATA_DIR}" || true
  unset PG_TEST_DATA_DIR PG_TEST_PORT PG_TEST_DSN
}
