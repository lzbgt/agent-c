#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<'USAGE'
Usage: tools/macos_package.sh

Builds, signs, and packages agentd (and optionally agent) into a macOS .pkg.

Environment variables:
  CODESIGN_IDENTITY     Developer ID Application identity (required to sign binaries)
  PKG_SIGN_IDENTITY     Developer ID Installer identity (required to sign .pkg)
  NOTARY_PROFILE        notarytool keychain profile (optional; enables notarization)
  AGENTD_BIN            path to agentd binary (default: ./build/agentd)
  AGENT_BIN             path to agent binary (default: ./build/agent, optional)
  MACOS_PKG_ID           package identifier (default: com.agentd.pkg)
  MACOS_PKG_VERSION      package version (default: git-describe + date)
  MACOS_PKG_NAME         output base name (default: agentd)
  MACOS_PKG_OUT_DIR      output directory (default: out/macos_pkg_<timestamp>)
  MACOS_PKG_INSTALL_PREFIX install prefix for binaries (default: /usr/local/bin)
USAGE
  exit 0
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[macos-package] WARNING: not running on macOS; continuing anyway"
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[macos-package] cmake not found" >&2
  exit 1
fi

if ! command -v codesign >/dev/null 2>&1; then
  echo "[macos-package] codesign not found (install Xcode CLT)" >&2
  exit 1
fi

if ! command -v pkgbuild >/dev/null 2>&1; then
  echo "[macos-package] pkgbuild not found (install Xcode CLT)" >&2
  exit 1
fi

if ! command -v productsign >/dev/null 2>&1; then
  echo "[macos-package] productsign not found (install Xcode CLT)" >&2
  exit 1
fi

ts="$(date +%Y%m%d_%H%M%S)"
out_dir_default="${ROOT}/out/macos_pkg_${ts}"
OUT_DIR="${MACOS_PKG_OUT_DIR:-${out_dir_default}}"
mkdir -p "${OUT_DIR}"

log_dir="${OUT_DIR}/logs"
mkdir -p "${log_dir}"

fail_tail() {
  local log="${1}"
  echo "---- tail ${log} ----" >&2
  tail -n 120 "${log}" >&2 || true
}

run_logged() {
  local label="${1}"
  local log="${2}"
  shift 2
  echo "[macos-package] ${label}"
  if ! "$@" >"${log}" 2>&1; then
    echo "[macos-package] FAILED: ${label} (log: ${log})" >&2
    fail_tail "${log}"
    return 1
  fi
  echo "[macos-package] OK: ${label} (log: ${log})"
}

build_dir="${ROOT}/build"
cfg_log="${log_dir}/cmake_configure.log"
build_log="${log_dir}/cmake_build.log"

run_logged "cmake configure" "${cfg_log}" cmake -S "${ROOT}" -B "${build_dir}"
run_logged "cmake build" "${build_log}" cmake --build "${build_dir}" -j

AGENTD_BIN="${AGENTD_BIN:-${build_dir}/agentd}"
AGENT_BIN="${AGENT_BIN:-${build_dir}/agent}"

if [[ ! -x "${AGENTD_BIN}" ]]; then
  echo "[macos-package] agentd binary not found: ${AGENTD_BIN}" >&2
  exit 1
fi

PKG_ID="${MACOS_PKG_ID:-com.agentd.pkg}"
PKG_VERSION_DEFAULT="$(git describe --tags --always 2>/dev/null || echo dev)-$(date +%Y%m%d)"
PKG_VERSION="${MACOS_PKG_VERSION:-${PKG_VERSION_DEFAULT}}"
PKG_NAME="${MACOS_PKG_NAME:-agentd}"
INSTALL_PREFIX="${MACOS_PKG_INSTALL_PREFIX:-/usr/local/bin}"

if [[ "${INSTALL_PREFIX}" != /* ]]; then
  echo "[macos-package] install prefix must be absolute: ${INSTALL_PREFIX}" >&2
  exit 1
fi

if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
  echo "[macos-package] CODESIGN_IDENTITY is required" >&2
  exit 1
fi

if [[ -z "${PKG_SIGN_IDENTITY:-}" ]]; then
  echo "[macos-package] PKG_SIGN_IDENTITY is required" >&2
  exit 1
fi

stage_dir="${OUT_DIR}/stage"
stage_bin_dir="${stage_dir}${INSTALL_PREFIX}"
mkdir -p "${stage_bin_dir}"

install -m 0755 "${AGENTD_BIN}" "${stage_bin_dir}/agentd"
if [[ -x "${AGENT_BIN}" ]]; then
  install -m 0755 "${AGENT_BIN}" "${stage_bin_dir}/agent"
else
  echo "[macos-package] agent binary not found; skipping: ${AGENT_BIN}"
fi

codesign_agentd_log="${log_dir}/codesign_agentd.log"
codesign_agent_log="${log_dir}/codesign_agent.log"
verify_agentd_log="${log_dir}/codesign_verify_agentd.log"
verify_agent_log="${log_dir}/codesign_verify_agent.log"

run_logged "codesign agentd" "${codesign_agentd_log}" codesign --force --options runtime --timestamp \
  --sign "${CODESIGN_IDENTITY}" "${stage_bin_dir}/agentd"

if [[ -x "${stage_bin_dir}/agent" ]]; then
  run_logged "codesign agent" "${codesign_agent_log}" codesign --force --options runtime --timestamp \
    --sign "${CODESIGN_IDENTITY}" "${stage_bin_dir}/agent"
fi

run_logged "codesign verify" "${verify_agentd_log}" codesign --verify --strict --deep "${stage_bin_dir}/agentd"
if [[ -x "${stage_bin_dir}/agent" ]]; then
  run_logged "codesign verify (agent)" "${verify_agent_log}" codesign --verify --strict --deep "${stage_bin_dir}/agent"
fi

unsigned_pkg="${OUT_DIR}/${PKG_NAME}_unsigned.pkg"
signed_pkg="${OUT_DIR}/${PKG_NAME}.pkg"
pkgbuild_log="${log_dir}/pkgbuild.log"
productsign_log="${log_dir}/productsign.log"

run_logged "pkgbuild" "${pkgbuild_log}" pkgbuild \
  --root "${stage_dir}" \
  --identifier "${PKG_ID}" \
  --version "${PKG_VERSION}" \
  --install-location / \
  "${unsigned_pkg}"

run_logged "productsign" "${productsign_log}" productsign \
  --sign "${PKG_SIGN_IDENTITY}" \
  "${unsigned_pkg}" "${signed_pkg}"

final_pkg="${signed_pkg}"

if [[ -n "${NOTARY_PROFILE:-}" ]]; then
  notary_log="${log_dir}/notarytool.log"
  staple_log="${log_dir}/stapler.log"
  run_logged "notarytool submit" "${notary_log}" xcrun notarytool submit "${final_pkg}" \
    --keychain-profile "${NOTARY_PROFILE}" --wait
  run_logged "stapler staple" "${staple_log}" xcrun stapler staple "${final_pkg}"
  run_logged "stapler validate" "${staple_log}" xcrun stapler validate "${final_pkg}"
else
  echo "[macos-package] NOTARY_PROFILE not set; skipping notarization"
fi

sha_log="${log_dir}/sha256.log"
run_logged "sha256" "${sha_log}" shasum -a 256 "${final_pkg}"
cp "${sha_log}" "${OUT_DIR}/SHA256SUMS.txt"

echo "[macos-package] DONE"
echo "  pkg: ${final_pkg}"
echo "  out: ${OUT_DIR}"
