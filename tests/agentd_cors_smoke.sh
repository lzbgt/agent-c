#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${PROJECT_ROOT}/build"
mkdir -p "${LOG_DIR}"

DAEMON_PID=""

cleanup() {
  if [[ -n "${DAEMON_PID}" ]]; then
    kill -TERM "${DAEMON_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${DAEMON_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${DAEMON_PID}" >/dev/null 2>&1; then
      kill -KILL "${DAEMON_PID}" >/dev/null 2>&1 || true
    fi
    wait "${DAEMON_PID}" >/dev/null 2>&1 || true
    DAEMON_PID=""
  fi
}
trap cleanup EXIT

wait_healthy() {
  local url="$1"
  for _ in $(seq 1 60); do
    if curl -fsS --noproxy "*" --max-time 2 "${url}/api/v1/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

curl_headers() {
  local method="$1"
  local url="$2"
  shift 2
  curl -sS --noproxy "*" --max-time 5 -X "${method}" -D - -o /dev/null "$@" "${url}"
}

assert_cors_headers() {
  local headers="$1"
  local want_origin="$2" # "*" | exact | "__absent__"
  local need_x_openrouter="$3" # "1"|"0"

  python3 - <<PY
import sys
h = r'''${headers}'''.replace('\r','')
lines = [ln for ln in h.splitlines() if ln.strip()]
if not lines:
  raise SystemExit("empty header capture")
status = lines[0]
if "204" not in status and "200" not in status and "401" not in status and "400" not in status:
  print("unexpected status:", status, file=sys.stderr)
  raise SystemExit(1)

def get(name):
  name = name.lower()
  for ln in lines[1:]:
    if ":" not in ln:
      continue
    k, v = ln.split(":", 1)
    if k.strip().lower() == name:
      return v.strip()
  return None

want_origin = "${want_origin}"
got_origin = get("Access-Control-Allow-Origin")
if want_origin == "__absent__":
  if got_origin is not None:
    print("expected allow-origin absent, got:", got_origin, file=sys.stderr)
    raise SystemExit(1)
else:
  if got_origin != want_origin:
    print("unexpected allow-origin:", got_origin, "want:", want_origin, file=sys.stderr)
    raise SystemExit(1)

need_x = "${need_x_openrouter}" == "1"
allow_headers = get("Access-Control-Allow-Headers") or ""
if need_x and "X-OpenRouter-Key" not in allow_headers:
  print("missing X-OpenRouter-Key in allow-headers:", allow_headers, file=sys.stderr)
  raise SystemExit(1)
PY
}

ORIGIN_OK="http://localhost:5173"
ORIGIN_BAD="http://evil.example"

preflight_headers() {
  local base_url="$1"
  local origin="$2"
  curl_headers OPTIONS "${base_url}/api/v1/run" \
    -H "Origin: ${origin}" \
    -H "Access-Control-Request-Method: POST" \
    -H "Access-Control-Request-Headers: Content-Type, X-OpenRouter-Key"
}

health_headers() {
  local base_url="$1"
  local origin="$2"
  curl_headers GET "${base_url}/api/v1/health" \
    -H "Origin: ${origin}"
}

# 1) Loopback defaults: allow any origin (*) and include X-OpenRouter-Key in allow headers.
cleanup
PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="127.0.0.1"
BASE="http://${HOST}:${PORT}"

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  > "${LOG_DIR}/agentd_cors_smoke.loopback.stdout.log" 2> "${LOG_DIR}/agentd_cors_smoke.loopback.stderr.log" &
DAEMON_PID=$!

wait_healthy "${BASE}"

hdr="$(preflight_headers "${BASE}" "${ORIGIN_OK}")"
assert_cors_headers "${hdr}" "*" "1"

# 2) Non-loopback defaults: CORS disabled unless explicitly configured.
cleanup
PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="0.0.0.0"
BASE="http://127.0.0.1:${PORT}"

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --auth-token "t" \
  > "${LOG_DIR}/agentd_cors_smoke.nonloopback_default.stdout.log" 2> "${LOG_DIR}/agentd_cors_smoke.nonloopback_default.stderr.log" &
DAEMON_PID=$!

wait_healthy "${BASE}"

hdr="$(preflight_headers "${BASE}" "${ORIGIN_OK}")"
assert_cors_headers "${hdr}" "__absent__" "0"

# 3) Non-loopback explicit allowlist: reflect allowed origin, deny others.
cleanup
PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="0.0.0.0"
BASE="http://127.0.0.1:${PORT}"

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --auth-token "t" \
  --cors-origin "${ORIGIN_OK}" \
  > "${LOG_DIR}/agentd_cors_smoke.nonloopback_allowlist.stdout.log" 2> "${LOG_DIR}/agentd_cors_smoke.nonloopback_allowlist.stderr.log" &
DAEMON_PID=$!

wait_healthy "${BASE}"

hdr="$(preflight_headers "${BASE}" "${ORIGIN_OK}")"
assert_cors_headers "${hdr}" "${ORIGIN_OK}" "1"

hdr2="$(health_headers "${BASE}" "${ORIGIN_OK}")"
assert_cors_headers "${hdr2}" "${ORIGIN_OK}" "0"

hdr3="$(preflight_headers "${BASE}" "${ORIGIN_BAD}")"
assert_cors_headers "${hdr3}" "__absent__" "0"

