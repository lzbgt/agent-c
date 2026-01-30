#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

MODE="host"   # host|core
SKIP_UI=0
UI_INSTALL=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-only)
      MODE="core"
      shift 1
      ;;
    --skip-ui)
      SKIP_UI=1
      shift 1
      ;;
    --ui-install)
      UI_INSTALL=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/verify.sh [--core-only] [--skip-ui] [--ui-install]

Runs a local verification pass with timestamped logs under ./build/.

Modes:
  --core-only   Configure/build/test core-only (AGENT_BUILD_HOST=OFF) in ./build-core/

UI:
  By default, runs `npm run build` in ./ui/ only if ./ui/node_modules exists.
  --ui-install  Run `npm ci` before `npm run build`
  --skip-ui     Skip UI build entirely
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

ts="$(date +%Y%m%d_%H%M%S)"
log_dir="${ROOT}/build"
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
  echo "[verify] ${label}"
  if ! "$@" >"${log}" 2>&1; then
    echo "[verify] FAILED: ${label} (log: ${log})" >&2
    fail_tail "${log}"
    return 1
  fi
  echo "[verify] OK: ${label} (log: ${log})"
}

if [[ "${MODE}" == "core" ]]; then
  build_dir="${ROOT}/build-core"
  cfg_log="${log_dir}/verify_${ts}_cmake_configure_core.log"
  build_log="${log_dir}/verify_${ts}_cmake_build_core.log"
  test_log="${log_dir}/verify_${ts}_ctest_core.log"

  run_logged "cmake configure (core-only)" "${cfg_log}" cmake -S . -B "${build_dir}" -DAGENT_BUILD_HOST=OFF
  run_logged "cmake build (core-only)" "${build_log}" cmake --build "${build_dir}" -j
  run_logged "ctest (core-only)" "${test_log}" ctest --test-dir "${build_dir}" --output-on-failure
else
  build_dir="${ROOT}/build"
  cfg_log="${log_dir}/verify_${ts}_cmake_configure.log"
  build_log="${log_dir}/verify_${ts}_cmake_build.log"
  test_log="${log_dir}/verify_${ts}_ctest.log"

  run_logged "cmake configure" "${cfg_log}" cmake -S . -B "${build_dir}"
  run_logged "cmake build" "${build_log}" cmake --build "${build_dir}" -j
  run_logged "ctest" "${test_log}" ctest --test-dir "${build_dir}" --output-on-failure
fi

if [[ "${SKIP_UI}" == "1" ]]; then
  echo "[verify] skip UI build (--skip-ui)"
  exit 0
fi

if [[ ! -f "${ROOT}/ui/package.json" ]]; then
  echo "[verify] UI not present (ui/package.json missing)"
  exit 0
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "[verify] npm not found; skipping UI build" >&2
  exit 0
fi

ui_log="${log_dir}/verify_${ts}_ui_build.log"
ui_install_log="${log_dir}/verify_${ts}_ui_install.log"

if [[ "${UI_INSTALL}" == "1" ]]; then
  run_logged "ui: npm ci" "${ui_install_log}" bash -lc "cd ui && npm ci"
fi

if [[ ! -d "${ROOT}/ui/node_modules" ]]; then
  echo "[verify] UI dependencies not installed (ui/node_modules missing); skipping UI build."
  echo "[verify] Run: tools/verify.sh --ui-install   (or: cd ui && npm install)"
  exit 0
fi

run_logged "ui: npm run build" "${ui_log}" bash -lc "cd ui && npm run build"

