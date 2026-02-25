#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

label="com.agentd.spawn-adapter"

spawn_bin="${SPAWN_ADAPTER_BIN:-/usr/local/bin/agentd-spawn-adapter}"
broker_base="${BROKER_BASE:-}"
broker_token="${BROKER_OIDC_TOKEN:-}"
broker_token_file="${BROKER_OIDC_TOKEN_FILE:-}"
broker_insecure="${BROKER_INSECURE_TLS:-}"
spawn_log_dir="${SPAWN_ADAPTER_LOG_DIR:-${HOME}/Library/Logs}"
spawn_plist_path="${SPAWN_ADAPTER_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"
spawn_poll_interval="${SPAWN_ADAPTER_POLL_INTERVAL:-3s}"
spawn_status="${SPAWN_ADAPTER_STATUS:-requested}"
spawn_limit="${SPAWN_ADAPTER_LIMIT:-50}"
spawn_command_timeout="${SPAWN_ADAPTER_COMMAND_TIMEOUT:-2m}"
spawn_command="${SPAWN_COMMAND:-}"
spawn_allocator="${SPAWN_ALLOCATOR:-}"
spawn_adapter_id="${SPAWN_ADAPTER_ID:-}"
spawn_extra_args="${SPAWN_ADAPTER_EXTRA_ARGS:-}"

is_truthy() {
  case "${1}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

if [[ ! -x "${spawn_bin}" ]]; then
  echo "spawn adapter binary not found or not executable: ${spawn_bin}" >&2
  exit 1
fi
if [[ -z "${broker_base}" || ( -z "${broker_token}" && -z "${broker_token_file}" ) ]]; then
  echo "missing BROKER_BASE or BROKER_OIDC_TOKEN/BROKER_OIDC_TOKEN_FILE" >&2
  exit 1
fi
if [[ -z "${spawn_command}" ]] && ! is_truthy "${spawn_allocator}"; then
  echo "missing SPAWN_COMMAND (or set SPAWN_ALLOCATOR=1)" >&2
  exit 1
fi

mkdir -p "${spawn_log_dir}" "$(dirname "${spawn_plist_path}")"

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
  "${spawn_bin}"
  "--poll-interval" "${spawn_poll_interval}"
  "--status" "${spawn_status}"
  "--limit" "${spawn_limit}"
  "--command-timeout" "${spawn_command_timeout}"
)

if [[ -n "${spawn_command}" ]]; then
  args+=("--command" "${spawn_command}")
fi

if is_truthy "${spawn_allocator}"; then
  args+=("--allocator")
fi

if [[ -n "${spawn_extra_args}" ]]; then
  read -r -a extra_list <<< "${spawn_extra_args}"
  if [[ "${#extra_list[@]}" -gt 0 ]]; then
    args+=("${extra_list[@]}")
  fi
fi

stdout_path="${spawn_log_dir}/spawn_adapter.out.log"
stderr_path="${spawn_log_dir}/spawn_adapter.err.log"

{
  echo '<?xml version="1.0" encoding="UTF-8"?>'
  echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
  echo '<plist version="1.0">'
  echo '<dict>'
  echo "  <key>Label</key><string>${label}</string>"
  echo '  <key>EnvironmentVariables</key>'
  echo '  <dict>'
  echo "    <key>BROKER_BASE</key><string>$(xml_escape "${broker_base}")</string>"
  if [[ -n "${broker_token}" ]]; then
    echo "    <key>BROKER_OIDC_TOKEN</key><string>$(xml_escape "${broker_token}")</string>"
  fi
  if [[ -n "${broker_token_file}" ]]; then
    echo "    <key>BROKER_OIDC_TOKEN_FILE</key><string>$(xml_escape "${broker_token_file}")</string>"
  fi
  if [[ -n "${broker_insecure}" ]]; then
    echo "    <key>BROKER_INSECURE_TLS</key><string>$(xml_escape "${broker_insecure}")</string>"
  fi
  if [[ -n "${spawn_command}" ]]; then
    echo "    <key>SPAWN_COMMAND</key><string>$(xml_escape "${spawn_command}")</string>"
  fi
  if [[ -n "${spawn_allocator}" ]]; then
    echo "    <key>SPAWN_ALLOCATOR</key><string>$(xml_escape "${spawn_allocator}")</string>"
  fi
  if [[ -n "${spawn_adapter_id}" ]]; then
    echo "    <key>SPAWN_ADAPTER_ID</key><string>$(xml_escape "${spawn_adapter_id}")</string>"
  fi
  echo '  </dict>'
  echo '  <key>ProgramArguments</key>'
  echo '  <array>'
  for arg in "${args[@]}"; do
    echo "    <string>$(xml_escape "${arg}")</string>"
  done
  echo '  </array>'
  echo "  <key>WorkingDirectory</key><string>$(xml_escape "${repo_root}")</string>"
  echo '  <key>RunAtLoad</key><true/>'
  echo '  <key>KeepAlive</key><true/>'
  echo "  <key>StandardOutPath</key><string>$(xml_escape "${stdout_path}")</string>"
  echo "  <key>StandardErrorPath</key><string>$(xml_escape "${stderr_path}")</string>"
  echo '</dict>'
  echo '</plist>'
} > "${spawn_plist_path}"

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${spawn_plist_path}" >/dev/null 2>&1 || true
launchctl bootstrap "${launch_target}" "${spawn_plist_path}"
launchctl enable "${launch_target}/${label}" >/dev/null 2>&1 || true
launchctl kickstart -k "${launch_target}/${label}" >/dev/null 2>&1 || true

echo "Installed launchd agent: ${spawn_plist_path}"
echo "Logs: ${stdout_path} / ${stderr_path}"
