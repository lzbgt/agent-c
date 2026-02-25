#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

label="com.agentd.orchestrator"

orch_bin="${ORCHESTRATOR_BIN:-/usr/local/bin/agentd-orchestrator}"
broker_base="${BROKER_BASE:-}"
broker_token="${BROKER_OIDC_TOKEN:-}"
broker_token_file="${BROKER_OIDC_TOKEN_FILE:-}"
broker_insecure="${BROKER_INSECURE_TLS:-}"
orch_log_dir="${ORCHESTRATOR_LOG_DIR:-${HOME}/Library/Logs}"
orch_plist_path="${ORCHESTRATOR_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"
orch_poll_interval="${ORCHESTRATOR_POLL_INTERVAL:-5s}"
orch_status="${ORCHESTRATOR_STATUS:-running}"
orch_limit="${ORCHESTRATOR_LIMIT:-50}"
orch_include_planned="${ORCHESTRATOR_INCLUDE_PLANNED:-1}"
orch_extra_args="${ORCHESTRATOR_EXTRA_ARGS:-}"
orch_id="${ORCHESTRATOR_ID:-}"

if [[ ! -x "${orch_bin}" ]]; then
  echo "orchestrator binary not found or not executable: ${orch_bin}" >&2
  exit 1
fi
if [[ -z "${broker_base}" || ( -z "${broker_token}" && -z "${broker_token_file}" ) ]]; then
  echo "missing BROKER_BASE or BROKER_OIDC_TOKEN/BROKER_OIDC_TOKEN_FILE" >&2
  exit 1
fi

mkdir -p "${orch_log_dir}" "$(dirname "${orch_plist_path}")"

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
  "${orch_bin}"
  "--poll-interval" "${orch_poll_interval}"
  "--status" "${orch_status}"
  "--limit" "${orch_limit}"
)

if [[ "${orch_include_planned}" == "0" || "${orch_include_planned}" == "false" ]]; then
  args+=("--include-planned=false")
fi

if [[ -n "${orch_extra_args}" ]]; then
  read -r -a extra_list <<< "${orch_extra_args}"
  if [[ "${#extra_list[@]}" -gt 0 ]]; then
    args+=("${extra_list[@]}")
  fi
fi

stdout_path="${orch_log_dir}/orchestrator.out.log"
stderr_path="${orch_log_dir}/orchestrator.err.log"

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
  if [[ -n "${orch_id}" ]]; then
    echo "    <key>ORCHESTRATOR_ID</key><string>$(xml_escape "${orch_id}")</string>"
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
} > "${orch_plist_path}"

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${orch_plist_path}" >/dev/null 2>&1 || true
launchctl bootstrap "${launch_target}" "${orch_plist_path}"
launchctl enable "${launch_target}/${label}" >/dev/null 2>&1 || true
launchctl kickstart -k "${launch_target}/${label}" >/dev/null 2>&1 || true

echo "Installed launchd agent: ${orch_plist_path}"
echo "Logs: ${stdout_path} / ${stderr_path}"
