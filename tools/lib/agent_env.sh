#!/usr/bin/env bash
set -euo pipefail

agent_env_home_dir() {
  if [[ -n "${HOME:-}" ]]; then
    echo "${HOME}"
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PY'
import os
import pwd
try:
    print(pwd.getpwuid(os.getuid()).pw_dir)
except Exception:
    pass
PY
    return 0
  fi
  return 1
}

agent_env_source_home() {
  local home_dir=""
  home_dir="$(agent_env_home_dir || true)"
  if [[ -z "${home_dir}" ]]; then
    return 1
  fi
  local env_file="${home_dir}/.env"
  if [[ ! -f "${env_file}" ]]; then
    return 1
  fi
  set -a
  # shellcheck disable=SC1090
  source "${env_file}"
  set +a
  return 0
}
