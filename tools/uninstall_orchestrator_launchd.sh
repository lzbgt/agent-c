#!/usr/bin/env bash
set -euo pipefail

label="com.agentd.orchestrator"
orch_plist_path="${ORCHESTRATOR_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${orch_plist_path}" >/dev/null 2>&1 || true
if [[ -f "${orch_plist_path}" ]]; then
  rm -f "${orch_plist_path}"
fi

echo "Uninstalled launchd agent: ${orch_plist_path}"
