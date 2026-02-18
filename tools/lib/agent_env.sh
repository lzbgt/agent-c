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

agent_env_dotenv_path() {
  local override="${AGENTD_DOTENV_PATH:-}"
  if [[ -n "${override}" ]]; then
    if [[ "${override}" == "~/"* && -n "${HOME:-}" ]]; then
      override="${HOME}/${override#~/}"
    fi
    if [[ -f "${override}" ]]; then
      echo "${override}"
      return 0
    fi
  fi
  local home_dir=""
  home_dir="$(agent_env_home_dir || true)"
  if [[ -z "${home_dir}" ]]; then
    return 1
  fi
  local env_file="${home_dir}/.env"
  if [[ ! -f "${env_file}" ]]; then
    return 1
  fi
  echo "${env_file}"
}

agent_env_source_home() {
  local env_file=""
  env_file="$(agent_env_dotenv_path || true)"
  if [[ -z "${env_file}" || ! -f "${env_file}" ]]; then
    return 1
  fi
  set -a
  # shellcheck disable=SC1090
  source "${env_file}"
  set +a
  return 0
}

agent_env_source_home_if_unset() {
  local env_file=""
  env_file="$(agent_env_dotenv_path || true)"
  if [[ -z "${env_file}" || ! -f "${env_file}" ]]; then
    return 1
  fi

  local line key val trimmed
  while IFS= read -r line || [[ -n "${line}" ]]; do
    # Trim leading/trailing whitespace.
    trimmed="${line#"${line%%[![:space:]]*}"}"
    trimmed="${trimmed%"${trimmed##*[![:space:]]}"}"
    if [[ -z "${trimmed}" || "${trimmed}" == \#* ]]; then
      continue
    fi
    if [[ "${trimmed}" =~ ^(export[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=[[:space:]]*(.*)$ ]]; then
      key="${BASH_REMATCH[2]}"
      val="${BASH_REMATCH[3]}"
      if [[ -n "${!key:-}" ]]; then
        continue
      fi
      # Strip inline comments for unquoted values.
      if [[ "${val}" != \"*\" && "${val}" != \'*\' ]]; then
        val="${val%%#*}"
      fi
      val="${val#"${val%%[![:space:]]*}"}"
      val="${val%"${val##*[![:space:]]}"}"
      if [[ "${val}" == \"*\" && "${val}" == *\" ]]; then
        val="${val#\"}"
        val="${val%\"}"
      elif [[ "${val}" == \'*\' && "${val}" == *\' ]]; then
        val="${val#\'}"
        val="${val%\'}"
      fi
      export "${key}=${val}"
    fi
  done <"${env_file}"
  return 0
}
