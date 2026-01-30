#!/usr/bin/env bash
set -euo pipefail

# Key discovery for network smoke tests and local tooling.
#
# Priority:
# 1) Environment variables (OPENROUTER_API_KEY / DEEPSEEK_API_KEY)
# 2) project-local, gitignored file: project.local.md
#
# Expected file format (one per line):
# - deepseek: sk-...
# - openrouter: sk-or-v1-...
#
# We intentionally do NOT read keys from tracked docs like project.md.

agent_test_project_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  (cd "${script_dir}/.." && pwd)
}

agent_test_get_key() {
  if [[ $# -ne 1 ]]; then
    echo "agent_test_get_key: missing provider arg" >&2
    return 2
  fi
  local provider="${1}"
  local env_var=""
  case "${provider}" in
    deepseek) env_var="DEEPSEEK_API_KEY" ;;
    openrouter) env_var="OPENROUTER_API_KEY" ;;
    *)
      echo "agent_test_get_key: unknown provider: ${provider}" >&2
      return 2
      ;;
  esac

  local key="${!env_var:-}"
  if [[ -n "${key}" ]]; then
    echo "${key}"
    return 0
  fi

  local root file
  root="$(agent_test_project_root)"
  file="${root}/project.local.md"
  if [[ ! -f "${file}" ]]; then
    return 1
  fi

  # Extract only plausible-looking keys (avoid placeholders like "<YOUR_KEY>").
  # Accepts DeepSeek and OpenRouter key prefixes (both start with "sk-").
  key="$(
    grep -E "^[[:space:]]*- ${provider}:[[:space:]]*sk-[A-Za-z0-9_-]+" "${file}" \
      | head -n 1 \
      | sed -E "s/^[[:space:]]*- ${provider}:[[:space:]]*//"
  )"
  if [[ -z "${key}" ]]; then
    return 1
  fi
  if [[ ! "${key}" =~ ^sk-[A-Za-z0-9_-]+$ ]]; then
    return 1
  fi
  echo "${key}"
  return 0
}

agent_test_setup_proxy_env() {
  # Some environments require an HTTP proxy for outbound HTTPS. Many of this repo's network tests assume one
  # is present at localhost:8120 by default.
  local default_proxy="${1:-http://localhost:8120}"

  export https_proxy="${https_proxy:-${HTTPS_PROXY:-${default_proxy}}}"
  export http_proxy="${http_proxy:-${HTTP_PROXY:-${default_proxy}}}"
  export HTTPS_PROXY="${https_proxy}"
  export HTTP_PROXY="${http_proxy}"

  # Do not proxy localhost daemon traffic.
  export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost}}"
  export NO_PROXY="${no_proxy}"
}
