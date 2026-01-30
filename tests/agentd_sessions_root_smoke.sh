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

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

TMP_ROOT="$(python3 - <<'PY'
import tempfile
print(tempfile.mkdtemp(prefix="agentd_sessions_root_smoke_"))
PY
)"

cleanup() {
  agentd_smoke_stop
  rm -rf "${TMP_ROOT}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_sessions_root_smoke" \
  --tools host \
  --no-yolo \
  --sessions-root "${TMP_ROOT}"

agentd_smoke_wait_health "${DAEMON_URL}"

sid="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{}" \
  "${DAEMON_URL}/api/v1/session/new" | python3 -c 'import json,sys; obj=json.load(sys.stdin); assert obj.get("ok") and obj.get("session_id"); print(obj["session_id"])'
)"

python3 - <<PY
from pathlib import Path
sid = "${sid}"
root = Path("${TMP_ROOT}")
sess = root / f"{sid}.sess"
if not sess.exists():
  raise SystemExit(f"missing session file: {sess}")
PY

# /api/v1/sessions should list it (best-effort; ordering not guaranteed).
curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/sessions" \
  | python3 -c 'import json,sys; obj=json.load(sys.stdin); sid=sys.argv[1]; assert obj.get("ok"); assert sid in (obj.get("sessions") or [])' "${sid}"
