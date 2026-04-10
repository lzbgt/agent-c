#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="write"
if [[ "${1:-}" == "--check" ]]; then
  MODE="check"
fi

OPENAPI_TYPESCRIPT_BIN="${ROOT}/ui/node_modules/.bin/openapi-typescript"
if [[ ! -x "${OPENAPI_TYPESCRIPT_BIN}" ]]; then
  echo "missing generator: ${OPENAPI_TYPESCRIPT_BIN}" >&2
  echo "run: cd ui && npm install" >&2
  exit 1
fi

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/agent-openapi-types.XXXXXX")"
cleanup() {
  rm -rf "${tmp_dir}"
}
trap cleanup EXIT

bundle_and_generate() {
  local spec_name="$1"
  local bundled="${tmp_dir}/${spec_name}.openapi.yaml"
  local output="${tmp_dir}/${spec_name}-openapi.d.ts"
  python3 "${ROOT}/tools/check_openapi_component_refs.py" "${ROOT}/docs/openapi/${spec_name}.yaml"
  python3 "${ROOT}/tools/openapi_bundle.py" "${ROOT}/docs/openapi/${spec_name}.yaml" -o "${bundled}"
  NPM_CONFIG_CACHE="${ROOT}/.npm-cache" "${OPENAPI_TYPESCRIPT_BIN}" "${bundled}" -o "${output}"
}

bundle_and_generate "agentd"
bundle_and_generate "broker"

mkdir -p "${ROOT}/ui/src/api/generated"

for spec_name in agentd broker; do
  src="${tmp_dir}/${spec_name}-openapi.d.ts"
  dst="${ROOT}/ui/src/api/generated/${spec_name}-openapi.d.ts"
  if [[ "${MODE}" == "check" ]]; then
    if [[ ! -f "${dst}" ]]; then
      echo "missing generated file: ${dst}" >&2
      exit 1
    fi
    if ! cmp -s "${src}" "${dst}"; then
      echo "generated OpenAPI types are stale: ${dst}" >&2
      diff -u "${dst}" "${src}" || true
      exit 1
    fi
  else
    cp "${src}" "${dst}"
  fi
done

if [[ "${MODE}" == "write" ]]; then
  echo "updated ui/src/api/generated/{agentd,broker}-openapi.d.ts"
fi
