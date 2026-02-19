#!/usr/bin/env bash
set -euo pipefail

pg_test_has_local_pg() {
  command -v initdb >/dev/null 2>&1 && command -v pg_ctl >/dev/null 2>&1
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
    rm -rf "${data_dir}" || true
    return 1
  fi

  local port
  port="$(pg_test_pick_port)"
  if ! pg_ctl -D "${data_dir}" -o "-F -h 127.0.0.1 -p ${port}" -w start >/dev/null; then
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
