#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
if [[ -f "${ROOT}/tools/lib/devstack_state.sh" ]]; then
  # shellcheck source=tools/lib/devstack_state.sh
  source "${ROOT}/tools/lib/devstack_state.sh"
fi

MODE="host"   # host|core
SKIP_UI=0
UI_INSTALL=0
REPO_GUARDS=0
REPO_GUARDS_STRICT=0
EVAL_PACK=0
EVAL_PACK_BASELINE=""
EVAL_PACK_UPDATE=0
INCLUDE_COMPOSE_TESTS=0

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
    --repo-guards)
      REPO_GUARDS=1
      shift 1
      ;;
    --repo-guards-strict)
      REPO_GUARDS=1
      REPO_GUARDS_STRICT=1
      shift 1
      ;;
    --eval-pack)
      EVAL_PACK=1
      shift 1
      ;;
    --eval-pack-baseline)
      EVAL_PACK=1
      EVAL_PACK_BASELINE="${2:-}"
      shift 2
      ;;
    --eval-pack-update-baseline)
      EVAL_PACK_UPDATE=1
      shift 1
      ;;
    --include-compose-tests)
      INCLUDE_COMPOSE_TESTS=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/verify.sh [--core-only] [--skip-ui] [--ui-install] [--repo-guards] [--repo-guards-strict] [--eval-pack] [--eval-pack-baseline <path|auto>] [--eval-pack-update-baseline] [--include-compose-tests]

Runs a local verification pass with timestamped logs under ./build/.

Modes:
  --core-only   Configure/build/test core-only (AGENT_BUILD_HOST=OFF) in ./build-core/
  --include-compose-tests  Include compose broker/WebUI smoke tests in ctest (default verify excludes them to preserve the singleton devstack).

UI:
  By default, runs `npm run build` in ./ui/ only if ./ui/node_modules exists.
  --ui-install  Run `npm ci` before `npm run build`
  --skip-ui     Skip UI build entirely

Guards:
  --repo-guards        Run repo hygiene guards after build/tests.
  --repo-guards-strict Run repo hygiene guards in strict mode (nested .git detection).

Eval:
  --eval-pack  Run eval pack smoke after build/tests and compare against the canonical baseline by default.
  --eval-pack-baseline <path|auto>  Compare eval pack summary to a baseline, or use 'auto' for ref/eval_packs/<pack>.summary.json.
  --eval-pack-update-baseline  Write current eval pack baseline (defaults to canonical baseline when path omitted).
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

run_ctest_logged_with_retry() {
  # Network smoke tests can be flaky (DNS / transient provider issues). Retry failed tests once to reduce false negatives.
  local label="${1}"
  local log1="${2}"
  local log2="${3}"
  local build_dir="${4}"
  shift 4
  local ctest_args=("$@")

  echo "[verify] ${label}"
  if ctest --test-dir "${build_dir}" "${ctest_args[@]}" --output-on-failure >"${log1}" 2>&1; then
    echo "[verify] OK: ${label} (log: ${log1})"
    return 0
  fi

  echo "[verify] WARN: ${label} failed; rerun failed tests once (--rerun-failed)" >&2
  if ctest --test-dir "${build_dir}" --rerun-failed "${ctest_args[@]}" --output-on-failure >"${log2}" 2>&1; then
    echo "[verify] OK (after rerun-failed): ${label} (log: ${log2})"
    return 0
  fi

  echo "[verify] FAILED: ${label} (logs: ${log1}, ${log2})" >&2
  fail_tail "${log1}"
  fail_tail "${log2}"
  return 1
}

clean_extra_compose_stacks() {
  local preserve_project="" state_path="${ROOT}/out/devstack_state.json" projects project
  if ! command -v docker >/dev/null 2>&1; then
    return 0
  fi
  if [[ -f "${state_path}" ]] && declare -F devstack_state_field >/dev/null 2>&1; then
    preserve_project="$(devstack_state_field "${state_path}" "compose_project")"
  fi
  projects="$(
    docker ps --format '{{.Label "com.docker.compose.project"}}' 2>/dev/null \
      | rg -N '^agent_' \
      | sort -u || true
  )"
  while IFS= read -r project; do
    if [[ -z "${project}" || "${project}" == "${preserve_project}" ]]; then
      continue
    fi
    echo "[verify] cleaning stray compose stack: ${project}"
    docker compose -p "${project}" down -v --remove-orphans >/dev/null 2>&1 || true
  done <<<"${projects}"
}

if [[ "${MODE}" == "core" ]]; then
  build_dir="${ROOT}/build-core"
  cfg_log="${log_dir}/verify_${ts}_cmake_configure_core.log"
  build_log="${log_dir}/verify_${ts}_cmake_build_core.log"
  test_log="${log_dir}/verify_${ts}_ctest_core.log"
  test_retry_log="${log_dir}/verify_${ts}_ctest_core_rerun_failed.log"

  run_logged "cmake configure (core-only)" "${cfg_log}" cmake -S . -B "${build_dir}" -DAGENT_BUILD_HOST=OFF
  run_logged "cmake build (core-only)" "${build_log}" cmake --build "${build_dir}" -j
  run_ctest_logged_with_retry "ctest (core-only)" "${test_log}" "${test_retry_log}" "${build_dir}"
else
  build_dir="${ROOT}/build"
  cfg_log="${log_dir}/verify_${ts}_cmake_configure.log"
  build_log="${log_dir}/verify_${ts}_cmake_build.log"
  test_log="${log_dir}/verify_${ts}_ctest.log"
  test_retry_log="${log_dir}/verify_${ts}_ctest_rerun_failed.log"

  run_logged "cmake configure" "${cfg_log}" cmake -S . -B "${build_dir}"
  run_logged "cmake build" "${build_log}" cmake --build "${build_dir}" -j
  ctest_args=()
  if [[ "${INCLUDE_COMPOSE_TESTS}" != "1" ]]; then
    clean_extra_compose_stacks
    ctest_args=(-E 'broker_.*_compose_smoke')
  fi
  run_ctest_logged_with_retry "ctest" "${test_log}" "${test_retry_log}" "${build_dir}" "${ctest_args[@]}"
fi

if [[ "${REPO_GUARDS}" == "1" ]]; then
  repo_log="${log_dir}/verify_${ts}_repo_guards.log"
  repo_args=""
  if [[ "${REPO_GUARDS_STRICT}" == "1" ]]; then
    repo_args="--strict"
  fi
  run_logged "repo guards" "${repo_log}" bash -lc "tools/verify_repo_guards.sh ${repo_args}"
fi

if [[ "${EVAL_PACK}" == "1" ]]; then
  eval_log="${log_dir}/verify_${ts}_eval_pack.log"
  eval_args=(--file "${ROOT}/tools/eval_packs/eval_pack_smoke.json")
  if [[ -n "${EVAL_PACK_BASELINE}" ]]; then
    eval_args+=(--baseline "${EVAL_PACK_BASELINE}")
  else
    eval_args+=(--baseline auto)
  fi
  if [[ "${EVAL_PACK_UPDATE}" == "1" ]]; then
    eval_args+=(--update-baseline)
  fi
  run_logged "eval pack smoke" "${eval_log}" python3 "${ROOT}/tools/eval_pack.py" "${eval_args[@]}"
fi

if [[ "${SKIP_UI}" == "1" ]]; then
  echo "[verify] skip UI build (--skip-ui)"
  exit 0
fi

# Optional smoke tests (host-only)
if [[ "${MODE}" == "host" ]]; then
  smoke_log="${log_dir}/verify_${ts}_workflow_list_query_smoke.log"
  run_logged "workflow list query smoke" "${smoke_log}" bash -lc "${ROOT}/tests/agentd_workflow_list_query_smoke.sh ${build_dir}/agentd"
  schedule_smoke_log="${log_dir}/verify_${ts}_workflow_schedule_smoke.log"
  run_logged "workflow schedule smoke" "${schedule_smoke_log}" bash -lc "${ROOT}/tests/agentd_workflow_schedule_smoke.sh ${build_dir}/agentd"
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
ui_openapi_log="${log_dir}/verify_${ts}_ui_openapi_types.log"
ui_cache_env="NPM_CONFIG_CACHE=./.npm-cache"

if [[ "${UI_INSTALL}" == "1" ]]; then
  run_logged "ui: npm ci" "${ui_install_log}" bash -lc "cd ui && ${ui_cache_env} npm ci"
fi

if [[ ! -d "${ROOT}/ui/node_modules" ]]; then
  echo "[verify] UI dependencies not installed (ui/node_modules missing); skipping UI build."
  echo "[verify] Run: tools/verify.sh --ui-install   (or: cd ui && npm install)"
  exit 0
fi

run_logged "ui: npm run openapi:types:check" "${ui_openapi_log}" bash -lc "cd ui && ${ui_cache_env} npm run openapi:types:check"
run_logged "ui: npm run build" "${ui_log}" bash -lc "cd ui && ${ui_cache_env} npm run build"
