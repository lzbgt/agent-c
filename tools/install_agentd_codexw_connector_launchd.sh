#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

label="${AGENTD_CODEXW_LABEL:-com.agentd.codexw-connector}"
connector_bin="${AGENTD_CODEXW_CONNECTOR_BIN:-${repo_root}/tools/agentd_codexw_native_broker_connector.py}"
broker_url="${AGENTD_CODEXW_BROKER_URL:-}"
deployment_id="${AGENTD_CODEXW_DEPLOYMENT_ID:-}"
display_name="${AGENTD_CODEXW_DISPLAY_NAME:-${deployment_id}}"
identity_dir="${AGENTD_CODEXW_IDENTITY_DIR:-${HOME}/Library/Application Support/agentd/codexw-native}"
agentd_base_url="${AGENTD_BASE_URL:-http://127.0.0.1:8123}"
agentd_auth_token="${AGENTD_AUTH_TOKEN:-}"
broker_user="${AGENTD_CODEXW_BROKER_USER:-}"
broker_password="${AGENTD_CODEXW_BROKER_PASSWORD:-}"
enrollment_token_id="${AGENTD_CODEXW_ENROLLMENT_TOKEN_ID:-}"
enrollment_secret="${AGENTD_CODEXW_ENROLLMENT_SECRET:-}"
log_dir="${AGENTD_CODEXW_LOG_DIR:-${HOME}/Library/Logs}"
plist_path="${AGENTD_CODEXW_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"
dry_run="${AGENTD_CODEXW_DRY_RUN:-0}"

usage() {
  cat <<'EOF'
Usage:
  AGENTD_CODEXW_BROKER_URL=https://broker.example \
  AGENTD_CODEXW_DEPLOYMENT_ID=agentd-mac \
  AGENTD_AUTH_TOKEN=<agentd-token> \
  AGENTD_CODEXW_BROKER_USER=admin \
  AGENTD_CODEXW_BROKER_PASSWORD=<first-boot-password> \
    tools/install_agentd_codexw_connector_launchd.sh

Installs a launchd service for the native agentd -> codexw broker connector.
The connector is a foreground protocol adapter. launchd owns lifetime,
restart/backoff, logging, and boot startup.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ ! -f "${connector_bin}" ]]; then
  echo "native connector script not found: ${connector_bin}" >&2
  exit 1
fi
if [[ -z "${broker_url}" ]]; then
  echo "AGENTD_CODEXW_BROKER_URL is required" >&2
  exit 2
fi
if [[ -z "${deployment_id}" ]]; then
  echo "AGENTD_CODEXW_DEPLOYMENT_ID is required" >&2
  exit 2
fi

mkdir -p "${identity_dir}" "${log_dir}" "$(dirname "${plist_path}")"

xml_escape() {
  local s="$1"
  s="${s//&/&amp;}"
  s="${s//</&lt;}"
  s="${s//>/&gt;}"
  s="${s//\"/&quot;}"
  printf '%s' "${s}"
}

args=(
  "/usr/bin/env" "python3" "${connector_bin}"
  "--broker-url" "${broker_url}"
  "--deployment-id" "${deployment_id}"
  "--display-name" "${display_name:-${deployment_id}}"
  "--identity-dir" "${identity_dir}"
  "--agentd-base-url" "${agentd_base_url}"
  "--bootstrap-identity"
  "--connect"
)

if [[ -n "${agentd_auth_token}" ]]; then
  args+=("--agentd-auth-token" "${agentd_auth_token}")
fi
if [[ -n "${broker_user}" ]]; then
  args+=("--broker-user" "${broker_user}")
fi
if [[ -n "${broker_password}" ]]; then
  args+=("--broker-password" "${broker_password}")
fi
if [[ -n "${enrollment_token_id}" ]]; then
  args+=("--enrollment-token-id" "${enrollment_token_id}")
fi
if [[ -n "${enrollment_secret}" ]]; then
  args+=("--enrollment-shared-secret" "${enrollment_secret}")
fi

stdout_path="${log_dir}/agentd-codexw-connector.out.log"
stderr_path="${log_dir}/agentd-codexw-connector.err.log"

{
  echo '<?xml version="1.0" encoding="UTF-8"?>'
  echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
  echo '<plist version="1.0">'
  echo '<dict>'
  echo "  <key>Label</key><string>$(xml_escape "${label}")</string>"
  echo '  <key>ProgramArguments</key>'
  echo '  <array>'
  for arg in "${args[@]}"; do
    echo "    <string>$(xml_escape "${arg}")</string>"
  done
  echo '  </array>'
  echo "  <key>WorkingDirectory</key><string>$(xml_escape "${identity_dir}")</string>"
  echo '  <key>RunAtLoad</key><true/>'
  echo '  <key>KeepAlive</key><true/>'
  echo "  <key>StandardOutPath</key><string>$(xml_escape "${stdout_path}")</string>"
  echo "  <key>StandardErrorPath</key><string>$(xml_escape "${stderr_path}")</string>"
  echo '</dict>'
  echo '</plist>'
} > "${plist_path}"

if [[ "${dry_run}" == "1" || "${dry_run}" == "true" ]]; then
  echo "Wrote launchd agent plist: ${plist_path}"
  echo "Dry run enabled; not loading launchd service."
  exit 0
fi

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${plist_path}" >/dev/null 2>&1 || true
launchctl bootstrap "${launch_target}" "${plist_path}"
launchctl enable "${launch_target}/${label}" >/dev/null 2>&1 || true
launchctl kickstart -k "${launch_target}/${label}" >/dev/null 2>&1 || true

echo "Installed launchd agent: ${plist_path}"
echo "Logs: ${stdout_path} / ${stderr_path}"
