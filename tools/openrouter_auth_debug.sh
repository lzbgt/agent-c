#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT}/tests"

# shellcheck source=tests/test_keys.sh
source "${SCRIPT_DIR}/test_keys.sh"
curl() {
  agent_test_curl "$@"
}

agent_test_setup_proxy_env
agent_test_load_openrouter_headers_if_unset || true
OPENROUTER_KEY="$(agent_test_get_key openrouter 2>/dev/null || true)"
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "SKIP: OPENROUTER_API_KEY not set and not found in .not_in_repo, project.local.md, or ~/.env" >&2
  exit 77
fi

BASE_URL="${OPENROUTER_API_BASE:-https://openrouter.ai/api/v1}"
SOURCE_HINT="$(agent_test_get_key_source openrouter 2>/dev/null || true)"
MODEL_OVERRIDE="${AGENT_TEST_OPENROUTER_MODEL:-}"
HEADERS=(-H "Authorization: Bearer ${OPENROUTER_KEY}")
if [[ -n "${OPENROUTER_HTTP_REFERER:-}" ]]; then
  HEADERS+=(-H "HTTP-Referer: ${OPENROUTER_HTTP_REFERER}")
fi
if [[ -n "${OPENROUTER_X_TITLE:-}" ]]; then
  HEADERS+=(-H "X-Title: ${OPENROUTER_X_TITLE}")
fi

echo "base_url=${BASE_URL}"
if [[ -n "${SOURCE_HINT}" ]]; then
  echo "key_source=${SOURCE_HINT}"
fi
if [[ -n "${OPENROUTER_HTTP_REFERER:-}" ]]; then
  echo "http_referer_set=1"
else
  echo "http_referer_set=0"
fi
if [[ -n "${OPENROUTER_X_TITLE:-}" ]]; then
  echo "x_title_set=1"
else
  echo "x_title_set=0"
fi
if [[ -n "${MODEL_OVERRIDE}" ]]; then
  echo "model_override=${MODEL_OVERRIDE}"
fi
PINS_SOURCE="default"
if [[ -n "${AGENT_OPENROUTER_STREAM_PINS:-}" ]]; then
  PINS_PATH="${AGENT_OPENROUTER_STREAM_PINS}"
  PINS_SOURCE="AGENT_OPENROUTER_STREAM_PINS"
elif [[ -n "${OPENROUTER_STREAM_PINS_PATH:-}" ]]; then
  PINS_PATH="${OPENROUTER_STREAM_PINS_PATH}"
  PINS_SOURCE="OPENROUTER_STREAM_PINS_PATH"
else
  PINS_PATH="${ROOT}/ref/openrouter/streaming_pins.json"
fi
echo "stream_pins_source=${PINS_SOURCE}"
echo "stream_pins_path=${PINS_PATH}"
if [[ -f "${PINS_PATH}" ]]; then
  echo "stream_pins_present=1"
else
  echo "stream_pins_present=0"
  echo "stream_pins_hint=run tools/probe_openrouter_stream_models.sh with OPENROUTER_STREAM_PROBE_WRITE_PINS=1"
fi
if [[ -f "${PINS_PATH}" ]]; then
  python3 - <<PY 2>/dev/null || true
import json
path = "${PINS_PATH}"
try:
    with open(path, "r", encoding="utf-8") as f:
        obj = json.load(f)
except Exception:
    raise SystemExit(0)
assistant = str(obj.get("assistant_model") or "").strip()
tool = str(obj.get("tool_model") or "").strip()
assistant_count = len(obj.get("ok_models_assistant") or [])
tool_count = len(obj.get("ok_models_tool") or [])
both_count = len(obj.get("ok_models_both") or [])
has_any = bool(assistant or tool or assistant_count or tool_count or both_count)
if assistant:
    print(f"pins_assistant_model={assistant}")
if tool:
    print(f"pins_tool_model={tool}")
if assistant_count:
    print(f"pins_ok_models_assistant={assistant_count}")
if tool_count:
    print(f"pins_ok_models_tool={tool_count}")
if both_count:
    print(f"pins_ok_models_both={both_count}")
print(f"pins_empty={'0' if has_any else '1'}")
if not has_any:
    print("stream_pins_hint=run tools/probe_openrouter_stream_models.sh with OPENROUTER_STREAM_PROBE_WRITE_PINS=1")
PY
fi

resp="$(curl -sS --noproxy "*" -w "\n%{http_code}" \
  "${HEADERS[@]}" \
  "${BASE_URL}/models" || true)"
status="${resp##*$'\n'}"
body="${resp%$'\n'*}"
if [[ "${status}" == "401" || "${status}" == "403" ]]; then
  err_detail="$(printf "%s" "${body}" | python3 - <<'PY' 2>/dev/null || true
import json, sys
raw = sys.stdin.read()
try:
    obj = json.loads(raw)
except Exception:
    raise SystemExit
err = obj.get("error") or {}
msg = err.get("message") or ""
code = err.get("code") or ""
out = ""
if code:
    out = f"{code}"
if msg:
    out = f"{out}: {msg}" if out else msg
if out:
    print(out)
PY
)"
  echo "models_status=${status}" >&2
  if [[ -n "${err_detail}" ]]; then
    echo "models_error=${err_detail}" >&2
  fi
  echo "SKIP: OpenRouter auth failed on /models" >&2
  exit 77
fi
if [[ "${status}" -ge 400 || "${status}" == "000" ]]; then
  echo "models_status=${status}" >&2
  echo "SKIP: OpenRouter /models failed" >&2
  exit 77
fi

MODELS_BODY="${body}" MODEL_OVERRIDE="${MODEL_OVERRIDE}" python3 - <<'PY'
import json, os, sys
raw = os.environ.get("MODELS_BODY", "")
override = os.environ.get("MODEL_OVERRIDE") or ""
try:
    obj = json.loads(raw)
except Exception:
    print("models_parse=error")
    sys.exit(0)
models = obj.get("data") or obj.get("models") or []
rec = obj.get("recommended_model") or ""
first_text = ""
first_text_tools = ""
for m in models:
    arch = m.get("architecture") or {}
    inputs = arch.get("input_modalities") or []
    sp = m.get("supported_parameters") or []
    mid = m.get("id") or m.get("name") or ""
    if not mid:
        continue
    if not first_text and "text" in inputs:
        first_text = mid
    if not first_text_tools and "text" in inputs and "tools" in sp:
        first_text_tools = mid
    if first_text and first_text_tools:
        break

def pick():
    if override:
        return override, "override"
    if isinstance(rec, str) and rec:
        return rec, "recommended_model"
    if first_text_tools:
        return first_text_tools, "first_text_tools"
    if first_text:
        return first_text, "first_text"
    return "", "none"

model, reason = pick()
print(f"models_count={len(models)}")
print(f"recommended_model={rec or ''}")
print(f"first_text_model={first_text}")
print(f"first_text_tools_model={first_text_tools}")
print(f"chosen_model={model}")
print(f"chosen_reason={reason}")
PY

CHOSEN="$(MODELS_BODY="${body}" MODEL_OVERRIDE="${MODEL_OVERRIDE}" python3 - <<'PY'
import json, os
raw = os.environ.get("MODELS_BODY", "")
override = os.environ.get("MODEL_OVERRIDE") or ""
try:
    obj = json.loads(raw)
except Exception:
    print("")
    raise SystemExit
models = obj.get("data") or obj.get("models") or []
rec = obj.get("recommended_model") or ""
first_text = ""
first_text_tools = ""
for m in models:
    arch = m.get("architecture") or {}
    inputs = arch.get("input_modalities") or []
    sp = m.get("supported_parameters") or []
    mid = m.get("id") or m.get("name") or ""
    if not mid:
        continue
    if not first_text and "text" in inputs:
        first_text = mid
    if not first_text_tools and "text" in inputs and "tools" in sp:
        first_text_tools = mid
    if first_text and first_text_tools:
        break
if override:
    print(override)
elif isinstance(rec, str) and rec:
    print(rec)
elif first_text_tools:
    print(first_text_tools)
elif first_text:
    print(first_text)
PY
)"

if [[ -z "${CHOSEN}" ]]; then
  echo "SKIP: no candidate model for chat preflight" >&2
  exit 77
fi

chat_payload="$(python3 - <<PY
import json
print(json.dumps({
  "model": "${CHOSEN}",
  "messages": [{"role":"user","content":"ping"}],
  "max_tokens": 1
}))
PY
)"

CHAT_HEADERS=("${HEADERS[@]}" -H "Content-Type: application/json")
chat_resp="$(curl -sS --noproxy "*" -w "\n%{http_code}" \
  "${CHAT_HEADERS[@]}" \
  -d "${chat_payload}" \
  "${BASE_URL}/chat/completions" || true)"
chat_status="${chat_resp##*$'\n'}"
chat_body="${chat_resp%$'\n'*}"
echo "chat_status=${chat_status}"
CHAT_BODY="${chat_body}" python3 - <<'PY'
import json, os, sys
raw = os.environ.get("CHAT_BODY", "")
try:
    obj = json.loads(raw)
except Exception:
    print("chat_error=parse_error")
    sys.exit(0)
err = obj.get("error") or {}
msg = err.get("message") or ""
code = err.get("code") or ""
if msg or code:
    print(f"chat_error_code={code}")
    print(f"chat_error_message={msg}")
PY

if [[ "${chat_status}" -ge 400 || "${chat_status}" == "000" ]]; then
  exit 77
fi
exit 0
