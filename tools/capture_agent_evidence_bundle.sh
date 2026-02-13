#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
Capture an agentd/broker evidence bundle (best-effort JSON snapshots).

Usage:
  tools/capture_agent_evidence_bundle.sh [options]

Options:
  --agentd-base <url>        agentd base URL (default: http://127.0.0.1:8123)
  --agentd-token <token>     agentd bearer token (default: AGENTD_AUTH_TOKEN)
  --broker-base <url>        broker base URL (optional)
  --broker-token <token>     broker OIDC bearer token (default: BROKER_AUTH_TOKEN)
  --broker-agent-id <id>     broker agent id for membership + agent proxy (optional)
  --agentd-via-broker        call agentd through broker proxy when broker base + agent id are set
  --trace-id <trace_id>      capture trace endpoints (agentd + broker)
  --out-dir <dir>            output directory (default: docs/artifacts/evidence/agent_evidence_<ts>)
  --no-agentd                skip agentd captures
  --no-broker                skip broker captures
  -h, --help                 show this help
USAGE
}

agentd_base="http://127.0.0.1:8123"
agentd_token="${AGENTD_AUTH_TOKEN:-}"
broker_base="${BROKER_BASE:-}"
broker_token="${BROKER_AUTH_TOKEN:-}"
broker_agent_id="${BROKER_AGENT_ID:-}"
agentd_via_broker=0
trace_id=""
no_agentd=0
no_broker=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --agentd-base)
      agentd_base="$2"
      shift 2
      ;;
    --agentd-token)
      agentd_token="$2"
      shift 2
      ;;
    --broker-base)
      broker_base="$2"
      shift 2
      ;;
    --broker-token)
      broker_token="$2"
      shift 2
      ;;
    --broker-agent-id)
      broker_agent_id="$2"
      shift 2
      ;;
    --agentd-via-broker)
      agentd_via_broker=1
      shift
      ;;
    --trace-id)
      trace_id="$2"
      shift 2
      ;;
    --out-dir)
      out_dir="$2"
      shift 2
      ;;
    --no-agentd)
      no_agentd=1
      shift
      ;;
    --no-broker)
      no_broker=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 1
      ;;
  esac
done

normalize_base() {
  local raw="$1"
  local base
  base="$(echo -n "${raw}" | sed -e 's/[[:space:]]*$//')"
  if [[ -z "${base}" ]]; then
    echo ""
    return
  fi
  if [[ "${base}" =~ ^https?:// ]]; then
    echo "${base%/}"
  else
    echo "https://${base%/}"
  fi
}

errors=()

fetch_json() {
  local url="$1"
  local out="$2"
  shift 2
  if ! curl -fsS "${url}" "$@" >"${out}"; then
    errors+=("${url}")
    return 1
  fi
  return 0
}

stamp="$(date +%Y%m%d_%H%M%S)"
out_dir="${out_dir:-${ROOT}/docs/artifacts/evidence/agent_evidence_${stamp}}"
mkdir -p "${out_dir}"

meta_json="${out_dir}/meta.json"
readme_txt="${out_dir}/README.txt"

agentd_base_norm="${agentd_base%/}"
broker_base_norm="$(normalize_base "${broker_base}")"

agentd_headers=()
broker_headers=()

if [[ -n "${agentd_token}" ]]; then
  agentd_headers+=("-H" "Authorization: Bearer ${agentd_token}")
fi

if [[ -n "${broker_token}" ]]; then
  broker_headers+=("-H" "Authorization: Bearer ${broker_token}")
fi

if [[ "${agentd_via_broker}" -eq 1 ]]; then
  if [[ -z "${broker_base_norm}" || -z "${broker_agent_id}" ]]; then
    echo "--agentd-via-broker requires --broker-base and --broker-agent-id" >&2
    exit 1
  fi
  agentd_base_norm="${broker_base_norm}/v1/agents/${broker_agent_id}/proxy"
  agentd_headers=()
  if [[ -n "${broker_token}" ]]; then
    agentd_headers+=("-H" "Authorization: Bearer ${broker_token}")
  fi
  if [[ -n "${agentd_token}" ]]; then
    agentd_headers+=("-H" "X-Agentd-Authorization: Bearer ${agentd_token}")
  fi
fi

cat <<EOF_META >"${meta_json}"
{
  "captured_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "agentd_base": "${agentd_base_norm}",
  "broker_base": "${broker_base_norm}",
  "broker_agent_id": "${broker_agent_id}",
  "agentd_via_broker": ${agentd_via_broker},
  "agentd_auth_set": $( [[ -n "${agentd_token}" ]] && echo true || echo false ),
  "broker_auth_set": $( [[ -n "${broker_token}" ]] && echo true || echo false ),
  "trace_id": "${trace_id}"
}
EOF_META

cat <<EOF_README >"${readme_txt}"
Agent evidence bundle

Captured at: $(date -u +%Y-%m-%dT%H:%M:%SZ)
Output dir: ${out_dir}

Reproduce (example):
  tools/capture_agent_evidence_bundle.sh \\
    --agentd-base "${agentd_base_norm}" \\
    ${broker_base_norm:+--broker-base "${broker_base_norm}"} \\
    ${broker_agent_id:+--broker-agent-id "${broker_agent_id}"}
EOF_README

if [[ "${no_agentd}" -eq 0 ]]; then
  fetch_json "${agentd_base_norm}/api/v1/health" "${out_dir}/agentd_health.json" "${agentd_headers[@]}" || true
  fetch_json "${agentd_base_norm}/api/v1/config" "${out_dir}/agentd_config.json" "${agentd_headers[@]}" || true
  fetch_json "${agentd_base_norm}/api/v1/diagnostics" "${out_dir}/agentd_diagnostics.json" "${agentd_headers[@]}" || true
  fetch_json "${agentd_base_norm}/api/v1/diagnostics/providers" "${out_dir}/agentd_diagnostics_providers.json" "${agentd_headers[@]}" || true
  fetch_json "${agentd_base_norm}/api/v1/sessions" "${out_dir}/agentd_sessions.json" "${agentd_headers[@]}" || true
  if [[ -n "${trace_id}" ]]; then
    fetch_json "${agentd_base_norm}/api/v1/trace?trace_id=${trace_id}&limit=200&max_bytes=1048576" "${out_dir}/agentd_trace.json" "${agentd_headers[@]}" || true
  fi
fi

if [[ "${no_broker}" -eq 0 && -n "${broker_base_norm}" ]]; then
  fetch_json "${broker_base_norm}/healthz" "${out_dir}/broker_healthz.json" "${broker_headers[@]}" || true
  fetch_json "${broker_base_norm}/readyz" "${out_dir}/broker_readyz.json" "${broker_headers[@]}" || true
  fetch_json "${broker_base_norm}/v1/agents" "${out_dir}/broker_agents.json" "${broker_headers[@]}" || true
  if [[ -n "${broker_agent_id}" ]]; then
    fetch_json "${broker_base_norm}/v1/agents/${broker_agent_id}/members" "${out_dir}/broker_members.json" "${broker_headers[@]}" || true
    fetch_json "${broker_base_norm}/v1/agents/${broker_agent_id}/membership_audit?limit=200" "${out_dir}/broker_membership_audit.json" "${broker_headers[@]}" || true
  fi
  if [[ -n "${trace_id}" ]]; then
    fetch_json "${broker_base_norm}/v1/trace?trace_id=${trace_id}&limit=200&fanout=1" "${out_dir}/broker_trace.json" "${broker_headers[@]}" || true
  fi
fi

if [[ ${#errors[@]} -gt 0 ]]; then
  printf '%s\n' "${errors[@]}" >"${out_dir}/errors.txt"
  echo "Warnings: some endpoints failed. See ${out_dir}/errors.txt" >&2
fi

printf 'Bundle captured at %s\n' "${out_dir}"
