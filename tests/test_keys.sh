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

agent_test_get_env_value_from_file() {
  if [[ $# -ne 2 ]]; then
    echo "agent_test_get_env_value_from_file: usage: <file> <key>" >&2
    return 2
  fi
  local file="${1}"
  local key="${2}"
  if [[ ! -f "${file}" ]]; then
    return 1
  fi
  python3 - "${file}" "${key}" <<'PY'
import re
import sys

path = sys.argv[1]
key = sys.argv[2]
try:
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()
except Exception:
    raise SystemExit(1)

pat = re.compile(r"^\\s*(?:export\\s+)?%s\\s*=\\s*(.*)$" % re.escape(key))
for line in lines:
    raw = line.strip()
    if not raw or raw.startswith("#"):
        continue
    m = pat.match(line)
    if not m:
        continue
    val = m.group(1).strip()
    if not val:
        continue
    if val[0] not in ("'", '"'):
        val = val.split("#", 1)[0].strip()
    if len(val) >= 2 and ((val[0] == '"' and val[-1] == '"') or (val[0] == "'" and val[-1] == "'")):
        val = val[1:-1]
    if val:
        print(val)
        raise SystemExit(0)
raise SystemExit(1)
PY
}

agent_test_load_openrouter_headers_if_unset() {
  local root
  root="$(agent_test_project_root)"
  local preferred="${root}/.not_in_repo"
  local fallback="${root}/project.local.md"
  local home_env=""
  home_env="$(agent_env_dotenv_path || true)"

  if [[ -z "${OPENROUTER_HTTP_REFERER:-}" ]]; then
    local v=""
    v="$(agent_test_get_env_value_from_file "${preferred}" "OPENROUTER_HTTP_REFERER" 2>/dev/null || true)"
    if [[ -z "${v}" ]]; then
      v="$(agent_test_get_env_value_from_file "${fallback}" "OPENROUTER_HTTP_REFERER" 2>/dev/null || true)"
    fi
    if [[ -z "${v}" && -n "${home_env}" ]]; then
      v="$(agent_test_get_env_value_from_file "${home_env}" "OPENROUTER_HTTP_REFERER" 2>/dev/null || true)"
    fi
    if [[ -n "${v}" ]]; then
      export OPENROUTER_HTTP_REFERER="${v}"
    fi
  fi

  if [[ -z "${OPENROUTER_X_TITLE:-}" ]]; then
    local v=""
    v="$(agent_test_get_env_value_from_file "${preferred}" "OPENROUTER_X_TITLE" 2>/dev/null || true)"
    if [[ -z "${v}" ]]; then
      v="$(agent_test_get_env_value_from_file "${fallback}" "OPENROUTER_X_TITLE" 2>/dev/null || true)"
    fi
    if [[ -z "${v}" && -n "${home_env}" ]]; then
      v="$(agent_test_get_env_value_from_file "${home_env}" "OPENROUTER_X_TITLE" 2>/dev/null || true)"
    fi
    if [[ -n "${v}" ]]; then
      export OPENROUTER_X_TITLE="${v}"
    fi
  fi
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
  home_env="$(agent_env_dotenv_path || true)"

  local k
  k="$(agent_test_get_key_from_file "${preferred}" "${provider}")" && { echo "${k}"; return 0; }
  k="$(agent_test_get_key_from_file "${fallback}" "${provider}")" && { echo "${k}"; return 0; }
  if [[ -n "${home_env}" ]]; then
    k="$(agent_test_get_key_from_file "${home_env}" "${provider}")" && { echo "${k}"; return 0; }
  fi

  if [[ "${provider}" == "openrouter" ]]; then
    local moonshot_hint=""
    if [[ -n "${KIMI_API_KEY_CN:-}" || -n "${MOONSHOT_API_KEY:-}" || -n "${MOONSHOT_API_KEY_CN:-}" ]]; then
      moonshot_hint="env"
    else
      if agent_test_get_key_from_file "${preferred}" "moonshot" >/dev/null 2>&1; then
        moonshot_hint=".not_in_repo"
      elif agent_test_get_key_from_file "${fallback}" "moonshot" >/dev/null 2>&1; then
        moonshot_hint="project.local.md"
      elif [[ -n "${home_env}" ]] && agent_test_get_key_from_file "${home_env}" "moonshot" >/dev/null 2>&1; then
        moonshot_hint="~/.env"
      fi
    fi
    if [[ -n "${moonshot_hint}" ]]; then
      echo "NOTE: Found Moonshot key (${moonshot_hint}); OpenRouter requires OPENROUTER_API_KEY." >&2
    fi
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
  home_env="$(agent_env_dotenv_path || true)"

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
  # is present at localhost:8120 by default (host), or host.docker.internal:8120 when running in a container.
  local default_proxy="${1:-}"
  local host_internal_ok="0"
  if [[ -z "${default_proxy}" ]]; then
    default_proxy="http://localhost:8120"
    if [[ -f "/.dockerenv" ]] || [[ -n "${container:-}" ]] || (grep -qa "docker" /proc/1/cgroup 2>/dev/null); then
      if python3 - <<'PY' >/dev/null 2>&1
import socket
try:
    socket.getaddrinfo("host.docker.internal", 8120)
except Exception:
    raise SystemExit(1)
raise SystemExit(0)
PY
      then
        default_proxy="http://host.docker.internal:8120"
        host_internal_ok="1"
      fi
    fi
  fi

  if [[ "${AGENT_TEST_DISABLE_PROXY:-}" == "1" ]]; then
    unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY
    export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost,::1}}"
    if [[ "${host_internal_ok}" == "1" ]]; then
      no_proxy="${no_proxy},host.docker.internal"
    fi
    export NO_PROXY="${no_proxy}"
    return 0
  fi

  export https_proxy="${https_proxy:-${HTTPS_PROXY:-${default_proxy}}}"
  export http_proxy="${http_proxy:-${HTTP_PROXY:-${default_proxy}}}"
  export HTTPS_PROXY="${https_proxy}"
  export HTTP_PROXY="${http_proxy}"

  # Do not proxy localhost daemon traffic.
  export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost,::1}}"
  if [[ "${host_internal_ok}" == "1" ]]; then
    no_proxy="${no_proxy},host.docker.internal"
  fi
  export NO_PROXY="${no_proxy}"
}

agent_test_openrouter_auth_ok() {
  if [[ $# -lt 1 ]]; then
    echo "agent_test_openrouter_auth_ok: missing key arg" >&2
    return 2
  fi
  agent_test_load_openrouter_headers_if_unset || true
  local key="${1}"
  local base_url="${2:-https://openrouter.ai/api/v1}"
  local -a headers
  headers=(-H "Authorization: Bearer ${key}")
  if [[ -n "${OPENROUTER_HTTP_REFERER:-}" ]]; then
    headers+=(-H "HTTP-Referer: ${OPENROUTER_HTTP_REFERER}")
  fi
  if [[ -n "${OPENROUTER_X_TITLE:-}" ]]; then
    headers+=(-H "X-Title: ${OPENROUTER_X_TITLE}")
  fi
  local resp status body
  resp="$(curl -sS --noproxy "*" -w "\n%{http_code}" \
    "${headers[@]}" \
    "${base_url}/models" || true)"
  status="${resp##*$'\n'}"
  body="${resp%$'\n'*}"
  if [[ "${status}" == "401" || "${status}" == "403" ]]; then
    local err
    err="$(printf "%s" "${body}" | python3 - <<'PY' 2>/dev/null || true
import json, sys
raw = sys.stdin.read()
try:
    obj = json.loads(raw)
except Exception:
    raise SystemExit
err = obj.get("error") or {}
msg = err.get("message") or ""
code = err.get("code") or ""
out = ""
if code:
    out = f"{code}"
if msg:
    out = f"{out}: {msg}" if out else msg
if out:
    print(out)
PY
)"
    if [[ -n "${err}" ]]; then
      echo "SKIP: OpenRouter auth error detail: ${err}" >&2
    fi
    echo "SKIP: OpenRouter auth failed (${status}); check OPENROUTER_API_KEY" >&2
    if [[ -z "${OPENROUTER_HTTP_REFERER:-}" && -z "${OPENROUTER_X_TITLE:-}" ]]; then
      echo "SKIP: OpenRouter may require OPENROUTER_HTTP_REFERER and OPENROUTER_X_TITLE headers" >&2
    fi
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
  local model="${AGENT_TEST_OPENROUTER_MODEL:-}"
  if [[ -z "${model}" ]]; then
    local py_script
    py_script="$(cat <<'PY'
import json
import sys

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
    raise SystemExit(0)
models = obj.get("data") or obj.get("models") or []
def score(m):
    arch = m.get("architecture") or {}
    inputs = arch.get("input_modalities") or []
    sp = m.get("supported_parameters") or []
    score = 0
    if "text" in inputs:
        score += 2
    if "tools" in sp:
        score += 1
    return score
best = None
best_score = -1
for m in models:
    mid = m.get("id") or m.get("name")
    if not isinstance(mid, str) or not mid:
        continue
    s = score(m)
    if s > best_score:
        best = mid
        best_score = s
if best:
    print(best)
PY
)"
    model="$(printf "%s" "${body}" | python3 -c "${py_script}" 2>/dev/null || true)"
  fi
  if [[ -z "${model}" ]]; then
    model="bytedance-seed/seed-1.6-flash"
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
  local -a chat_headers
  chat_headers=("${headers[@]}" -H "Content-Type: application/json")
  chat_resp="$(curl -sS --noproxy "*" -w "\n%{http_code}" \
    "${chat_headers[@]}" \
    -d "${chat_payload}" \
    "${base_url}/chat/completions" || true)"
  chat_status="${chat_resp##*$'\n'}"
  local chat_body="${chat_resp%$'\n'*}"
  if [[ "${chat_status}" == "401" || "${chat_status}" == "403" ]]; then
    local chat_err
    chat_err="$(printf "%s" "${chat_body}" | python3 - <<'PY' 2>/dev/null || true
import json, sys
raw = sys.stdin.read()
try:
    obj = json.loads(raw)
except Exception:
    raise SystemExit
err = obj.get("error") or {}
msg = err.get("message") or ""
code = err.get("code") or ""
out = ""
if code:
    out = f"{code}"
if msg:
    out = f"{out}: {msg}" if out else msg
if out:
    print(out)
PY
)"
    if [[ -n "${chat_err}" ]]; then
      echo "SKIP: OpenRouter auth error detail: ${chat_err}" >&2
    fi
    echo "SKIP: OpenRouter chat auth failed (${chat_status}); check OPENROUTER_API_KEY" >&2
    if [[ -z "${OPENROUTER_HTTP_REFERER:-}" && -z "${OPENROUTER_X_TITLE:-}" ]]; then
      echo "SKIP: OpenRouter may require OPENROUTER_HTTP_REFERER and OPENROUTER_X_TITLE headers" >&2
    fi
    return 77
  fi
  if [[ "${chat_status}" == "429" || "${chat_status}" == "503" ]]; then
    echo "SKIP: OpenRouter chat preflight throttled (${chat_status})" >&2
    return 77
  fi
  if agent_test_openrouter_output_is_auth_error "${chat_body}"; then
    local chat_err
    chat_err="$(printf "%s" "${chat_body}" | python3 - <<'PY' 2>/dev/null || true
import json, sys
raw = sys.stdin.read()
try:
    obj = json.loads(raw)
except Exception:
    raise SystemExit
err = obj.get("error") or {}
msg = err.get("message") or ""
code = err.get("code") or ""
out = ""
if code:
    out = f"{code}"
if msg:
    out = f"{out}: {msg}" if out else msg
if out:
    print(out)
PY
)"
    if [[ -n "${chat_err}" ]]; then
      echo "SKIP: OpenRouter auth error detail: ${chat_err}" >&2
    fi
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
