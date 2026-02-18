#!/usr/bin/env bash
set -euo pipefail

# Key discovery for network smoke tests and local tooling.
#
# Priority:
# 1) Environment variables (OPENROUTER_API_KEY / DEEPSEEK_API_KEY)
# 2) project-local, gitignored file: .not_in_repo (preferred local secrets)
# 3) project-local, gitignored file: project.local.md
# 4) ~/.env (developer convenience; not exported by default)
#    - when HOME is missing (service contexts), fall back to passwd-derived home.
#
# Proxy control:
# - AGENT_TEST_DISABLE_PROXY=1 disables the default localhost proxy for network tests.
#
# Expected file format (one per line):
# - deepseek: sk-...
# - openrouter: sk-or-v1-...
# - moonshot: sk-...
#
# We intentionally do NOT read keys from tracked docs like project.md.

AGENT_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/agent_env.sh
source "${AGENT_TEST_ROOT}/tools/lib/agent_env.sh"

agent_test_project_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  (cd "${script_dir}/.." && pwd)
}

agent_test_get_key_from_file() {
  if [[ $# -ne 2 ]]; then
    echo "agent_test_get_key_from_file: usage: <file> <provider>" >&2
    return 2
  fi
  local file="${1}"
  local provider="${2}"
  if [[ ! -f "${file}" ]]; then
    return 1
  fi

  # Accept a minimal YAML-ish format (same as project.local.md):
  # - deepseek: sk-...
  # - openrouter: sk-or-v1-...
  #
  # Also accept env-style lines:
  # DEEPSEEK_API_KEY=sk-...
  # OPENROUTER_API_KEY=sk-...
  local key=""
  key="$(
    grep -E "^[[:space:]]*- ${provider}:[[:space:]]*sk-[A-Za-z0-9_.-]+" "${file}" \
      | head -n 1 \
      | sed -E "s/^[[:space:]]*- ${provider}:[[:space:]]*//"
  )"
  if [[ -z "${key}" ]]; then
    local env_var=""
    case "${provider}" in
      deepseek) env_var="DEEPSEEK_API_KEY" ;;
      openrouter) env_var="OPENROUTER_API_KEY" ;;
      moonshot) env_var="KIMI_API_KEY_CN" ;;
      *) return 1 ;;
    esac
    local env_re="^[[:space:]]*(export[[:space:]]+)?${env_var}[[:space:]]*=[[:space:]]*['\\\"]?(sk-[A-Za-z0-9_.-]+)['\\\"]?([[:space:]]+#.*)?$"
    key="$(
      grep -E "${env_re}" "${file}" \
        | head -n 1 \
        | sed -E "s/${env_re}/\\2/"
    )"
    if [[ -z "${key}" && "${provider}" == "moonshot" ]]; then
      # Accept common Moonshot env-style keys too.
      local moonshot_re="^[[:space:]]*(export[[:space:]]+)?MOONSHOT_API_KEY[[:space:]]*=[[:space:]]*['\\\"]?(sk-[A-Za-z0-9_.-]+)['\\\"]?([[:space:]]+#.*)?$"
      key="$(
        grep -E "${moonshot_re}" "${file}" \
          | head -n 1 \
          | sed -E "s/${moonshot_re}/\\2/"
      )"
      if [[ -z "${key}" ]]; then
        local moonshot_cn_re="^[[:space:]]*(export[[:space:]]+)?MOONSHOT_API_KEY_CN[[:space:]]*=[[:space:]]*['\\\"]?(sk-[A-Za-z0-9_.-]+)['\\\"]?([[:space:]]+#.*)?$"
        key="$(
          grep -E "${moonshot_cn_re}" "${file}" \
            | head -n 1 \
            | sed -E "s/${moonshot_cn_re}/\\2/"
        )"
      fi
    fi
  fi
  if [[ -z "${key}" ]]; then
    return 1
  fi
  if [[ ! "${key}" =~ ^sk-[A-Za-z0-9_.-]+$ ]]; then
    return 1
  fi
  echo "${key}"
  return 0
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
    moonshot) env_var="KIMI_API_KEY_CN" ;;
    *)
      echo "agent_test_get_key: unknown provider: ${provider}" >&2
      return 2
      ;;
  esac

  local key="${!env_var:-}"
  if [[ -z "${key}" && "${provider}" == "moonshot" ]]; then
    key="${MOONSHOT_API_KEY:-}"
    if [[ -z "${key}" ]]; then
      key="${MOONSHOT_API_KEY_CN:-}"
    fi
  fi
  if [[ -n "${key}" ]]; then
    echo "${key}"
    return 0
  fi

  local root file
  root="$(agent_test_project_root)"
  local preferred="${root}/.not_in_repo"
  local fallback="${root}/project.local.md"
  local home_env=""
  local home_dir=""
  home_dir="$(agent_env_home_dir || true)"
  if [[ -n "${home_dir}" ]]; then
    home_env="${home_dir}/.env"
  fi

  local k
  k="$(agent_test_get_key_from_file "${preferred}" "${provider}")" && { echo "${k}"; return 0; }
  k="$(agent_test_get_key_from_file "${fallback}" "${provider}")" && { echo "${k}"; return 0; }
  if [[ -n "${home_env}" ]]; then
    k="$(agent_test_get_key_from_file "${home_env}" "${provider}")" && { echo "${k}"; return 0; }
  fi

  return 1
}

agent_test_get_key_source() {
  if [[ $# -ne 1 ]]; then
    echo "agent_test_get_key_source: missing provider arg" >&2
    return 2
  fi
  local provider="${1}"
  local env_var=""
  case "${provider}" in
    deepseek) env_var="DEEPSEEK_API_KEY" ;;
    openrouter) env_var="OPENROUTER_API_KEY" ;;
    moonshot) env_var="KIMI_API_KEY_CN" ;;
    *)
      echo "agent_test_get_key_source: unknown provider: ${provider}" >&2
      return 2
      ;;
  esac

  if [[ -n "${!env_var:-}" ]]; then
    echo "env:${env_var}"
    return 0
  fi
  if [[ "${provider}" == "moonshot" ]]; then
    if [[ -n "${MOONSHOT_API_KEY:-}" ]]; then
      echo "env:MOONSHOT_API_KEY"
      return 0
    fi
    if [[ -n "${MOONSHOT_API_KEY_CN:-}" ]]; then
      echo "env:MOONSHOT_API_KEY_CN"
      return 0
    fi
  fi

  local root
  root="$(agent_test_project_root)"
  local preferred="${root}/.not_in_repo"
  local fallback="${root}/project.local.md"
  local home_env=""
  local home_dir=""
  home_dir="$(agent_env_home_dir || true)"
  if [[ -n "${home_dir}" ]]; then
    home_env="${home_dir}/.env"
  fi

  if agent_test_get_key_from_file "${preferred}" "${provider}" >/dev/null 2>&1; then
    echo ".not_in_repo"
    return 0
  fi
  if agent_test_get_key_from_file "${fallback}" "${provider}" >/dev/null 2>&1; then
    echo "project.local.md"
    return 0
  fi
  if [[ -n "${home_env}" ]] && agent_test_get_key_from_file "${home_env}" "${provider}" >/dev/null 2>&1; then
    echo "~/.env"
    return 0
  fi
  return 1
}

agent_test_setup_proxy_env() {
  # Some environments require an HTTP proxy for outbound HTTPS. Many of this repo's network tests assume one
  # is present at localhost:8120 by default.
  local default_proxy="${1:-http://localhost:8120}"

  if [[ "${AGENT_TEST_DISABLE_PROXY:-}" == "1" ]]; then
    unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY
    export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost}}"
    export NO_PROXY="${no_proxy}"
    return 0
  fi

  export https_proxy="${https_proxy:-${HTTPS_PROXY:-${default_proxy}}}"
  export http_proxy="${http_proxy:-${HTTP_PROXY:-${default_proxy}}}"
  export HTTPS_PROXY="${https_proxy}"
  export HTTP_PROXY="${http_proxy}"

  # Do not proxy localhost daemon traffic.
  export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost}}"
  export NO_PROXY="${no_proxy}"
}

agent_test_openrouter_auth_ok() {
  if [[ $# -lt 1 ]]; then
    echo "agent_test_openrouter_auth_ok: missing key arg" >&2
    return 2
  fi
  local key="${1}"
  local base_url="${2:-https://openrouter.ai/api/v1}"
  local resp status
  resp="$(curl -sS --noproxy "*" -w "\n%{http_code}" \
    -H "Authorization: Bearer ${key}" \
    "${base_url}/models" || true)"
  status="${resp##*$'\n'}"
  if [[ "${status}" == "401" || "${status}" == "403" ]]; then
    echo "SKIP: OpenRouter auth failed (${status}); check OPENROUTER_API_KEY" >&2
    return 77
  fi
  if [[ "${status}" -ge 400 || "${status}" == "000" ]]; then
    echo "OpenRouter auth check failed (status=${status})" >&2
    return 1
  fi
  if [[ "${AGENT_TEST_OPENROUTER_SKIP_CHAT_PREFLIGHT:-}" == "1" ]]; then
    return 0
  fi

  local body="${resp%$'\n'*}"
  local model
  model="$(printf "%s" "${body}" | python3 - <<'PY'
import json, sys
raw = sys.stdin.buffer.read()
if not raw:
    sys.exit(0)
try:
    text = raw.decode("utf-8", errors="ignore")
except Exception:
    sys.exit(0)
try:
    obj = json.loads(text)
except Exception:
    sys.exit(0)
rec = obj.get("recommended_model")
if isinstance(rec, str) and rec:
    print(rec)
PY
)"
  if [[ -z "${model}" ]]; then
    model="${AGENT_TEST_OPENROUTER_MODEL:-bytedance-seed/seed-1.6-flash}"
  fi
  if [[ -z "${model}" ]]; then
    return 0
  fi

  local chat_payload
  chat_payload="$(python3 - <<PY
import json
print(json.dumps({
  "model": "${model}",
  "messages": [{"role":"user","content":"ping"}],
  "max_tokens": 1
}))
PY
)"
  local chat_resp chat_status
  chat_resp="$(curl -sS --noproxy "*" -w "\n%{http_code}" \
    -H "Authorization: Bearer ${key}" \
    -H "Content-Type: application/json" \
    -d "${chat_payload}" \
    "${base_url}/chat/completions" || true)"
  chat_status="${chat_resp##*$'\n'}"
  if [[ "${chat_status}" == "401" || "${chat_status}" == "403" ]]; then
    echo "SKIP: OpenRouter chat auth failed (${chat_status}); check OPENROUTER_API_KEY" >&2
    return 77
  fi
  if [[ "${chat_status}" == "429" || "${chat_status}" == "503" ]]; then
    echo "SKIP: OpenRouter chat preflight throttled (${chat_status})" >&2
    return 77
  fi
  local chat_body="${chat_resp%$'\n'*}"
  if agent_test_openrouter_output_is_auth_error "${chat_body}"; then
    echo "SKIP: OpenRouter chat auth error response" >&2
    return 77
  fi
  if [[ "${chat_status}" -ge 400 || "${chat_status}" == "000" ]]; then
    echo "SKIP: OpenRouter chat preflight failed (${chat_status}) for model ${model}" >&2
    return 77
  fi
  return 0
}

agent_test_openrouter_output_is_auth_error() {
  if [[ $# -lt 1 ]]; then
    return 1
  fi
  local lower
  lower="$(printf "%s" "${1}" | tr '[:upper:]' '[:lower:]')"
  if [[ "${lower}" == *"user not found"* ]]; then
    return 0
  fi
  if [[ "${lower}" == *"invalid api key"* || "${lower}" == *"invalid_api_key"* ]]; then
    return 0
  fi
  if [[ "${lower}" == *"unauthorized"* || "${lower}" == *"forbidden"* ]]; then
    return 0
  fi
  if [[ "${lower}" == *"http_status\":401"* || "${lower}" == *"status\":401"* ]]; then
    return 0
  fi
  return 1
}
