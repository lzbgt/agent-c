#!/usr/bin/env bash
set -euo pipefail

# Prod-like local verification harness for the docker-compose stack.
#
# What it verifies (minimal but end-to-end):
# - Postgres + Keycloak (OIDC) are reachable
# - Broker starts with Postgres + OIDC config
# - Agentd starts and serves /api/v1/health (auth enabled)
# - Connector can connect via broker mTLS and proxy to agentd
#
# Notes:
# - Uses Keycloak password grant for local dev convenience.
# - Uses curl -k for broker HTTPS because we generate a local self-signed CA.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT}/out"
mkdir -p "${OUT_DIR}"

if ! command -v docker >/dev/null 2>&1; then
  echo "[compose] SKIP: docker not found (install Docker Desktop or Colima)" >&2
  exit 77
fi

if ! docker info >/dev/null 2>&1; then
  echo "[compose] SKIP: docker daemon not running" >&2
  echo "[compose] Hint: start Docker Desktop or Colima, then re-run." >&2
  exit 77
fi
if ! docker compose version >/dev/null 2>&1; then
  echo "[compose] SKIP: docker compose not available" >&2
  echo "[compose] Hint: install the docker compose plugin or upgrade Docker Desktop." >&2
  exit 77
fi

LOG_BUILD="${OUT_DIR}/compose_build_$(date +%Y-%m-%d_%H%M%S).log"
LOG_UP="${OUT_DIR}/compose_up_$(date +%Y-%m-%d_%H%M%S).log"
LOG_PULL="${OUT_DIR}/compose_pull_$(date +%Y-%m-%d_%H%M%S).log"

MTLS_DIR="${ROOT}/tools/_compose_mtls"

is_port_free() {
  local port="$1"
  # We consider a port "free" only if it is unused on BOTH IPv4 loopback and IPv6 loopback.
  # This avoids false-positives where one service listens on 127.0.0.1:<port> (IPv4) while
  # docker binds *:port (IPv6), which would still conflict for users hitting 127.0.0.1.
  python3 - "${port}" <<'PY'
import socket, sys
port = int(sys.argv[1])

def loopback_is_free(host: str, family: int) -> bool:
  try:
    s = socket.socket(family, socket.SOCK_STREAM)
  except Exception:
    # If the platform doesn't support this family (e.g. IPv6 disabled), ignore it.
    return True
  try:
    s.settimeout(0.15)
    rc = s.connect_ex((host, port))
    # rc==0 means a listener accepted the TCP connect -> NOT free.
    return rc != 0
  finally:
    try:
      s.close()
    except Exception:
      pass

ok = loopback_is_free("127.0.0.1", socket.AF_INET) and loopback_is_free("::1", socket.AF_INET6)
print("yes" if ok else "no")
PY
}

pick_port() {
  local preferred="$1"
  local label="${2:-port}"
  local p
  for p in "${preferred}" "$((preferred + 1))" "$((preferred + 2))" "$((preferred + 10))" "$((preferred + 100))"; do
    if [[ "$(is_port_free "${p}")" == "yes" ]]; then
      echo "${p}"
      return 0
    fi
  done
  echo "[compose] ERROR: no free ${label} found near ${preferred}" >&2
  return 1
}

export BROKER_PUBLISHED_PORT="${BROKER_PUBLISHED_PORT:-$(pick_port 8443 broker)}"
export KEYCLOAK_PUBLISHED_PORT="${KEYCLOAK_PUBLISHED_PORT:-$(pick_port 8081 keycloak)}"
export POSTGRES_PUBLISHED_PORT="${POSTGRES_PUBLISHED_PORT:-$(pick_port 5433 postgres)}"
export AGENTD_PUBLISHED_PORT="${AGENTD_PUBLISHED_PORT:-$(pick_port 8123 agentd)}"
export WEBUI_PUBLISHED_PORT="${WEBUI_PUBLISHED_PORT:-$(pick_port 8100 webui)}"

# Allow multiple stacks concurrently by default by making the compose project name stable-per-port.
# Users can still override explicitly via COMPOSE_PROJECT_NAME.
export COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-agent_${WEBUI_PUBLISHED_PORT}}"

export BROKER_IMAGE="${BROKER_IMAGE:-agentd-broker-local}"
export AGENTD_IMAGE="${AGENTD_IMAGE:-agentd-daemon-local}"
export CONNECTOR_IMAGE="${CONNECTOR_IMAGE:-agentd-connector-local}"
export WEBUI_IMAGE="${WEBUI_IMAGE:-agentd-webui-local}"

BROKER_BASE="https://127.0.0.1:${BROKER_PUBLISHED_PORT}"
# Use a stable dev hostname so the token issuer matches what the broker validates.
# keycloak.lvh.me resolves to 127.0.0.1 on the host; inside containers we map it to host-gateway.
KEYCLOAK_BASE="http://keycloak.lvh.me:${KEYCLOAK_PUBLISHED_PORT}"
AGENTD_BASE="http://127.0.0.1:${AGENTD_PUBLISHED_PORT}"
WEBUI_BASE="http://127.0.0.1:${WEBUI_PUBLISHED_PORT}"

if [[ "${COMPOSE_BUILD:-1}" == "0" ]]; then
  missing=()
  missing_svcs=()
  image_for() {
    case "$1" in
      agentd) echo "${AGENTD_IMAGE}" ;;
      broker) echo "${BROKER_IMAGE}" ;;
      connector) echo "${CONNECTOR_IMAGE}" ;;
      webui) echo "${WEBUI_IMAGE}" ;;
      *) echo "" ;;
    esac
  }
  for svc in agentd broker connector webui; do
    img="$(image_for "${svc}")"
    if [[ -z "${img}" ]]; then
      continue
    fi
    if ! docker image inspect "${img}" >/dev/null 2>&1; then
      missing+=("${img}")
      missing_svcs+=("${svc}")
    fi
  done
  if (( ${#missing[@]} > 0 )); then
    if [[ "${COMPOSE_PULL:-0}" == "1" ]]; then
      echo "[compose] pulling missing images (logs: ${LOG_PULL})"
      : >"${LOG_PULL}"
      if (cd "${ROOT}" && docker compose pull "${missing_svcs[@]}") >>"${LOG_PULL}" 2>&1; then
        missing=()
        for svc in agentd broker connector webui; do
          img="$(image_for "${svc}")"
          if [[ -n "${img}" ]] && ! docker image inspect "${img}" >/dev/null 2>&1; then
            missing+=("${img}")
          fi
        done
      else
        echo "[compose] SKIP: docker pull failed; see ${LOG_PULL}" >&2
        exit 77
      fi
    fi
    if (( ${#missing[@]} > 0 )); then
      echo "[compose] SKIP: COMPOSE_BUILD=0 but missing images: ${missing[*]}" >&2
      exit 77
    fi
  fi
fi

(
  cd "${ROOT}"
  # Host-generated mTLS certs (keeps compose deterministic; avoids "apk add" in init containers).
  rm -rf "${MTLS_DIR}"
  mkdir -p "${MTLS_DIR}"
  bash tools/gen_agentd_broker_mtls_test_certs.sh "${MTLS_DIR}" agent1 >/dev/null

  if [[ "${COMPOSE_CLEAN:-1}" == "1" ]]; then
    docker compose down -v --remove-orphans >/dev/null 2>&1 || true
  fi
) >/dev/null 2>&1

if [[ "${COMPOSE_BUILD:-1}" == "1" ]]; then
  echo "[compose] building images (logs: ${LOG_BUILD})"
  compose_build_cmd() {
    if [[ "${COMPOSE_BUILD_SERIAL:-1}" == "1" ]]; then
      for svc in agentd broker connector webui; do
        echo "[compose] build ${svc}"
        env "${build_env[@]}" docker compose build "${svc}"
      done
    else
      env "${build_env[@]}" docker compose build
    fi
  }

  : >"${LOG_BUILD}"
  max_attempts="${COMPOSE_BUILD_RETRIES:-3}"
  attempt=1
  build_env=()
  used_legacy_builder=0
  used_pigz_throttle=0
  saw_resource_error=0
  while true; do
    attempt_log="${LOG_BUILD}.attempt_${attempt}"
    echo "[compose] build attempt ${attempt}/${max_attempts}" >"${attempt_log}"
    if (cd "${ROOT}" && compose_build_cmd) >>"${attempt_log}" 2>&1; then
      cat "${attempt_log}" >>"${LOG_BUILD}"
      rm -f "${attempt_log}"
      break
    fi
    cat "${attempt_log}" >>"${LOG_BUILD}"
    if grep -Eqi "resource temporarily unavailable|unpigz|runc run failed" "${attempt_log}"; then
      saw_resource_error=1
    fi
    if [[ "${saw_resource_error}" == "1" && "${attempt}" -lt "${max_attempts}" ]]; then
      if [[ "${used_legacy_builder}" == "0" ]]; then
        # Retry once with legacy builder settings to reduce process pressure.
        build_env=("DOCKER_BUILDKIT=0" "COMPOSE_DOCKER_CLI_BUILD=0")
        used_legacy_builder=1
      fi
      if [[ "${used_pigz_throttle}" == "0" ]]; then
        # Limit pigz threads to reduce decompression resource spikes.
        build_env+=("PIGZ=-p1" "GZIP=-p1")
        used_pigz_throttle=1
      fi
      rm -f "${attempt_log}"
      sleep 5
      attempt="$((attempt + 1))"
      continue
    fi
    rm -f "${attempt_log}"
    if [[ "${saw_resource_error}" == "1" ]]; then
      echo "[compose] SKIP: docker build failed due to resource limits (unpigz/runc). Try restarting Docker Desktop or increasing CPU/RAM. You can also try PIGZ=-p1 GZIP=-p1." >&2
      exit 77
    fi
    exit 1
  done
fi

echo "[compose] bringing stack up (logs: ${LOG_UP})"
up_args=(-d)
if [[ "${COMPOSE_BUILD:-1}" == "0" ]]; then
  up_args+=(--no-build)
fi
set +e
(cd "${ROOT}" && docker compose up "${up_args[@]}") >"${LOG_UP}" 2>&1
up_rc=$?
set -e
if [[ "${up_rc}" -ne 0 ]]; then
  if grep -Eqi "resource temporarily unavailable|iptables|runc run failed" "${LOG_UP}"; then
    echo "[compose] SKIP: docker up failed due to resource limits (iptables/runc). Try restarting Docker Desktop or increasing CPU/RAM." >&2
    exit 77
  fi
  exit "${up_rc}"
fi

wait_http_ok() {
  local url="$1"
  local timeout_s="${2:-120}"
  local started
  started="$(date +%s)"
  while true; do
    if curl -fsS "${url}" >/dev/null 2>&1; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if (( now - started > timeout_s )); then
      echo "[compose] ERROR: timeout waiting for ${url}" >&2
      return 1
    fi
    sleep 1
  done
}

echo "[compose] waiting for keycloak OIDC discovery..."
wait_http_ok "${KEYCLOAK_BASE}/realms/agentd/.well-known/openid-configuration" 240

echo "[compose] waiting for agentd health..."
wait_http_ok "${AGENTD_BASE}/api/v1/health" 240 || true

get_token() {
  local token_json
  token_json="$(
    curl -fsS \
      -d "grant_type=password" \
      -d "client_id=agentd-broker-dev" \
      -d "username=test" \
      -d "password=test" \
      "${KEYCLOAK_BASE}/realms/agentd/protocol/openid-connect/token"
  )"
  echo "${token_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("access_token",""))'
}

echo "[compose] acquiring OIDC token (dev password grant)..."
OIDC_JWT="$(get_token)"
if [[ -z "${OIDC_JWT}" ]]; then
  echo "[compose] ERROR: failed to fetch OIDC token" >&2
  exit 2
fi

echo "[compose] creating broker agent record agent_id=agent1 (wait/retry)..."
started="$(date +%s)"
while true; do
  if curl -fsS -k \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"agent_id":"agent1"}' \
    "${BROKER_BASE}/v1/agents" >/dev/null 2>&1; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[compose] ERROR: broker did not accept create agent in time" >&2
    exit 4
  fi
  sleep 1
done

echo "[compose] waiting for connector to connect (agent1 connected=true)..."
started="$(date +%s)"
while true; do
  j="$(curl -fsS -k -H "Authorization: Bearer ${OIDC_JWT}" "${BROKER_BASE}/v1/agents" || true)"
  ok="$(python3 - "${j}" <<'PY'
import json,sys
raw = sys.argv[1] if len(sys.argv) > 1 else ""
try:
  obj = json.loads(raw or "{}")
  for a in (obj.get("agents") or []):
    if a.get("agent_id") == "agent1" and a.get("connected") is True:
      print("yes")
      raise SystemExit(0)
except Exception:
  pass
print("no")
PY
)"
  if [[ "${ok}" == "yes" ]]; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[compose] ERROR: connector did not become connected in time" >&2
    echo "[compose] broker /v1/agents response:" >&2
    echo "${j}" >&2
    exit 3
  fi
  sleep 1
done

echo "[compose] verifying broker proxy to agentd /api/v1/health..."
curl -fsS -k -H "Authorization: Bearer ${OIDC_JWT}" \
  "${BROKER_BASE}/v1/agents/agent1/proxy/api/v1/health" | python3 -m json.tool >/dev/null

echo "[compose] verifying direct agentd health (auth enabled)..."
curl -fsS -H "Authorization: Bearer dev-agentd-token" \
  "${AGENTD_BASE}/api/v1/health" | python3 -m json.tool >/dev/null

echo "[compose] verifying webui is served..."
curl -fsS "${WEBUI_BASE}/" >/dev/null

echo "[compose] OK"
echo "  - WebUI:    ${WEBUI_BASE} (set Daemon auth token: dev-agentd-token)"
echo "  - agentd:   ${AGENTD_BASE}"
echo "  - Keycloak: ${KEYCLOAK_BASE} (realm=agentd user=test pass=test)"
echo "  - Broker:   ${BROKER_BASE} (OIDC aud=agentd-broker-dev agent_id=agent1)"
