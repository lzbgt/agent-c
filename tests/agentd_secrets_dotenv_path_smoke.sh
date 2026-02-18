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
TOKEN="test_token_123"

TMP_DIR="$(mktemp -d)"
ENV_FILE="${TMP_DIR}/agentd.env"

echo "KIMI_API_KEY_CN=sk-testkey123" > "${ENV_FILE}"

auto_cleanup() {
  agentd_smoke_stop || true
  rm -rf "${TMP_DIR}" || true
}
trap auto_cleanup EXIT

unset KIMI_API_KEY_CN MOONSHOT_API_KEY MOONSHOT_API_KEY_CN
export AGENTD_DOTENV_PATH="${ENV_FILE}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_secrets_dotenv_path_smoke" \
  --tools none \
  --auth-token "${TOKEN}"

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 5 -H "Authorization: Bearer ${TOKEN}" \
  "${DAEMON_URL}/api/v1/diagnostics/providers")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
providers = obj.get("providers") or {}
moonshot = providers.get("moonshot") or {}
if moonshot.get("key_present") is not True:
  print("expected moonshot.key_present true", moonshot, file=sys.stderr)
  raise SystemExit(1)
source = moonshot.get("source") or {}
if source.get("kind") != "file":
  print("expected key source kind file", source, file=sys.stderr)
  raise SystemExit(1)
if source.get("label") != "AGENTD_DOTENV_PATH":
  print("expected key source label AGENTD_DOTENV_PATH", source, file=sys.stderr)
  raise SystemExit(1)
PY
