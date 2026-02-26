#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: tools/devstack_agent_ensure.sh [options]

Ensures the devstack is running and ready for testing.

Options:
  --agentd-port    agentd port (default: 54512)
  --broker-port    broker port (default: 54513)
  --webui-port     WebUI port (default: 54514)
  --postgres-port  Postgres published port (default: 54515)
  --keycloak-port  Keycloak published port (default: 54516)
  -h, --help       Show this help
USAGE
}

AGENTD_PORT="54512"
BROKER_PORT="54513"
WEBUI_PORT="54514"
POSTGRES_PORT="54515"
KEYCLOAK_PORT="54516"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --agentd-port) AGENTD_PORT="$2"; shift 2 ;;
    --broker-port) BROKER_PORT="$2"; shift 2 ;;
    --webui-port) WEBUI_PORT="$2"; shift 2 ;;
    --postgres-port) POSTGRES_PORT="$2"; shift 2 ;;
    --keycloak-port) KEYCLOAK_PORT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

AGENTD_BASE="http://127.0.0.1:${AGENTD_PORT}"
BROKER_BASE="https://127.0.0.1:${BROKER_PORT}"
WEBUI_BASE="http://127.0.0.1:${WEBUI_PORT}"
KEYCLOAK_BASE="http://keycloak.lvh.me:${KEYCLOAK_PORT}"

ok_http() {
  local url="$1"
  curl -fsS "$url" >/dev/null 2>&1
}

ok_https_insecure() {
  local url="$1"
  curl -fsSk "$url" >/dev/null 2>&1
}

ready=1
ok_http "${AGENTD_BASE}/api/v1/health" || ready=0
ok_https_insecure "${BROKER_BASE}/readyz" || ready=0
ok_http "${WEBUI_BASE}" || ready=0
ok_http "${KEYCLOAK_BASE}/realms/agentd/.well-known/openid-configuration" || ready=0

if [[ "${ready}" -eq 1 ]]; then
  echo "[devstack] OK (already running)"
  echo "  - WebUI:  ${WEBUI_BASE}"
  echo "  - agentd: ${AGENTD_BASE}"
  echo "  - broker: ${BROKER_BASE}"
  exit 0
fi

echo "[devstack] starting devstack..."
tools/devstack_agent.sh \
  --agentd-tools host \
  --agentd-port "${AGENTD_PORT}" \
  --broker-port "${BROKER_PORT}" \
  --webui-port "${WEBUI_PORT}" \
  --postgres-port "${POSTGRES_PORT}" \
  --keycloak-port "${KEYCLOAK_PORT}" \
  --workflow-http \
  --keep

echo "[devstack] waiting for readiness..."
for _ in $(seq 1 120); do
  ok_http "${AGENTD_BASE}/api/v1/health" || { sleep 1; continue; }
  ok_https_insecure "${BROKER_BASE}/readyz" || { sleep 1; continue; }
  ok_http "${WEBUI_BASE}" || { sleep 1; continue; }
  ok_http "${KEYCLOAK_BASE}/realms/agentd/.well-known/openid-configuration" || { sleep 1; continue; }
  echo "[devstack] OK (ready)"
  echo "  - WebUI:  ${WEBUI_BASE}"
  echo "  - agentd: ${AGENTD_BASE}"
  echo "  - broker: ${BROKER_BASE}"
  exit 0
done

echo "[devstack] ERROR: stack did not become ready" >&2
exit 1
