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
active_provider = diag.get("active_provider")
if not isinstance(active_provider, str) or not active_provider:
  print("missing/invalid active_provider", active_provider, file=sys.stderr)
  raise SystemExit(1)
key_present = diag.get("active_provider_key_present")
if not isinstance(key_present, bool):
  print("missing/invalid active_provider_key_present", key_present, file=sys.stderr)
  raise SystemExit(1)
base_url = diag.get("active_provider_base_url")
base_url_source = diag.get("active_provider_base_url_source")
if not isinstance(base_url, str) or not base_url:
  print("missing/invalid active_provider_base_url", base_url, file=sys.stderr)
  raise SystemExit(1)
if base_url_source not in ("config", "env", "default"):
  print("missing/invalid active_provider_base_url_source", base_url_source, file=sys.stderr)
  raise SystemExit(1)
if key_present:
  src = diag.get("active_provider_key_source")
  if not isinstance(src, dict):
    print("missing active_provider_key_source", src, file=sys.stderr)
    raise SystemExit(1)
  if src.get("kind") not in ("config", "env", "file"):
    print("invalid active_provider_key_source kind", src, file=sys.stderr)
    raise SystemExit(1)
  if not isinstance(src.get("label"), str) or not src.get("label"):
    print("invalid active_provider_key_source label", src, file=sys.stderr)
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
if active_provider not in prov:
  print("active_provider not in providers list", active_provider, file=sys.stderr)
  raise SystemExit(1)
if not prov[active_provider].get("active"):
  print("active_provider not marked active in providers", active_provider, file=sys.stderr)
  raise SystemExit(1)
prov_active = prov[active_provider]
prov_base = prov_active.get("base_url")
prov_source = prov_active.get("base_url_source")
if isinstance(prov_base, str) and prov_base:
  if prov_base != base_url:
    print("active provider base_url mismatch", prov_base, base_url, file=sys.stderr)
    raise SystemExit(1)
  if prov_source != base_url_source:
    print("active provider base_url_source mismatch", prov_source, base_url_source, file=sys.stderr)
    raise SystemExit(1)
if prov[active_provider].get("key_present") is False:
  warning = prov[active_provider].get("warning")
  if not isinstance(warning, str) or "Active provider key missing" not in warning:
    print("expected active provider missing-key warning", warning, file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_diagnostics_smoke OK"
