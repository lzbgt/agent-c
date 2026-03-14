#!/usr/bin/env bash

devstack_state_field() {
  local state_path="$1"
  local key="$2"
  python3 - "$state_path" "$key" <<'PY'
import json
import sys

state_path = sys.argv[1]
key = sys.argv[2]
try:
    with open(state_path, "r", encoding="utf-8") as fh:
        payload = json.load(fh)
except Exception:
    print("")
    raise SystemExit(0)
value = payload.get(key)
if value is None:
    print("")
elif isinstance(value, bool):
    print("1" if value else "0")
else:
    print(value)
PY
}

devstack_pid_alive() {
  local pid="$1"
  [[ -n "${pid}" && "${pid}" =~ ^[0-9]+$ ]] || return 1
  kill -0 "${pid}" >/dev/null 2>&1
}

devstack_port_listener_pids() {
  local port="$1"
  [[ -n "${port}" && "${port}" =~ ^[0-9]+$ ]] || return 0
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi
  lsof -t -nP -iTCP:"${port}" -sTCP:LISTEN 2>/dev/null | sort -u | tr '\n' ' '
}

devstack_state_service_live() {
  local state_path="$1"
  local pid_key="$2"
  local port_key="$3"
  local pid port listeners listener
  pid="$(devstack_state_field "${state_path}" "${pid_key}")"
  port="$(devstack_state_field "${state_path}" "${port_key}")"
  listeners="$(devstack_port_listener_pids "${port}")"
  if devstack_pid_alive "${pid}"; then
    if [[ -z "${port}" ]]; then
      return 0
    fi
    for listener in ${listeners}; do
      if [[ "${listener}" == "${pid}" ]]; then
        return 0
      fi
    done
  fi
  [[ -n "${listeners}" ]]
}

devstack_state_is_live() {
  local state_path="$1"
  local webui_port
  [[ -f "${state_path}" ]] || return 1
  devstack_state_service_live "${state_path}" "broker_pid" "broker_port" || return 1
  webui_port="$(devstack_state_field "${state_path}" "webui_port")"
  if [[ -n "${webui_port}" && "${webui_port}" != "0" ]]; then
    devstack_state_service_live "${state_path}" "webui_pid" "webui_port" || return 1
  fi
  return 0
}

devstack_state_cleanup_stale() {
  local root="$1"
  local state_path="$2"
  if [[ -x "${root}/tools/devstack_agent_down.sh" ]]; then
    "${root}/tools/devstack_agent_down.sh" --state "${state_path}" >/dev/null 2>&1 || true
  fi
}
