#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

SCRIPT_DIR="${ROOT}/tests"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"
# shellcheck source=tools/lib/agent_env.sh
source "${ROOT}/tools/lib/agent_env.sh"
agent_env_source_home_if_unset >/dev/null 2>&1 || true

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

if [[ "${AGENT_DISABLE_NETWORK_TESTS:-}" == "1" ]]; then
  echo "SKIP: AGENT_DISABLE_NETWORK_TESTS=1" >&2
  exit 77
fi

source "${SCRIPT_DIR}/test_keys.sh"
agent_test_setup_proxy_env
OPENROUTER_KEY="$(agent_test_get_key openrouter 2>/dev/null || true)"
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "missing OPENROUTER_API_KEY (env or project.local.md)" >&2
  exit 2
fi

if ! agent_test_openrouter_auth_ok "${OPENROUTER_KEY}" "${OPENROUTER_API_BASE:-https://openrouter.ai/api/v1}"; then
  rc=$?
  echo "HINT: run tools/openrouter_auth_debug.sh for detailed diagnostics" >&2
  if [[ "${rc}" -eq 77 ]]; then
    exit 77
  fi
  exit 1
fi

BASE_URL="${OPENROUTER_API_BASE:-https://openrouter.ai/api/v1}"
MODEL_DEFAULT="${AGENT_TEST_OPENROUTER_MODEL:-bytedance-seed/seed-1.6-flash}"
MIN_TOTAL="${OPENROUTER_MIN_TOTAL:-0.01}"
MAX_TOTAL="${OPENROUTER_MAX_TOTAL:-0.50}"
LIMIT="${OPENROUTER_MODEL_LIMIT:-30}"
MAX_MODELS="${OPENROUTER_STREAM_PROBE_MAX_MODELS:-12}"
MODELS_OVERRIDE="${OPENROUTER_STREAM_PROBE_MODELS:-}"
SAVE_STREAMS="${OPENROUTER_STREAM_PROBE_SAVE:-0}"
WRITE_PINS="${OPENROUTER_STREAM_PROBE_WRITE_PINS:-0}"
PINS_PATH="${OPENROUTER_STREAM_PINS_PATH:-${ROOT}/ref/openrouter/streaming_pins.json}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

LOG_DIR="${ROOT}/build/openrouter_probe"
mkdir -p "${LOG_DIR}"

result_file="${LOG_DIR}/probe_$(date +%Y%m%d_%H%M%S).json"
stream_dir="${LOG_DIR}/streams_$(date +%Y%m%d_%H%M%S)"
if [[ "${SAVE_STREAMS}" == "1" ]]; then
  mkdir -p "${stream_dir}"
fi

OPENROUTER_API_KEY="${OPENROUTER_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "openrouter_stream_probe" \
  --base-url "${BASE_URL}" \
  --model "${MODEL_DEFAULT}" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

candidates_file="${LOG_DIR}/candidates_$(date +%Y%m%d_%H%M%S).txt"
if [[ -n "${MODELS_OVERRIDE}" ]]; then
  echo "[probe] using OPENROUTER_STREAM_PROBE_MODELS override" >&2
  MODELS_OVERRIDE="${MODELS_OVERRIDE}" python3 - <<PY >"${candidates_file}"
import os
import re
raw = os.environ.get("MODELS_OVERRIDE", "")
parts = [p for p in re.split(r"[\\s,]+", raw) if p]
for mid in parts:
    print(mid)
PY
else
  resp="$(curl -fsS --noproxy "*" --max-time 60 \
    "${DAEMON_URL}/api/v1/openrouter/models?min_total=${MIN_TOTAL}&max_total=${MAX_TOTAL}&require_tools=1&limit=${LIMIT}&refresh=1")"

  python3 - <<PY >"${candidates_file}"
import json
obj = json.loads(r'''${resp}''')
models = obj.get("models") or []
rec = obj.get("recommended_model") or ""
ids = []
if rec:
    ids.append(rec)
for m in models:
    mid = m.get("id")
    if not mid:
        continue
    if mid not in ids:
        ids.append(mid)
if not ids:
    raise SystemExit("no candidates returned from /openrouter/models")
for mid in ids[: int(${MAX_MODELS})]:
    print(mid)
PY
fi

run_stream_check() {
  local model="$1"
  local mode="$2"
  local session_id="openrouter_probe_${mode}_$(date +%s)_$RANDOM"

  if [[ "${mode}" == "tool" ]]; then
    payload="$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the calculator tool to compute 21*2. After the tool result, respond with exactly: 42",
  "session_id": "${session_id}",
  "tools": "basic",
  "stream_assistant": True,
  "model": "${model}",
  "force_tool": "calculator",
  "require_tool_call": True,
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": False,
  "trace": False
}))
PY
)"
  else
    payload="$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Write a short paragraph (at least 80 words) about streaming. End with exactly: END",
  "session_id": "${session_id}",
  "tools": "none",
  "stream_assistant": True,
  "model": "${model}",
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": False,
  "trace": False
}))
PY
)"
  fi

  job="$(curl -fsS --noproxy "*" --max-time 30 -H "Content-Type: application/json" -d "${payload}" "${DAEMON_URL}/api/v1/run_async")"
  job_id="$(python3 - <<PY
import json
obj = json.loads(r'''${job}''')
print(obj.get("job_id",""))
PY
)"
  if [[ -z "${job_id}" ]]; then
    echo "error"
    return
  fi

  job_id_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${job_id}")"
  stream="$(curl -fsS --noproxy "*" --max-time 120 "${DAEMON_URL}/api/v1/job/stream?job_id=${job_id_q}&cursor=0" | head -n 8000)"
  if [[ "${SAVE_STREAMS}" == "1" ]]; then
    safe_model="${model//\//_}"
    printf "%s" "${stream}" > "${stream_dir}/${safe_model}_${mode}.sse"
  fi

  result="$(STREAM="${stream}" MODE="${mode}" python3 - <<'PY'
import os, re, sys
s = os.environ["STREAM"]
mode = os.environ["MODE"]
lower = s.lower()
err_line = None
for line in re.findall(r"^data: (.*)$", s, flags=re.M):
    if '"type":"error"' in line or '"type": "error"' in line:
        err_line = line
        break
if '"http_status":401' in s or '"status":401' in s or "user not found" in lower:
    print("auth_error")
    raise SystemExit(0)
if ('"http_status":429' in s or '"status":429' in s) and ("overloaded" in lower or "rate" in lower):
    print("rate_limited")
    raise SystemExit(0)
if ('"type":"error"' in s or '"type": "error"' in s) and ("unsupported" in lower or "not support" in lower or "not_supported" in lower):
    if mode == "tool" and "tool" in lower:
        print("unsupported")
        raise SystemExit(0)
    if mode == "assistant" and "stream" in lower:
        print("unsupported")
        raise SystemExit(0)
if 'event: job_done' not in s:
    print("no_job_done")
    raise SystemExit(0)
if mode == "tool":
    ok = any('"type":"tool_call"' in line or '"type": "tool_call"' in line for line in re.findall(r"^data: (.*)$", s, flags=re.M))
    if ok:
        print("ok")
    elif err_line:
        print("error")
    else:
        print("no_tool_call")
else:
    ok = any('"type":"assistant_delta"' in line or '"type": "assistant_delta"' in line for line in re.findall(r"^data: (.*)$", s, flags=re.M))
    if ok:
        print("ok")
    elif err_line:
        print("error")
    else:
        print("no_delta")
PY
)"

  curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${session_id}"))
PY
)" --max-time 10 >/dev/null || true

  echo "${result}"
}

results_tmp="${LOG_DIR}/results_$(date +%Y%m%d_%H%M%S).jsonl"
: >"${results_tmp}"

while IFS= read -r model; do
  [[ -z "${model}" ]] && continue
  echo "[probe] ${model}"
  assistant_status="$(run_stream_check "${model}" "assistant")"
  tool_status="$(run_stream_check "${model}" "tool")"
  if [[ "${assistant_status}" == "auth_error" || "${tool_status}" == "auth_error" ]]; then
    echo "[probe] WARN: auth error for ${model} (skipping)" >&2
  fi
  python3 - <<PY >>"${results_tmp}"
import json
print(json.dumps({
  "id": "${model}",
  "assistant_stream": "${assistant_status}",
  "tool_stream": "${tool_status}"
}))
PY
  if [[ "${assistant_status}" == "ok" && "${tool_status}" == "ok" ]]; then
    echo "[probe] OK ${model}"
  fi
done <"${candidates_file}"

python3 - <<PY >"${result_file}"
import json
rows = []
with open("${results_tmp}") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        rows.append(json.loads(line))
out = {
  "base_url": "${BASE_URL}",
  "min_total": "${MIN_TOTAL}",
  "max_total": "${MAX_TOTAL}",
  "limit": int("${LIMIT}"),
  "max_models": int("${MAX_MODELS}"),
  "results": rows
}
print(json.dumps(out, indent=2))
PY

python3 - <<PY
import json
import sys
with open("${result_file}") as f:
    obj = json.load(f)
rows = obj.get("results", [])
assistant_oks = [r["id"] for r in rows if r.get("assistant_stream") == "ok"]
tool_oks = [r["id"] for r in rows if r.get("tool_stream") == "ok"]
both_oks = [r["id"] for r in rows if r.get("assistant_stream") == "ok" and r.get("tool_stream") == "ok"]
auth_only = bool(rows) and all(r.get("assistant_stream") == "auth_error" and r.get("tool_stream") == "auth_error" for r in rows)
print("OK models (assistant + tool-call streaming):")
for mid in both_oks:
    print(f"- {mid}")
print("OK models (assistant streaming):")
for mid in assistant_oks:
    print(f"- {mid}")
print("OK models (tool-call streaming):")
for mid in tool_oks:
    print(f"- {mid}")
if not assistant_oks or not tool_oks:
    if auth_only:
        print("SKIP: OpenRouter auth errors across all candidates", file=sys.stderr)
        raise SystemExit(77)
    raise SystemExit(1)
PY

echo "[probe] results: ${result_file}"

if [[ "${WRITE_PINS}" == "1" ]]; then
  mkdir -p "$(dirname "${PINS_PATH}")"
  python3 - <<PY
import json
from datetime import datetime, timezone
with open("${result_file}") as f:
    obj = json.load(f)
rows = obj.get("results", [])
assistant_oks = [r["id"] for r in rows if r.get("assistant_stream") == "ok"]
tool_oks = [r["id"] for r in rows if r.get("tool_stream") == "ok"]
both_oks = [r["id"] for r in rows if r.get("assistant_stream") == "ok" and r.get("tool_stream") == "ok"]
if not assistant_oks or not tool_oks:
    raise SystemExit("missing assistant/tool streaming models; refusing to write pins")
pins = {
  "generated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
  "base_url": obj.get("base_url"),
  "min_total": obj.get("min_total"),
  "max_total": obj.get("max_total"),
  "limit": obj.get("limit"),
  "max_models": obj.get("max_models"),
  "assistant_model": assistant_oks[0],
  "tool_model": tool_oks[0],
  "ok_models_both": both_oks,
  "ok_models_assistant": assistant_oks,
  "ok_models_tool": tool_oks,
}
with open("${PINS_PATH}", "w", encoding="utf-8") as f:
    json.dump(pins, f, indent=2, sort_keys=False)
PY
  echo "[probe] wrote pins: ${PINS_PATH}"
fi
