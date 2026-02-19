#!/usr/bin/env bash
set -euo pipefail

TOOL="${1:-}"
ED_TOOL="${2:-}"
if [[ -z "${TOOL}" || -z "${ED_TOOL}" ]]; then
  echo "usage: run_attestation_bundle_tool_smoke.sh <tool_path> <ed25519_tool_path>" >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

BUNDLE_JSON="${TMP_DIR}/bundle.json"
OUT1="${TMP_DIR}/attest1.json"
OUT2="${TMP_DIR}/attest2.json"
OUT3="${TMP_DIR}/attest3.json"

cat > "${BUNDLE_JSON}" <<'JSON'
{
  "schema": "run_replay_bundle_v1",
  "request": {"prompt": "hello"},
  "response": {"assistant_text": "OK"},
  "tool_records": []
}
JSON

KEY_HEX="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

"${TOOL}" \
  --replay-json "${BUNDLE_JSON}" \
  --created-utc-ms 1700000000000 \
  --kid test-kid \
  --issuer test-issuer \
  --run-id run-test-1 \
  --hmac-key-hex "${KEY_HEX}" \
  --out "${OUT1}"

"${TOOL}" \
  --replay-json "${BUNDLE_JSON}" \
  --created-utc-ms 1700000000000 \
  --kid test-kid \
  --issuer test-issuer \
  --run-id run-test-1 \
  --hmac-key-hex "${KEY_HEX}" \
  --out "${OUT2}"

"${TOOL}" \
  --verify \
  --attestation-json "${OUT1}" \
  --replay-json "${BUNDLE_JSON}" \
  --hmac-key-hex "${KEY_HEX}" >/dev/null

SK_HEX="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
PK_B64="$("${ED_TOOL}" --sk-hex "${SK_HEX}" --print-pk-b64)"

"${TOOL}" \
  --replay-json "${BUNDLE_JSON}" \
  --created-utc-ms 1700000000000 \
  --kid test-ed25519-kid \
  --ed25519-seed-hex "${SK_HEX}" \
  --out "${OUT3}"

"${TOOL}" \
  --verify \
  --attestation-json "${OUT3}" \
  --replay-json "${BUNDLE_JSON}" \
  --ed25519-pubkey-b64 "${PK_B64}" >/dev/null

export OUT1 OUT2 OUT3
python3 - <<'PY'
import json, sys
from pathlib import Path
import os

out1 = json.loads(Path(os.environ["OUT1"]).read_text())
out2 = json.loads(Path(os.environ["OUT2"]).read_text())
out3 = json.loads(Path(os.environ["OUT3"]).read_text())

if out1.get("schema") != "run_attestation_bundle_v1":
  print("unexpected schema", out1.get("schema"), file=sys.stderr)
  sys.exit(1)

sha = out1.get("replay_sha256")
if not (isinstance(sha, str) and sha.startswith("sha256:")):
  print("missing replay_sha256", sha, file=sys.stderr)
  sys.exit(1)

if out1.get("replay_sha256_alg") != "agent_json_c14n_v1":
  print("unexpected replay_sha256_alg", out1.get("replay_sha256_alg"), file=sys.stderr)
  sys.exit(1)

if out1.get("replay_sha256_schema") != "run_replay_bundle_v1":
  print("unexpected replay_sha256_schema", out1.get("replay_sha256_schema"), file=sys.stderr)
  sys.exit(1)

if out1.get("created_utc_ms") != 1700000000000:
  print("unexpected created_utc_ms", out1.get("created_utc_ms"), file=sys.stderr)
  sys.exit(1)

att = out1.get("attest") or {}
if att.get("alg") != "hmac-sha256":
  print("unexpected attest alg", att, file=sys.stderr)
  sys.exit(1)

if att.get("kid") != "test-kid":
  print("unexpected attest kid", att, file=sys.stderr)
  sys.exit(1)

sig = att.get("sig") or ""
if not (isinstance(sig, str) and len(sig) >= 40):
  print("missing attest sig", att, file=sys.stderr)
  sys.exit(1)

if att.get("hash_alg") != "agent_json_c14n_v1":
  print("unexpected attest hash_alg", att.get("hash_alg"), file=sys.stderr)
  sys.exit(1)

if att.get("signing_schema") != "run_attestation_bundle_v1":
  print("unexpected attest signing_schema", att.get("signing_schema"), file=sys.stderr)
  sys.exit(1)

att2 = out2.get("attest") or {}
if att2.get("sig") != sig:
  print("signature mismatch between runs", sig, att2.get("sig"), file=sys.stderr)
  sys.exit(1)

att3 = out3.get("attest") or {}
if att3.get("alg") != "ed25519":
  print("unexpected ed25519 alg", att3.get("alg"), file=sys.stderr)
  sys.exit(1)

print("run_attestation_bundle_tool_smoke OK")
PY
