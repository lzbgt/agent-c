#!/usr/bin/env bash

pick_python_cmd() {
  if command -v python3 >/dev/null 2>&1; then
    echo "python3"
    return 0
  fi
  if command -v python >/dev/null 2>&1; then
    echo "python"
    return 0
  fi
  return 1
}

python_http_server_cmd() {
  local port="${1:?port required}"
  local python_bin
  if ! python_bin="$(pick_python_cmd)"; then
    return 1
  fi
  echo "${python_bin} -m http.server ${port}"
  return 0
}
