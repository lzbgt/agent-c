#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/devstack_state.sh
source "${ROOT}/tools/lib/devstack_state.sh"

usage() {
  cat <<'USAGE'
Usage: tools/devstack_status.sh [--state <path>] [--json] [--require-live] [--require-ready]

Reports the current canonical devstack state from out/devstack_state.json and
whether the broker/WebUI referenced there are still alive.
USAGE
}

STATE="${ROOT}/out/devstack_state.json"
JSON=0
REQUIRE_LIVE=0
REQUIRE_READY=0

http_status() {
  local url="$1"
  if [[ -z "${url}" ]]; then
    echo "000"
    return 0
  fi
  env -u HTTPS_PROXY -u https_proxy -u HTTP_PROXY -u http_proxy -u ALL_PROXY -u all_proxy \
    curl -sS -o /dev/null -w '%{http_code}' --max-time 2 "${url}" 2>/dev/null || echo "000"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state)
      STATE="$2"
      shift 2
      ;;
    --json)
      JSON=1
      shift
      ;;
    --require-live)
      REQUIRE_LIVE=1
      shift
      ;;
    --require-ready)
      REQUIRE_READY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -f "${STATE}" ]]; then
  if [[ "${JSON}" -eq 1 ]]; then
    printf '{"status":"missing","state":"%s"}\n' "${STATE}"
  else
    echo "status=missing"
    echo "state=${STATE}"
  fi
  exit 1
fi

status="stale"
if devstack_state_is_live "${STATE}"; then
  status="live"
fi

broker_base="$(devstack_state_field "${STATE}" "broker_base")"
webui_base="$(devstack_state_field "${STATE}" "webui_base")"
agentd_base="$(devstack_state_field "${STATE}" "agentd_base")"
broker_pid="$(devstack_state_field "${STATE}" "broker_pid")"
broker_port="$(devstack_state_field "${STATE}" "broker_port")"
webui_pid="$(devstack_state_field "${STATE}" "webui_pid")"
webui_port="$(devstack_state_field "${STATE}" "webui_port")"
broker_listeners="$(devstack_port_listener_pids "${broker_port}")"
webui_listeners="$(devstack_port_listener_pids "${webui_port}")"
agentd_health_code="000"
broker_health_code="000"
broker_ready_code="000"
if [[ "${status}" == "live" ]]; then
  agentd_health_code="$(http_status "${agentd_base}/api/v1/health")"
  broker_health_code="$(http_status "${broker_base}/healthz")"
  broker_ready_code="$(http_status "${broker_base}/readyz")"
  if [[ "${agentd_health_code}" != "200" || "${broker_health_code}" != "200" || "${broker_ready_code}" != "200" ]]; then
    status="degraded"
  fi
fi

exit_code=0
if [[ "${REQUIRE_LIVE}" -eq 1 && "${status}" == "stale" ]]; then
  exit_code=1
fi
if [[ "${REQUIRE_READY}" -eq 1 && "${status}" != "live" ]]; then
  exit_code=1
fi

if [[ "${JSON}" -eq 1 ]]; then
  python3 - <<PY
import json

def split_pids(raw: str):
    return [int(x) for x in raw.split() if x.strip().isdigit()]

print(json.dumps({
    "status": "${status}",
    "state": "${STATE}",
    "agentd_base": "${agentd_base}",
    "broker_base": "${broker_base}",
    "webui_base": "${webui_base}",
    "broker_pid": int("${broker_pid}") if "${broker_pid}".isdigit() else 0,
    "broker_port": int("${broker_port}") if "${broker_port}".isdigit() else 0,
    "broker_listener_pids": split_pids("${broker_listeners}"),
    "agentd_health_code": int("${agentd_health_code}") if "${agentd_health_code}".isdigit() else 0,
    "broker_health_code": int("${broker_health_code}") if "${broker_health_code}".isdigit() else 0,
    "broker_ready_code": int("${broker_ready_code}") if "${broker_ready_code}".isdigit() else 0,
    "webui_pid": int("${webui_pid}") if "${webui_pid}".isdigit() else 0,
    "webui_port": int("${webui_port}") if "${webui_port}".isdigit() else 0,
    "webui_listener_pids": split_pids("${webui_listeners}"),
}, indent=2))
PY
  exit "${exit_code}"
fi

echo "status=${status}"
echo "state=${STATE}"
echo "agentd_base=${agentd_base}"
echo "broker_base=${broker_base}"
echo "broker_pid=${broker_pid}"
echo "broker_port=${broker_port}"
echo "broker_listener_pids=${broker_listeners}"
echo "agentd_health_code=${agentd_health_code}"
echo "broker_health_code=${broker_health_code}"
echo "broker_ready_code=${broker_ready_code}"
echo "webui_base=${webui_base}"
echo "webui_pid=${webui_pid}"
echo "webui_port=${webui_port}"
echo "webui_listener_pids=${webui_listeners}"
exit "${exit_code}"
