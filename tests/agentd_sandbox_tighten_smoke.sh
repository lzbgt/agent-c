#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

SCOPE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/agentd_scope_XXXXXX")"
OUTSIDE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/agentd_outside_XXXXXX")"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${AGENTD_PID:-}" ]]; then
    :
  fi
  rm -rf "${SCOPE_ROOT}" "${OUTSIDE_ROOT}" || true
}
trap cleanup EXIT

echo "inside" > "${SCOPE_ROOT}/inside.txt"
echo "outside" > "${OUTSIDE_ROOT}/outside.txt"

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_sandbox_tighten_smoke" \
  --tools host \
  --no-yolo \
  --host-scope "${SCOPE_ROOT}" \
  --tools-root "@host"

agentd_smoke_wait_health "${DAEMON_URL}"

# 1) A client cannot loosen to yolo=1 via query params when daemon is --no-yolo.
resp="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=1&tools_root=@cwd")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_yolo") is not False:
  print("expected effective_yolo=false, got:", obj.get("effective_yolo"), file=sys.stderr)
  raise SystemExit(1)
want = os.path.realpath(r'''${SCOPE_ROOT}''')
got = os.path.realpath(str(obj.get("effective_tools_root") or ""))
if got != want:
  print("expected effective_tools_root", want, "got", got, file=sys.stderr)
  raise SystemExit(1)
PY

# 2) File endpoint: absolute paths should be blocked even if client sets yolo=1.
abs_outside="${OUTSIDE_ROOT}/outside.txt"
code="$(curl -sS --noproxy "*" --max-time 5 -o "${LOG_DIR}/agentd_sandbox_tighten_smoke.file_outside.body" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/file?path=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${abs_outside}")&yolo=1")"
if [[ "${code}" == "200" ]]; then
  echo "expected /api/v1/file outside scope to be blocked (non-200), got 200" >&2
  exit 1
fi

# 3) Relative paths within scope should still work (even if client requests yolo=1; daemon tightens).
body="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/file?path=inside.txt&yolo=1")"
if [[ "${body}" != "inside" ]]; then
  echo "unexpected file body: ${body}" >&2
  exit 1
fi
