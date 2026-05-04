#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
AGENTD_BIN="${1:-${ROOT}/build/agentd}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agentd-service-installers.XXXXXX")"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

bash -n \
  "${ROOT}/tools/install_agentd_launchd.sh" \
  "${ROOT}/tools/uninstall_agentd_launchd.sh" \
  "${ROOT}/tools/install_agentd_codexw_connector_launchd.sh" \
  "${ROOT}/tools/uninstall_agentd_codexw_connector_launchd.sh" \
  "${ROOT}/tools/run_agentd_codexw_compat.sh"

python3 -m py_compile "${ROOT}/tools/verify_agentd_codexw_connector_service.py"

FAKE_CONNECTOR="${TMP_DIR}/fake_connector.py"
cat >"${FAKE_CONNECTOR}" <<'PY'
#!/usr/bin/env python3
import json
import sys
from pathlib import Path

if "--self-test" not in sys.argv:
    raise SystemExit("fake connector expected --self-test")
payload = {
    "ok": True,
    "checks": [
        {"name": "broker_runtime_instance_visible", "ok": True, "online": True},
        {"name": "broker_runtime_update_preflight", "ok": "--require-update-preflight" in sys.argv},
    ],
}
if "--self-test-output-path" in sys.argv:
    path = Path(sys.argv[sys.argv.index("--self-test-output-path") + 1])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, sort_keys=True) + "\n")
print(json.dumps(payload))
PY
chmod +x "${FAKE_CONNECTOR}"

AGENTD_BIN="${AGENTD_BIN}" \
AGENTD_AUTH_TOKEN="agentd-service-smoke-token" \
AGENTD_STATE_DIR="${TMP_DIR}/agentd-state" \
AGENTD_DB_PATH="${TMP_DIR}/agentd-state/agentd.db" \
AGENTD_PLIST_PATH="${TMP_DIR}/com.agentd.daemon.plist" \
AGENTD_LOG_DIR="${TMP_DIR}/logs" \
AGENTD_DRY_RUN=1 \
  "${ROOT}/tools/install_agentd_launchd.sh" >/dev/null

if grep -q -- "--host-scope\\|--tools-root" "${TMP_DIR}/com.agentd.daemon.plist"; then
  echo "agentd launchd plist contains stale host-scope/tools-root args" >&2
  exit 1
fi
if ! grep -q -- "--state-dir" "${TMP_DIR}/com.agentd.daemon.plist"; then
  echo "agentd launchd plist missing state-dir" >&2
  exit 1
fi
python3 - <<PY
import plistlib
from pathlib import Path
plist = plistlib.loads(Path("${TMP_DIR}/com.agentd.daemon.plist").read_bytes())
args = plist.get("ProgramArguments") or []
env = plist.get("EnvironmentVariables") or {}
if "agentd-service-smoke-token" in args or "--auth-token" in args:
    raise SystemExit("agentd launchd plist leaked auth token into ProgramArguments")
if env.get("AGENTD_AUTH_TOKEN") != "agentd-service-smoke-token":
    raise SystemExit("agentd launchd plist missing AGENTD_AUTH_TOKEN environment")
PY

AGENTD_CODEXW_BROKER_URL="http://127.0.0.1:8787" \
AGENTD_CODEXW_DEPLOYMENT_ID="agentd-service-smoke" \
AGENTD_CODEXW_DISPLAY_NAME="agentd service smoke" \
AGENTD_CODEXW_IDENTITY_DIR="${TMP_DIR}/codexw-native" \
AGENTD_CODEXW_CONNECTOR_BIN="${FAKE_CONNECTOR}" \
AGENTD_CODEXW_PLIST_PATH="${TMP_DIR}/com.agentd.codexw-connector.plist" \
AGENTD_CODEXW_SELF_TEST_PLIST_PATH="${TMP_DIR}/com.agentd.codexw-connector.self-test.plist" \
AGENTD_CODEXW_LOG_DIR="${TMP_DIR}/logs" \
AGENTD_AUTH_TOKEN="agentd-service-smoke-token" \
AGENTD_CODEXW_BROKER_USER="admin" \
AGENTD_CODEXW_BROKER_PASSWORD="secret" \
AGENTD_CODEXW_BROKER_TOKEN="read-token" \
AGENTD_CODEXW_RUNTIME_UPDATE_MODE="agentd_ota" \
AGENTD_CODEXW_RUNTIME_RESTART_MODE="agentd_ota" \
AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT=1 \
AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH="${TMP_DIR}/codexw-native/self-test-status.json" \
AGENTD_CODEXW_SELF_TEST_INTERVAL_SECONDS=123 \
AGENTD_CODEXW_DRY_RUN=1 \
  "${ROOT}/tools/install_agentd_codexw_connector_launchd.sh" >/dev/null

if ! grep -q -- "--connect" "${TMP_DIR}/com.agentd.codexw-connector.plist"; then
  echo "connector launchd plist missing --connect" >&2
  exit 1
fi
if grep -q -- "--reconnect" "${TMP_DIR}/com.agentd.codexw-connector.plist"; then
  echo "connector launchd plist must not enable in-process reconnect supervision" >&2
  exit 1
fi
if ! grep -q -- "--bootstrap-identity" "${TMP_DIR}/com.agentd.codexw-connector.plist"; then
  echo "connector launchd plist missing bootstrap identity" >&2
  exit 1
fi
if ! grep -q -- "--self-test" "${TMP_DIR}/com.agentd.codexw-connector.self-test.plist"; then
  echo "connector launchd self-test plist missing --self-test" >&2
  exit 1
fi
if ! grep -q -- "--require-broker-visible" "${TMP_DIR}/com.agentd.codexw-connector.self-test.plist"; then
  echo "connector launchd self-test plist missing broker visibility check" >&2
  exit 1
fi
if ! grep -q -- "--require-update-preflight" "${TMP_DIR}/com.agentd.codexw-connector.self-test.plist"; then
  echo "connector launchd self-test plist missing update preflight check" >&2
  exit 1
fi
if ! grep -q -- "--self-test-output-path" "${TMP_DIR}/com.agentd.codexw-connector.self-test.plist"; then
  echo "connector launchd self-test plist missing durable status output path" >&2
  exit 1
fi
python3 - <<PY
import plistlib
from pathlib import Path
plist = plistlib.loads(Path("${TMP_DIR}/com.agentd.codexw-connector.plist").read_bytes())
self_test = plistlib.loads(Path("${TMP_DIR}/com.agentd.codexw-connector.self-test.plist").read_bytes())
args = plist.get("ProgramArguments") or []
self_test_args = self_test.get("ProgramArguments") or []
env = plist.get("EnvironmentVariables") or {}
for secret in ("agentd-service-smoke-token", "admin", "secret", "read-token"):
    if secret in args:
        raise SystemExit(f"connector secret leaked into launchd ProgramArguments: {secret}")
    if secret in self_test_args:
        raise SystemExit(f"connector self-test secret leaked into launchd ProgramArguments: {secret}")
if env.get("AGENTD_AUTH_TOKEN") != "agentd-service-smoke-token":
    raise SystemExit("launchd plist missing AGENTD_AUTH_TOKEN environment")
if env.get("AGENTD_CODEXW_BROKER_USER") != "admin":
    raise SystemExit("launchd plist missing broker user environment")
if env.get("AGENTD_CODEXW_BROKER_PASSWORD") != "secret":
    raise SystemExit("launchd plist missing broker password environment")
if env.get("AGENTD_CODEXW_BROKER_TOKEN") != "read-token":
    raise SystemExit("launchd plist missing broker token environment")
if env.get("AGENTD_CODEXW_RUNTIME_UPDATE_MODE") != "agentd_ota":
    raise SystemExit("launchd plist missing runtime update mode environment")
if env.get("AGENTD_CODEXW_RUNTIME_RESTART_MODE") != "agentd_ota":
    raise SystemExit("launchd plist missing runtime restart mode environment")
if env.get("AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT") != "1":
    raise SystemExit("launchd plist missing update preflight environment")
if env.get("AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH") != "${TMP_DIR}/codexw-native/self-test-status.json":
    raise SystemExit("launchd plist missing self-test output path environment")
if env.get("AGENTD_CODEXW_SELF_TEST_STALE_AFTER_SECONDS") != "900":
    raise SystemExit("launchd plist missing self-test stale threshold environment")
for runtime_args in (args, self_test_args):
    try:
        runtime_arg_index = runtime_args.index("--runtime-instance-id")
    except ValueError:
        raise SystemExit("launchd plist missing stable --runtime-instance-id")
    if runtime_args[runtime_arg_index + 1] != "agentd-service-smoke-runtime":
        raise SystemExit(f"unexpected default runtime instance id: {runtime_args[runtime_arg_index + 1]}")
if self_test.get("KeepAlive"):
    raise SystemExit("launchd self-test plist must not be KeepAlive")
if self_test.get("StartInterval") != 123:
    raise SystemExit("launchd self-test plist missing StartInterval")
if "--connect" in self_test_args or "--bootstrap-identity" in self_test_args:
    raise SystemExit("launchd self-test plist must be read-only")
PY

python3 "${ROOT}/tools/verify_agentd_codexw_connector_service.py" \
  --platform launchd \
  --launchd-plist-path "${TMP_DIR}/com.agentd.codexw-connector.plist" \
  --launchd-self-test-plist-path "${TMP_DIR}/com.agentd.codexw-connector.self-test.plist" \
  --skip-supervisor \
  --json >"${TMP_DIR}/launchd-service-report.json"
python3 - <<PY
import json
from pathlib import Path
payload = json.loads(Path("${TMP_DIR}/launchd-service-report.json").read_text())
if not payload.get("ok"):
    raise SystemExit(f"launchd verifier failed: {payload}")
if payload["configuration"]["environment"].get("AGENTD_CODEXW_BROKER_TOKEN") != "<redacted>":
    raise SystemExit("launchd verifier did not redact broker token")
checks = {check["name"]: check for check in payload["self_test"]["payload"]["checks"]}
if not checks.get("broker_runtime_update_preflight", {}).get("ok"):
    raise SystemExit("launchd verifier did not run update preflight self-test")
last_status = payload.get("last_self_test_status", {})
if not last_status.get("exists") or not last_status.get("payload", {}).get("ok"):
    raise SystemExit(f"launchd verifier did not read durable self-test status: {payload}")
PY

SYSTEMD_ENV="${TMP_DIR}/codexw-connector.env"
cat >"${SYSTEMD_ENV}" <<EOF
AGENTD_BASE_URL=http://127.0.0.1:8123
AGENTD_AUTH_TOKEN=agentd-service-smoke-token
AGENTD_CODEXW_BROKER_URL=http://127.0.0.1:8787
AGENTD_CODEXW_DEPLOYMENT_ID=agentd-service-smoke
AGENTD_CODEXW_DISPLAY_NAME=agentd service smoke
AGENTD_CODEXW_IDENTITY_DIR=${TMP_DIR}/codexw-native
AGENTD_CODEXW_BROKER_TOKEN=read-token
AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota
AGENTD_CODEXW_RUNTIME_RESTART_MODE=agentd_ota
AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT=1
AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH=${TMP_DIR}/systemd-self-test-status.json
EOF
python3 "${ROOT}/tools/verify_agentd_codexw_connector_service.py" \
  --platform systemd \
  --systemd-env-file "${SYSTEMD_ENV}" \
  --connector-bin "${FAKE_CONNECTOR}" \
  --skip-supervisor \
  --json >"${TMP_DIR}/systemd-service-report.json"
python3 - <<PY
import json
from pathlib import Path
payload = json.loads(Path("${TMP_DIR}/systemd-service-report.json").read_text())
if not payload.get("ok"):
    raise SystemExit(f"systemd verifier failed: {payload}")
if payload["configuration"]["environment"].get("AGENTD_CODEXW_BROKER_TOKEN") != "<redacted>":
    raise SystemExit("systemd verifier did not redact broker token")
checks = {check["name"]: check for check in payload["self_test"]["payload"]["checks"]}
if not checks.get("broker_runtime_update_preflight", {}).get("ok"):
    raise SystemExit("systemd verifier did not run update preflight self-test")
last_status = payload.get("last_self_test_status", {})
if not last_status.get("exists") or not last_status.get("payload", {}).get("ok"):
    raise SystemExit(f"systemd verifier did not read durable self-test status: {payload}")
PY

grep -q "Restart=always" "${ROOT}/packaging/systemd/agentd.service"
grep -q "Restart=always" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"
grep -q -- "--connect" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"
grep -q -- "--self-test" "${ROOT}/packaging/systemd/agentd-codexw-connector-self-test.service"
grep -q -- "--require-broker-visible" "${ROOT}/packaging/systemd/agentd-codexw-connector-self-test.service"
grep -q -- "--self-test-output-path" "${ROOT}/packaging/systemd/agentd-codexw-connector-self-test.service"
grep -q "OnUnitActiveSec=5min" "${ROOT}/packaging/systemd/agentd-codexw-connector-self-test.timer"
grep -q "AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT=0" "${ROOT}/packaging/systemd/codexw-connector.env.example"
grep -q "AGENTD_CODEXW_RUNTIME_RESTART_MODE=disabled" "${ROOT}/packaging/systemd/codexw-connector.env.example"
grep -q "AGENTD_CODEXW_SELF_TEST_OUTPUT_PATH=" "${ROOT}/packaging/systemd/codexw-connector.env.example"
grep -q "AGENTD_CODEXW_SELF_TEST_STALE_AFTER_SECONDS=900" "${ROOT}/packaging/systemd/codexw-connector.env.example"
if grep -q -- "--agentd-auth-token\\|--broker-user\\|--broker-password\\|--enrollment-token-id\\|--enrollment-shared-secret" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"; then
  echo "systemd connector unit must not pass secrets on argv" >&2
  exit 1
fi
if grep -q -- "--agentd-auth-token\\|--broker-user\\|--broker-password\\|--broker-token\\|--enrollment-token-id\\|--enrollment-shared-secret" "${ROOT}/packaging/systemd/agentd-codexw-connector-self-test.service"; then
  echo "systemd connector self-test unit must not pass secrets on argv" >&2
  exit 1
fi
if grep -q -- "--reconnect" "${ROOT}/packaging/systemd/agentd-codexw-connector.service"; then
  echo "systemd connector unit must not enable in-process reconnect supervision" >&2
  exit 1
fi

echo "agentd_codexw_service_installers_smoke OK"
