#!/usr/bin/env bash
set -euo pipefail

# Generate a small self-signed CA + server + agent client certificates for local broker mTLS testing.
#
# Output folder structure:
#   <out>/
#     ca.pem
#     ca.key.pem
#     server.pem
#     server.key.pem
#     client.pem              (compat alias for first agent id)
#     client.key.pem          (compat alias for first agent id)
#     client_<agent_id>.pem
#     client_<agent_id>.key.pem
#
# CN convention:
# - client CN is "agentd-<id>" to match the broker default.
#
# This is for local testing only. Do not commit keys.

OUT_DIR="${1:-tools/_agentd_broker_mtls_test_certs}"
shift || true

AGENT_IDS=("$@")
if [[ "${#AGENT_IDS[@]}" -eq 0 ]]; then
  AGENT_IDS=("1")
fi

mkdir -p "${OUT_DIR}"

CA_KEY="${OUT_DIR}/ca.key.pem"
CA_CERT="${OUT_DIR}/ca.pem"
SERVER_KEY="${OUT_DIR}/server.key.pem"
SERVER_CERT="${OUT_DIR}/server.pem"
CLIENT_KEY_ALIAS="${OUT_DIR}/client.key.pem"
CLIENT_CERT_ALIAS="${OUT_DIR}/client.pem"

echo "[mtls] out=${OUT_DIR}"

if ! command -v openssl >/dev/null 2>&1; then
  echo "ERROR: openssl not found" >&2
  exit 1
fi

# CA
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "${CA_KEY}" >/dev/null 2>&1
openssl req -x509 -new -nodes -key "${CA_KEY}" -sha256 -days 3650 \
  -subj "/CN=AgentD Broker Dev CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out "${CA_CERT}" >/dev/null 2>&1

# Server
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "${SERVER_KEY}" >/dev/null 2>&1
openssl req -new -key "${SERVER_KEY}" -subj "/CN=localhost" \
  -out "${OUT_DIR}/server.csr.pem" >/dev/null 2>&1
openssl x509 -req -in "${OUT_DIR}/server.csr.pem" -CA "${CA_CERT}" -CAkey "${CA_KEY}" -CAcreateserial \
  -out "${SERVER_CERT}" -days 3650 -sha256 \
  -extfile <(printf "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,DNS:broker,IP:127.0.0.1\n") >/dev/null 2>&1
rm -f "${OUT_DIR}/server.csr.pem"

# Clients
first=1
for agent_id in "${AGENT_IDS[@]}"; do
  if [[ -z "${agent_id}" ]]; then
    echo "ERROR: agent_id empty" >&2
    exit 2
  fi
  if ! [[ "${agent_id}" =~ ^[0-9A-Za-z._-]+$ ]]; then
    echo "ERROR: agent_id has unsupported chars: ${agent_id}" >&2
    exit 2
  fi

  client_key="${OUT_DIR}/client_${agent_id}.key.pem"
  client_cert="${OUT_DIR}/client_${agent_id}.pem"
  csr="${OUT_DIR}/client_${agent_id}.csr.pem"

  openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "${client_key}" >/dev/null 2>&1
  openssl req -new -key "${client_key}" -subj "/CN=agentd-${agent_id}" -out "${csr}" >/dev/null 2>&1
  openssl x509 -req -in "${csr}" -CA "${CA_CERT}" -CAkey "${CA_KEY}" -CAcreateserial \
    -out "${client_cert}" -days 3650 -sha256 \
    -extfile <(printf "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n") >/dev/null 2>&1
  rm -f "${csr}"

  chmod 600 "${client_key}"

  if [[ "${first}" == "1" ]]; then
    cp -f "${client_key}" "${CLIENT_KEY_ALIAS}"
    cp -f "${client_cert}" "${CLIENT_CERT_ALIAS}"
    chmod 600 "${CLIENT_KEY_ALIAS}"
    first=0
  fi
done

rm -f "${OUT_DIR}/ca.srl"
chmod 600 "${CA_KEY}" "${SERVER_KEY}"

echo "[mtls] ok:"
echo "  - ca:     ${CA_CERT}"
echo "  - server: ${SERVER_CERT} ${SERVER_KEY}"
echo "  - clients:"
for agent_id in "${AGENT_IDS[@]}"; do
  echo "    - agentd-${agent_id}: ${OUT_DIR}/client_${agent_id}.pem ${OUT_DIR}/client_${agent_id}.key.pem"
done
echo "  - compat: ${CLIENT_CERT_ALIAS} ${CLIENT_KEY_ALIAS}"
