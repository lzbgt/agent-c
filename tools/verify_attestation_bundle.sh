#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

if [[ $# -lt 2 ]]; then
  cat <<'USAGE' >&2
usage: tools/verify_attestation_bundle.sh <attestation_json> <replay_json> [verify args]

Examples:
  tools/verify_attestation_bundle.sh build/attest.json build/replay.json --hmac-key-hex <hex>
  tools/verify_attestation_bundle.sh build/attest.json build/replay.json --ed25519-pubkey-b64 <b64>

Notes:
  - This script calls run_attestation_bundle_tool --verify
  - Set RUN_ATTESTATION_BUNDLE_TOOL to override the tool path
USAGE
  exit 2
fi

att_json="$1"
shift
replay_json="$1"
shift

TOOL_PATH="${RUN_ATTESTATION_BUNDLE_TOOL:-${ROOT}/build/run_attestation_bundle_tool}"
if [[ ! -x "${TOOL_PATH}" ]]; then
  echo "run_attestation_bundle_tool not found at ${TOOL_PATH}" >&2
  echo "Build it with: cmake --build build --target run_attestation_bundle_tool" >&2
  exit 2
fi

"${TOOL_PATH}" --verify --attestation-json "${att_json}" --replay-json "${replay_json}" "$@"
