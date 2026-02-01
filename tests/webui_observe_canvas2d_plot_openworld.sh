#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

ROOT="$(agentd_smoke_project_root)"
LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

AGENTD_BIN="${1:-${ROOT}/build/agentd}"
if [[ ! -x "${AGENTD_BIN}" ]]; then
  echo "agentd binary not found/executable: ${AGENTD_BIN}" >&2
  exit 2
fi

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
PORT_UI="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
UI_URL="http://${HOST}:${PORT_UI}"

OUT_DIR="${ROOT}/out/e2e_observe_canvas2d_plot_$(date +%s)_$RANDOM"
mkdir -p "${OUT_DIR}"

UI_KILL_PGID="0"

cleanup() {
  agentd_smoke_stop || true

  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 50); do
      if ! kill -0 "${STUB_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${STUB_PID}" >/dev/null 2>&1; then
      kill -KILL "${STUB_PID}" >/dev/null 2>&1 || true
    fi
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi

  if [[ -n "${UI_PID:-}" ]]; then
    if [[ "${UI_KILL_PGID:-0}" == "1" ]]; then
      kill -TERM -- "-${UI_PID}" >/dev/null 2>&1 || true
    else
      kill -TERM "${UI_PID}" >/dev/null 2>&1 || true
    fi
    for _ in $(seq 1 50); do
      if ! kill -0 "${UI_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${UI_PID}" >/dev/null 2>&1; then
      if [[ "${UI_KILL_PGID:-0}" == "1" ]]; then
        kill -KILL -- "-${UI_PID}" >/dev/null 2>&1 || true
      else
        kill -KILL "${UI_PID}" >/dev/null 2>&1 || true
      fi
    fi
    wait "${UI_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

echo "[e2e] OUT_DIR=${OUT_DIR}"
echo "[e2e] STUB_BASE=${STUB_BASE}"
echo "[e2e] DAEMON_URL=${DAEMON_URL}"
echo "[e2e] UI_URL=${UI_URL}"

# OpenAI-compatible stub provider:
# - Validates the user prompt
# - Writes a canvas2d scene entity where props.script is a *function expression* (common model output)
#   so the WebUI must detect & invoke it.
python3 -u - <<PY > "${OUT_DIR}/stub.stdout.log" 2> "${OUT_DIR}/stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

PROMPT = "nice, present a sine fun with time advances plot for me"

def last_user(messages):
  for m in reversed(messages or []):
    if isinstance(m, dict) and m.get("role") == "user":
      c = m.get("content")
      if isinstance(c, str):
        return c.strip()
  return ""

def any_system_contains(messages, needle):
  for m in messages or []:
    if not isinstance(m, dict) or m.get("role") != "system":
      continue
    c = m.get("content")
    if isinstance(c, str) and needle in c:
      return True
  return False

def has_any_tool_result(messages):
  return any(isinstance(m, dict) and m.get("role") == "tool" for m in (messages or []))

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return

    length = int(self.headers.get("content-length") or "0")
    raw = self.rfile.read(length) if length > 0 else b"{}"
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      req = {}

    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    # Sanity: ensure the daemon injects the webui profile guidance for durable scene usage.
    if not any_system_contains(messages, "CLIENT_PROFILE=webui"):
      self.send_response(500)
      self.end_headers()
      self.wfile.write(b"missing CLIENT_PROFILE=webui")
      return
    if not any_system_contains(messages, "scene_apply"):
      self.send_response(500)
      self.end_headers()
      self.wfile.write(b"missing scene_apply guidance")
      return

    if has_any_tool_result(messages):
      body = {
        "id": "cmpl_stub_done",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "done"}, "finish_reason": "stop"}
        ],
      }
    else:
      pu = last_user(messages)
      if pu != PROMPT:
        body = {
          "id": "cmpl_stub_err",
          "object": "chat.completion",
          "created": 0,
          "model": "stub",
          "choices": [
            {"index": 0, "message": {"role": "assistant", "content": f"expected exact prompt {PROMPT!r} got: {pu!r}"}, "finish_reason": "stop"}
          ],
        }
      else:
        # Function-expression form (regression target): WebUI must invoke this.
        script = """
async (api, args) => {
  const canvas = api.root;
  const ctx = canvas.getContext('2d');
  const width = canvas.width;
  const height = canvas.height;
  ctx.fillStyle = '#1a1a2e';
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = '#f72585';
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (let x = 0; x <= width; x += 2) {
    const t = (x / width) * Math.PI * 4;
    const y = height / 2 + Math.sin(t) * (height * 0.25);
    if (x === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
  return () => { /* no-op cleanup */ };
}
        """.strip()

        body = {
          "id": "cmpl_stub_1",
          "object": "chat.completion",
          "created": 0,
          "model": "stub",
          "choices": [
            {
              "index": 0,
              "message": {
                "role": "assistant",
                "content": "",
                "tool_calls": [
                  {"id": "call_scene", "type": "function", "function": {"name": "scene_apply", "arguments": json.dumps({"ops": [{"op":"create","id":"sine-wave","entity_kind":"canvas2d","title":"Animated Sine Wave","props":{"width":800,"height":400,"script": script}}]})}},
                ],
              },
              "finish_reason": "tool_calls",
            }
          ],
        }

    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

# Start daemon with stub provider as default base_url/model so the WebUI run is deterministic.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "webui_observe_canvas2d_plot_openworld" \
  --tools host \
  --tools-root "${ROOT}" \
  --yolo \
  --base-url "${STUB_BASE}" \
  --api-key "dummy" \
  --model "stub"

agentd_smoke_wait_health "${DAEMON_URL}"

# Build + serve WebUI (preview is closer to production than dev server).
npm --prefix "${ROOT}/ui" run build > "${OUT_DIR}/ui_build.log" 2>&1

if command -v setsid >/dev/null 2>&1; then
  setsid npm --prefix "${ROOT}/ui" run preview -- --host "${HOST}" --port "${PORT_UI}" \
    > "${OUT_DIR}/ui_preview.stdout.log" 2> "${OUT_DIR}/ui_preview.stderr.log" &
  UI_KILL_PGID="1"
else
  npm --prefix "${ROOT}/ui" run preview -- --host "${HOST}" --port "${PORT_UI}" \
    > "${OUT_DIR}/ui_preview.stdout.log" 2> "${OUT_DIR}/ui_preview.stderr.log" &
fi
UI_PID=$!

# Wait for UI to respond.
for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" --max-time 2 "${UI_URL}/" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

echo "[e2e] running playwright open-world test"
(
  cd "${ROOT}/ui" && \
  AGENT_E2E_UI_BASE_URL="${UI_URL}" \
  AGENT_E2E_AGENTD_BASE_URL="${DAEMON_URL}" \
  AGENT_E2E_PROVIDER_BASE_URL="${STUB_BASE}" \
  AGENT_E2E_PROVIDER_MODEL="stub" \
  AGENT_E2E_PROVIDER_API_KEY="dummy" \
  AGENT_E2E_OUT_DIR="${OUT_DIR}/pw" \
  npm run e2e -- observe_canvas2d_plot.spec.ts \
  > "${OUT_DIR}/playwright.stdout.log" 2> "${OUT_DIR}/playwright.stderr.log"
)

echo "[e2e] OK"

