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

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_diagnostics_smoke" \
  --tools none \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

diag_json="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/diagnostics")"
providers_json="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/diagnostics/providers")"

DIAG_JSON="${diag_json}" PROVIDERS_JSON="${providers_json}" python3 - <<'PY'
import json, os, sys

diag = json.loads(os.environ["DIAG_JSON"])
if not diag.get("ok"):
  print("diagnostics not ok:", diag, file=sys.stderr)
  raise SystemExit(1)
if "ready" not in diag:
  print("missing ready field in diagnostics", file=sys.stderr)
  raise SystemExit(1)
if "db" not in diag:
  print("missing db field in diagnostics", file=sys.stderr)
  raise SystemExit(1)

providers = json.loads(os.environ["PROVIDERS_JSON"])
if not providers.get("ok"):
  print("providers not ok:", providers, file=sys.stderr)
  raise SystemExit(1)
prov = providers.get("providers")
if not isinstance(prov, dict):
  print("providers missing or not object", file=sys.stderr)
  raise SystemExit(1)
active_count = 0
for name in ("deepseek", "moonshot", "openrouter", "openai"):
  if name not in prov:
    print("missing provider entry:", name, file=sys.stderr)
    raise SystemExit(1)
  entry = prov[name]
  active = entry.get("active")
  if not isinstance(active, bool):
    print("missing/invalid active flag for", name, ":", active, file=sys.stderr)
    raise SystemExit(1)
  if active:
    active_count += 1
  src = entry.get("base_url_source")
  if src not in ("config", "env", "default"):
    print("missing/invalid base_url_source for", name, ":", src, file=sys.stderr)
    raise SystemExit(1)
if active_count != 1:
  print("expected exactly one active provider, got", active_count, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_diagnostics_smoke OK"
