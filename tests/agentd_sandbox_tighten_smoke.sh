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

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/agentd_sandbox_XXXXXX")"
OUTSIDE_FILE="${TMP_ROOT}/outside.txt"

cleanup() {
  agentd_smoke_stop
  rm -rf "${TMP_ROOT}" || true
}
trap cleanup EXIT

echo "outside" > "${OUTSIDE_FILE}"

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_sandbox_tighten_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

# 1) A client cannot loosen to yolo=1 via query params when daemon is --no-yolo.
resp="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_yolo") is not False:
  print("expected effective_yolo=false, got:", obj.get("effective_yolo"), file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") if isinstance(obj.get("defs"), list) else []
names = set()
for d in defs:
  if isinstance(d, dict) and isinstance(d.get("name"), str):
    names.add(d["name"])
if "shell_exec" in names or "proc_exec" in names:
  print("expected exec tools to be absent when effective_yolo=false; got tools:", sorted(names), file=sys.stderr)
  raise SystemExit(1)
PY

# 2) File endpoint: absolute paths should always work (no tools_root/host_scope sandboxing).
body="$(curl -fsS --noproxy "*" --max-time 5 \
  "${DAEMON_URL}/api/v1/file?path=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${OUTSIDE_FILE}")")"
if [[ "${body}" != "outside" ]]; then
  echo "unexpected file body: ${body}" >&2
  exit 1
fi

echo "agentd_sandbox_tighten_smoke OK"
