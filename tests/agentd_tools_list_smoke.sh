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

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_tools_list_smoke" \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") or []
names = {d.get("name") for d in defs if isinstance(d, dict)}
need = {"shell_exec", "proc_exec", "file_apply_patch", "fs_read", "fs_list", "fs_stat", "camera_capture"}
missing = sorted(need - names)
if missing:
  print("missing tools:", missing, file=sys.stderr)
  raise SystemExit(1)
PY

# scoped (yolo=0) should omit exec tools but keep patch + fs inspection.
# camera_capture remains available and should default to backend=mock when exec is disabled.
resp_scoped="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_scoped}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_yolo") not in (False, 0):
  print("unexpected effective_yolo:", obj.get("effective_yolo"), file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") or []
names = {d.get("name") for d in defs if isinstance(d, dict)}
must_absent = {"shell_exec", "proc_exec"}
present = sorted(must_absent & names)
if present:
  print("exec tools should be omitted in scoped mode:", present, file=sys.stderr)
  raise SystemExit(1)
must_present = {"file_apply_patch", "fs_read", "fs_list", "fs_stat", "fs_find", "text_search", "camera_capture"}
missing = sorted(must_present - names)
if missing:
  print("missing scoped tools:", missing, file=sys.stderr)
  raise SystemExit(1)
PY

# readonly should omit mutating / exec tools even when daemon default is full.
resp_ro="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=1&host_policy=readonly")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_ro}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_host_policy") not in ("readonly",):
  print("unexpected effective_host_policy:", obj.get("effective_host_policy"), file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") or []
names = {d.get("name") for d in defs if isinstance(d, dict)}
must_absent = {"shell_exec", "proc_exec", "file_apply_patch", "camera_capture"}
present = sorted(must_absent & names)
if present:
  print("tools should be omitted in readonly policy:", present, file=sys.stderr)
  raise SystemExit(1)
must_present = {"fs_read", "fs_list", "fs_stat", "fs_find", "text_search"}
missing = sorted(must_present - names)
if missing:
  print("missing readonly tools:", missing, file=sys.stderr)
  raise SystemExit(1)
PY
