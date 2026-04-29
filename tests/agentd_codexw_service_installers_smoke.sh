#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
AGENTD_BIN="${1:-${ROOT}/build/agentd}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agentd-service-installers.XXXXXX")"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

bash -n \
  "${ROOT}/tools/install_agentd_launchd.sh" \
  "${ROOT}/tools/uninstall_agentd_launchd.sh" \
  "${ROOT}/tools/install_agentd_codexw_connector_launchd.sh" \
  "${ROOT}/tools/uninstall_agentd_codexw_connector_launchd.sh" \
  "${ROOT}/tools/run_agentd_codexw_compat.sh"

AGENTD_BIN="${AGENTD_BIN}" \
AGENTD_AUTH_TOKEN="agentd-service-smoke-token" \
AGENTD_STATE_DIR="${TMP_DIR}/agentd-state" \
AGENTD_DB_PATH="${TMP_DIR}/agentd-state/agentd.db" \
AGENTD_PLIST_PATH="${TMP_DIR}/com.agentd.daemon.plist" \
AGENTD_LOG_DIR="${TMP_DIR}/logs" \
AGENTD_DRY_RUN=1 \
  "${ROOT}/tools/install_agentd_launchd.sh" >/dev/null

if grep -q -- "--host-scope\\|--tools-root" "${TMP_DIR}/com.agentd.daemon.plist"; then
  echo "agentd launchd plist contains stale host-scope/tools-root args" >&2
  exit 1
fi
if ! grep -q -- "--state-dir" "${TMP_DIR}/com.agentd.daemon.plist"; then
  echo "agentd launchd plist missing state-dir" >&2
  exit 1
fi

AGENTD_CODEXW_BROKER_URL="http://127.0.0.1:8787" \
AGENTD_CODEXW_DEPLOYMENT_ID="agentd-service-smoke" \
AGENTD_CODEXW_DISPLAY_NAME="agentd service smoke" \
AGENTD_CODEXW_IDENTITY_DIR="${TMP_DIR}/codexw-native" \
AGENTD_CODEXW_PLIST_PATH="${TMP_DIR}/com.agentd.codexw-connector.plist" \
AGENTD_CODEXW_LOG_DIR="${TMP_DIR}/logs" \
AGENTD_AUTH_TOKEN="agentd-service-smoke-token" \
AGENTD_CODEXW_BROKER_USER="admin" \
AGENTD_CODEXW_BROKER_PASSWORD="secret" \
AGENTD_CODEXW_DRY_RUN=1 \
  "${ROOT}/tools/install_agentd_codexw_connector_launchd.sh" >/dev/null

if ! grep -q -- "--connect" "${TMP_DIR}/com.agentd.codexw-connector.plist"; then
  echo "connector launchd plist missing --connect" >&2
  exit 1
fi
if grep -q -- "--reconnect" "${TMP_DIR}/com.agentd.codexw-connector.plist"; then
  echo "connector launchd plist must not enable in-process reconnect supervision" >&2
  exit 1
fi
if ! grep -q -- "--bootstrap-identity" "${TMP_DIR}/com.agentd.codexw-connector.plist"; then
  echo "connector launchd plist missing bootstrap identity" >&2
  exit 1
fi

grep -q "Restart=always" "${ROOT}/packaging/systemd/agentd.service"
grep -q "Restart=always" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"
grep -q -- "--connect" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"
if grep -q -- "--reconnect" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"; then
  echo "systemd connector unit must not enable in-process reconnect supervision" >&2
  exit 1
fi

echo "agentd_codexw_service_installers_smoke OK"
