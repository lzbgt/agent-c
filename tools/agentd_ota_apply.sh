#!/usr/bin/env bash
set -euo pipefail

plan_path="${1:-${AGENTD_OTA_PLAN_PATH:-}}"
if [[ -z "${plan_path}" ]]; then
  echo "missing plan path (arg or AGENTD_OTA_PLAN_PATH)" >&2
  exit 2
fi
if [[ ! -f "${plan_path}" ]]; then
  echo "plan file not found: ${plan_path}" >&2
  exit 2
fi

read_json() {
  python3 - <<PY
import json,sys
with open(sys.argv[1],'r',encoding='utf-8') as f:
    obj=json.load(f)
key=sys.argv[2]
val=obj.get(key,"")
print(val if val is not None else "")
PY
}

url="$(read_json "${plan_path}" url)"
sha256="$(read_json "${plan_path}" sha256)"
state_dir="$(read_json "${plan_path}" state_dir)"
pid="$(read_json "${plan_path}" agentd_pid)"
version="$(read_json "${plan_path}" version)"

if [[ -z "${state_dir}" ]]; then
  state_dir="$(dirname "${plan_path}")/.."
fi
state_dir="$(cd "${state_dir}" && pwd)"

log_dir="${state_dir}/ota"
mkdir -p "${log_dir}"
log_file="${log_dir}/ota_apply.log"
exec >>"${log_file}" 2>&1

echo "[ota] start $(date -u +%Y-%m-%dT%H:%M:%SZ) plan=${plan_path} version=${version}"

tmp_dir="${log_dir}/stage"
mkdir -p "${tmp_dir}"
artifact="${tmp_dir}/artifact"

fetch_artifact() {
  local src="$1"
  if [[ "${src}" == file://* ]]; then
    local p="${src#file://}"
    if [[ ! -f "${p}" ]]; then
      echo "[ota] file not found: ${p}" >&2
      exit 3
    fi
    cp "${p}" "${artifact}"
  else
    curl -fsSL "${src}" -o "${artifact}"
  fi
}

if [[ -z "${url}" ]]; then
  echo "[ota] missing url" >&2
  exit 3
fi
fetch_artifact "${url}"

sha256_file() {
  local f="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${f}" | awk '{print $1}'
  else
    shasum -a 256 "${f}" | awk '{print $1}'
  fi
}

if [[ -n "${sha256}" ]]; then
  got="$(sha256_file "${artifact}")"
  if [[ "${got}" != "${sha256}" ]]; then
    echo "[ota] sha256 mismatch expected=${sha256} got=${got}" >&2
    exit 4
  fi
fi

extract_dir="${tmp_dir}/extract"
rm -rf "${extract_dir}"
mkdir -p "${extract_dir}"

artifact_bin="${artifact}"
if [[ "${url}" == *.tar.gz || "${url}" == *.tgz ]]; then
  tar -xzf "${artifact}" -C "${extract_dir}"
  candidate="$(find "${extract_dir}" -maxdepth 4 -type f \( -name agentd -o -name agentd.exe \) | head -n 1)"
  if [[ -z "${candidate}" ]]; then
    echo "[ota] agentd binary not found in archive" >&2
    exit 5
  fi
  artifact_bin="${candidate}"
fi

if [[ -x "${AGENTD_OTA_TARGET_BIN:-}" ]]; then
  target_bin="${AGENTD_OTA_TARGET_BIN}"
else
  target_bin=""
  if [[ -n "${pid}" && -e "/proc/${pid}/exe" ]]; then
    target_bin="$(readlink "/proc/${pid}/exe")"
  fi
  if [[ -z "${target_bin}" && -n "${pid}" ]]; then
    target_bin="$(ps -p "${pid}" -o comm= 2>/dev/null | awk '{print $1}')"
  fi
  if [[ -z "${target_bin}" ]]; then
    target_bin="$(command -v agentd || true)"
  fi
fi

if [[ -z "${target_bin}" ]]; then
  echo "[ota] unable to resolve target agentd binary (set AGENTD_OTA_TARGET_BIN)" >&2
  exit 6
fi

backup="${target_bin}.bak.$(date -u +%Y%m%dT%H%M%SZ)"
cp "${target_bin}" "${backup}" || true

install -m 755 "${artifact_bin}" "${target_bin}"

echo "[ota] installed ${artifact_bin} -> ${target_bin} (backup=${backup})"

restart_mode="${AGENTD_OTA_RESTART:-signal}"
service_name="${AGENTD_OTA_SERVICE:-}"

if [[ "${restart_mode}" == "systemd" && -n "${service_name}" ]]; then
  systemctl restart "${service_name}"
elif [[ "${restart_mode}" == "launchd" && -n "${service_name}" ]]; then
  uid="$(id -u)"
  launchctl kickstart -k "gui/${uid}/${service_name}"
else
  if [[ -n "${pid}" ]]; then
    kill -TERM "${pid}" || true
  fi
fi

echo "[ota] done $(date -u +%Y-%m-%dT%H:%M:%SZ)"
