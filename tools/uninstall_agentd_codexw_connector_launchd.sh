#!/usr/bin/env bash
set -euo pipefail

label="${AGENTD_CODEXW_LABEL:-com.agentd.codexw-connector}"
plist_path="${AGENTD_CODEXW_PLIST_PATH:-${HOME}/Library/LaunchAgents/${label}.plist}"
self_test_label="${AGENTD_CODEXW_SELF_TEST_LABEL:-${label}.self-test}"
self_test_plist_path="${AGENTD_CODEXW_SELF_TEST_PLIST_PATH:-${HOME}/Library/LaunchAgents/${self_test_label}.plist}"
uid_num="$(id -u)"
launch_target="gui/${uid_num}"

launchctl bootout "${launch_target}" "${self_test_plist_path}" >/dev/null 2>&1 || true
launchctl bootout "${launch_target}" "${plist_path}" >/dev/null 2>&1 || true
rm -f "${self_test_plist_path}"
rm -f "${plist_path}"

echo "Uninstalled launchd agent: ${plist_path}"
echo "Uninstalled launchd self-test agent: ${self_test_plist_path}"
