#!/usr/bin/env bash
set -euo pipefail

label="com.agentd.spawn-adapter"
spawn_plist_path="${SPAWN_ADAPTER_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"

uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${spawn_plist_path}" >/dev/null 2>&1 || true
if [[ -f "${spawn_plist_path}" ]]; then
  rm -f "${spawn_plist_path}"
fi

echo "Uninstalled launchd agent: ${spawn_plist_path}"
