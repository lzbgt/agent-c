#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

label="com.agentd.daemon"

agentd_bin="${AGENTD_BIN:-${repo_root}/build/agentd}"
agentd_host="${AGENTD_HOST:-127.0.0.1}"
agentd_port="${AGENTD_PORT:-8123}"
agentd_auth_token="${AGENTD_AUTH_TOKEN:-}"
agentd_state_dir="${AGENTD_STATE_DIR:-${HOME}/Library/Application Support/agentd}"
agentd_db_path="${AGENTD_DB_PATH:-${agentd_state_dir}/agentd.db}"
agentd_log_dir="${AGENTD_LOG_DIR:-${HOME}/Library/Logs}"
agentd_plist_path="${AGENTD_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"
agentd_tools="${AGENTD_TOOLS:-host}"
agentd_yolo="${AGENTD_YOLO:-1}"
agentd_host_scope="${AGENTD_HOST_SCOPE:-}"
agentd_tools_root="${AGENTD_TOOLS_ROOT:-}"
agentd_cors_origins="${AGENTD_CORS_ORIGINS:-}"
agentd_extra_args="${AGENTD_EXTRA_ARGS:-}"
agentd_dotenv_path="${AGENTD_DOTENV_PATH:-${HOME}/.env}"
agentd_base_url="${AGENTD_BASE_URL:-https://api.deepseek.com}"
agentd_model="${AGENTD_MODEL:-deepseek-v4-pro}"
agentd_timeout_ms="${AGENTD_TIMEOUT_MS:-}"
agentd_proxy_url="${AGENTD_PROXY_URL:-}"
agentd_dry_run="${AGENTD_DRY_RUN:-0}"

if [[ ! -x "${agentd_bin}" ]]; then
  echo "agentd binary not found or not executable: ${agentd_bin}" >&2
  echo "Build it first: cmake --build build -j" >&2
  exit 1
fi

mkdir -p "${agentd_state_dir}" "${agentd_log_dir}" "$(dirname "${agentd_plist_path}")"

trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "${s}"
}

xml_escape() {
  local s="$1"
  s="${s//&/&amp;}"
  s="${s//</&lt;}"
  s="${s//>/&gt;}"
  s="${s//\"/&quot;}"
  printf '%s' "${s}"
}

args=(
  "${agentd_bin}"
  "--host" "${agentd_host}"
  "--port" "${agentd_port}"
  "--state-dir" "${agentd_state_dir}"
  "--db-path" "${agentd_db_path}"
)

if [[ -n "${agentd_tools}" ]]; then
  args+=("--tools" "${agentd_tools}")
fi

if [[ "${agentd_yolo}" == "1" || "${agentd_yolo}" == "true" ]]; then
  args+=("--yolo")
fi

if [[ -n "${agentd_host_scope}" ]]; then
  args+=("--host-scope" "${agentd_host_scope}")
fi

if [[ -n "${agentd_tools_root}" ]]; then
  args+=("--tools-root" "${agentd_tools_root}")
fi

if [[ -n "${agentd_base_url}" ]]; then
  args+=("--base-url" "${agentd_base_url}")
fi

if [[ -n "${agentd_model}" ]]; then
  args+=("--model" "${agentd_model}")
fi

if [[ -n "${agentd_timeout_ms}" ]]; then
  args+=("--timeout-ms" "${agentd_timeout_ms}")
fi

if [[ -n "${agentd_proxy_url}" ]]; then
  args+=("--proxy" "${agentd_proxy_url}")
fi

if [[ -n "${agentd_cors_origins}" ]]; then
  IFS=',' read -r -a cors_list <<< "${agentd_cors_origins}"
  for origin in "${cors_list[@]}"; do
    origin="$(trim "${origin}")"
    if [[ -n "${origin}" ]]; then
      args+=("--cors-origin" "${origin}")
    fi
  done
fi

if [[ -n "${agentd_extra_args}" ]]; then
  read -r -a extra_list <<< "${agentd_extra_args}"
  if [[ "${#extra_list[@]}" -gt 0 ]]; then
    args+=("${extra_list[@]}")
  fi
fi

stdout_path="${agentd_log_dir}/agentd.out.log"
stderr_path="${agentd_log_dir}/agentd.err.log"

{
  echo '<?xml version="1.0" encoding="UTF-8"?>'
  echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
  echo '<plist version="1.0">'
  echo '<dict>'
  echo "  <key>Label</key><string>${label}</string>"
  if [[ -n "${agentd_auth_token}" || -n "${agentd_dotenv_path}" ]]; then
    echo '  <key>EnvironmentVariables</key>'
    echo '  <dict>'
    if [[ -n "${agentd_auth_token}" ]]; then
      echo "    <key>AGENTD_AUTH_TOKEN</key><string>$(xml_escape "${agentd_auth_token}")</string>"
    fi
    if [[ -n "${agentd_dotenv_path}" ]]; then
      echo "    <key>AGENTD_DOTENV_PATH</key><string>$(xml_escape "${agentd_dotenv_path}")</string>"
    fi
    echo '  </dict>'
  fi
  echo '  <key>ProgramArguments</key>'
  echo '  <array>'
  for arg in "${args[@]}"; do
    echo "    <string>$(xml_escape "${arg}")</string>"
  done
  echo '  </array>'
  echo "  <key>WorkingDirectory</key><string>$(xml_escape "${agentd_state_dir}")</string>"
  echo '  <key>RunAtLoad</key><true/>'
  echo '  <key>KeepAlive</key><true/>'
  echo "  <key>StandardOutPath</key><string>$(xml_escape "${stdout_path}")</string>"
  echo "  <key>StandardErrorPath</key><string>$(xml_escape "${stderr_path}")</string>"
  echo '</dict>'
  echo '</plist>'
} > "${agentd_plist_path}"

if [[ "${agentd_dry_run}" == "1" || "${agentd_dry_run}" == "true" ]]; then
  echo "Wrote launchd agent: ${agentd_plist_path}"
  echo "Dry run enabled; not loading launchd service."
  exit 0
fi

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${agentd_plist_path}" >/dev/null 2>&1 || true
launchctl bootstrap "${launch_target}" "${agentd_plist_path}"
launchctl enable "${launch_target}/${label}" >/dev/null 2>&1 || true
launchctl kickstart -k "${launch_target}/${label}" >/dev/null 2>&1 || true

echo "Installed launchd agent: ${agentd_plist_path}"
echo "Logs: ${stdout_path} / ${stderr_path}"
