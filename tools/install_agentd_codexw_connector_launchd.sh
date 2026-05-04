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
broker_token="${AGENTD_CODEXW_BROKER_TOKEN:-}"
enrollment_token_id="${AGENTD_CODEXW_ENROLLMENT_TOKEN_ID:-}"
enrollment_secret="${AGENTD_CODEXW_ENROLLMENT_SECRET:-}"
runtime_instance_id="${AGENTD_CODEXW_RUNTIME_INSTANCE_ID:-}"
runtime_update_mode="${AGENTD_CODEXW_RUNTIME_UPDATE_MODE:-disabled}"
runtime_restart_mode="${AGENTD_CODEXW_RUNTIME_RESTART_MODE:-disabled}"
require_update_preflight="${AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT:-0}"
self_test_output_path="${AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH:-${identity_dir}/self-test-status.json}"
self_test_stale_after_seconds="${AGENTD_CODEXW_SELF_TEST_STALE_AFTER_SECONDS:-900}"
log_dir="${AGENTD_CODEXW_LOG_DIR:-${HOME}/Library/Logs}"
plist_path="${AGENTD_CODEXW_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"
self_test_label="${AGENTD_CODEXW_SELF_TEST_LABEL:-${label}.self-test}"
self_test_plist_path="${AGENTD_CODEXW_SELF_TEST_PLIST_PATH:-${HOME}/Library/LaunchAgents/${self_test_label}.plist}"
self_test_interval_seconds="${AGENTD_CODEXW_SELF_TEST_INTERVAL_SECONDS:-300}"
install_self_test="${AGENTD_CODEXW_INSTALL_SELF_TEST:-1}"
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

By default this also installs a periodic read-only self-test LaunchAgent with
label "${AGENTD_CODEXW_LABEL}.self-test". Set
AGENTD_CODEXW_INSTALL_SELF_TEST=0 to skip it.
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
if [[ -z "${runtime_instance_id}" ]]; then
  runtime_instance_id="${deployment_id}-runtime"
fi

mkdir -p "${identity_dir}" "${log_dir}" "$(dirname "${plist_path}")" "$(dirname "${self_test_plist_path}")"

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
  "--runtime-instance-id" "${runtime_instance_id}"
  "--display-name" "${display_name:-${deployment_id}}"
  "--identity-dir" "${identity_dir}"
  "--agentd-base-url" "${agentd_base_url}"
  "--bootstrap-identity"
  "--connect"
)

env_keys=()
env_values=()
add_env() {
  local key="$1"
  local value="$2"
  if [[ -n "${value}" ]]; then
    env_keys+=("${key}")
    env_values+=("${value}")
  fi
}

add_env "AGENTD_AUTH_TOKEN" "${agentd_auth_token}"
add_env "AGENTD_CODEXW_BROKER_USER" "${broker_user}"
add_env "AGENTD_CODEXW_BROKER_PASSWORD" "${broker_password}"
add_env "AGENTD_CODEXW_BROKER_TOKEN" "${broker_token}"
add_env "AGENTD_CODEXW_RUNTIME_UPDATE_MODE" "${runtime_update_mode}"
add_env "AGENTD_CODEXW_RUNTIME_RESTART_MODE" "${runtime_restart_mode}"
add_env "AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT" "${require_update_preflight}"
add_env "AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH" "${self_test_output_path}"
add_env "AGENTD_CODEXW_SELF_TEST_STALE_AFTER_SECONDS" "${self_test_stale_after_seconds}"
add_env "AGENTD_CODEXW_ENROLLMENT_TOKEN_ID" "${enrollment_token_id}"
add_env "AGENTD_CODEXW_ENROLLMENT_SECRET" "${enrollment_secret}"

stdout_path="${log_dir}/agentd-codexw-connector.out.log"
stderr_path="${log_dir}/agentd-codexw-connector.err.log"
self_test_stdout_path="${log_dir}/agentd-codexw-connector-self-test.out.log"
self_test_stderr_path="${log_dir}/agentd-codexw-connector-self-test.err.log"

write_launchd_plist() {
  local target_label="$1"
  local stdout_log="$2"
  local stderr_log="$3"
  local keep_alive="$4"
  local start_interval="$5"
  shift 5
  local program_args=("$@")

  echo '<?xml version="1.0" encoding="UTF-8"?>'
  echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
  echo '<plist version="1.0">'
  echo '<dict>'
  echo "  <key>Label</key><string>$(xml_escape "${target_label}")</string>"
  echo '  <key>ProgramArguments</key>'
  echo '  <array>'
  for arg in "${program_args[@]}"; do
    echo "    <string>$(xml_escape "${arg}")</string>"
  done
  echo '  </array>'
  if [[ "${#env_keys[@]}" -gt 0 ]]; then
    echo '  <key>EnvironmentVariables</key>'
    echo '  <dict>'
    for idx in "${!env_keys[@]}"; do
      echo "    <key>$(xml_escape "${env_keys[$idx]}")</key><string>$(xml_escape "${env_values[$idx]}")</string>"
    done
    echo '  </dict>'
  fi
  echo "  <key>WorkingDirectory</key><string>$(xml_escape "${identity_dir}")</string>"
  echo '  <key>RunAtLoad</key><true/>'
  if [[ "${keep_alive}" == "1" ]]; then
    echo '  <key>KeepAlive</key><true/>'
  fi
  if [[ "${start_interval}" =~ ^[0-9]+$ && "${start_interval}" -gt 0 ]]; then
    echo "  <key>StartInterval</key><integer>${start_interval}</integer>"
  fi
  echo "  <key>StandardOutPath</key><string>$(xml_escape "${stdout_log}")</string>"
  echo "  <key>StandardErrorPath</key><string>$(xml_escape "${stderr_log}")</string>"
  echo '</dict>'
  echo '</plist>'
}

write_launchd_plist "${label}" "${stdout_path}" "${stderr_path}" "1" "0" "${args[@]}" > "${plist_path}"

self_test_args=(
  "/usr/bin/env" "python3" "${connector_bin}"
  "--broker-url" "${broker_url}"
  "--deployment-id" "${deployment_id}"
  "--runtime-instance-id" "${runtime_instance_id}"
  "--display-name" "${display_name:-${deployment_id}}"
  "--identity-dir" "${identity_dir}"
  "--agentd-base-url" "${agentd_base_url}"
  "--self-test"
  "--self-test-output-path" "${self_test_output_path}"
  "--require-broker-visible"
)
if [[ "${require_update_preflight}" == "1" || "${require_update_preflight}" == "true" ]]; then
  self_test_args+=("--require-update-preflight")
fi
if [[ "${install_self_test}" == "1" || "${install_self_test}" == "true" ]]; then
  write_launchd_plist \
    "${self_test_label}" \
    "${self_test_stdout_path}" \
    "${self_test_stderr_path}" \
    "0" \
    "${self_test_interval_seconds}" \
    "${self_test_args[@]}" > "${self_test_plist_path}"
fi

if [[ "${dry_run}" == "1" || "${dry_run}" == "true" ]]; then
  echo "Wrote launchd agent plist: ${plist_path}"
  if [[ "${install_self_test}" == "1" || "${install_self_test}" == "true" ]]; then
    echo "Wrote launchd self-test plist: ${self_test_plist_path}"
  fi
  echo "Dry run enabled; not loading launchd service."
  exit 0
fi

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${plist_path}" >/dev/null 2>&1 || true
launchctl bootstrap "${launch_target}" "${plist_path}"
launchctl enable "${launch_target}/${label}" >/dev/null 2>&1 || true
launchctl kickstart -k "${launch_target}/${label}" >/dev/null 2>&1 || true
if [[ "${install_self_test}" == "1" || "${install_self_test}" == "true" ]]; then
  launchctl bootout "${launch_target}" "${self_test_plist_path}" >/dev/null 2>&1 || true
  launchctl bootstrap "${launch_target}" "${self_test_plist_path}"
  launchctl enable "${launch_target}/${self_test_label}" >/dev/null 2>&1 || true
  launchctl kickstart -k "${launch_target}/${self_test_label}" >/dev/null 2>&1 || true
else
  launchctl bootout "${launch_target}" "${self_test_plist_path}" >/dev/null 2>&1 || true
  rm -f "${self_test_plist_path}"
fi

echo "Installed launchd agent: ${plist_path}"
if [[ "${install_self_test}" == "1" || "${install_self_test}" == "true" ]]; then
  echo "Installed launchd self-test agent: ${self_test_plist_path}"
fi
echo "Logs: ${stdout_path} / ${stderr_path}"
if [[ "${install_self_test}" == "1" || "${install_self_test}" == "true" ]]; then
  echo "Self-test logs: ${self_test_stdout_path} / ${self_test_stderr_path}"
fi
