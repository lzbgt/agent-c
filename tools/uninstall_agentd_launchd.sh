#!/usr/bin/env bash
set -euo pipefail

label="com.agentd.daemon"
agentd_plist_path="${AGENTD_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${agentd_plist_path}" >/dev/null 2>&1 || true
if [[ -f "${agentd_plist_path}" ]]; then
  rm -f "${agentd_plist_path}"
fi

echo "Uninstalled launchd agent: ${agentd_plist_path}"
