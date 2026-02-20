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

OUT_DIR="${ROOT}/out/e2e_observe_voice_hello_$(date +%s)_$RANDOM"
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
# - Validates the user prompt is exactly "say hello in voice to me"
# - Calls host tools to produce out/hello.mp3
# - Registers artifact
# - Writes a durable dom Scene entity via scene_apply (refresh-proof player)
python3 -u - <<PY > "${OUT_DIR}/stub.stdout.log" 2> "${OUT_DIR}/stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

ART_AIFF = "out/hello.aiff"
ART_MP3 = "out/hello.mp3"

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
      if pu != "say hello in voice to me":
        body = {
          "id": "cmpl_stub_err",
          "object": "chat.completion",
          "created": 0,
          "model": "stub",
          "choices": [
            {"index": 0, "message": {"role": "assistant", "content": f"expected exact prompt 'say hello in voice to me' got: {pu!r}"}, "finish_reason": "stop"}
          ],
        }
      else:
        player_html = (
          "<div style='display:flex;flex-direction:column;gap:8px'>"
          "  <div style='font:12px ui-sans-serif;color:#e5e7eb'>Voice player</div>"
          "  <audio id='voice-audio' controls style='width:100%'></audio>"
          "  <div id='voice-status' style='font:11px ui-sans-serif;color:#94a3b8'></div>"
          "</div>"
        )
        player_script = f"""
const audio = api.root.querySelector('#voice-audio');
const status = api.root.querySelector('#voice-status');
if (!audio) throw new Error('missing #voice-audio');
status && (status.textContent = 'loading audio...');
const url = await api.artifact.url('{ART_MP3}');
audio.src = url;
audio.autoplay = true;
audio.addEventListener('play', () => status && (status.textContent = 'playing'));
audio.addEventListener('pause', () => status && (status.textContent = 'paused'));
audio.addEventListener('ended', () => status && (status.textContent = 'ended'));
audio.addEventListener('error', () => status && (status.textContent = 'error'));
try {{
  await audio.play();
  status && (status.textContent = 'playing');
}} catch (e) {{
  status && (status.textContent = 'autoplay blocked; click play');
}}
return () => {{
  try {{ audio.pause(); }} catch (e) {{}}
}};
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
                  {"id": "call_mkdir", "type": "function", "function": {"name": "shell_exec", "arguments": json.dumps({"cmd": "mkdir -p out"})}},
                  {"id": "call_say", "type": "function", "function": {"name": "proc_exec", "arguments": json.dumps({"argv": ["say", "-v", "Alex", "-o", ART_AIFF, "hello"]})}},
                  {"id": "call_ffmpeg", "type": "function", "function": {"name": "proc_exec", "arguments": json.dumps({"argv": ["ffmpeg", "-y", "-i", ART_AIFF, "-codec:a", "libmp3lame", "-qscale:a", "2", ART_MP3]})}},
                  {"id": "call_art", "type": "function", "function": {"name": "artifact_register", "arguments": json.dumps({"path": ART_MP3, "kind": "audio", "title": "hello in voice"})}},
                  {"id": "call_scene", "type": "function", "function": {"name": "scene_apply", "arguments": json.dumps({"ops": [{"op": "create", "id": "voice-player", "entity_kind": "dom", "title": "Voice Player", "props": {"html": player_html, "script": player_script}}]})}},
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

# Start daemon with stub provider as default base_url/model so the WebUI run is fully deterministic.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "webui_observe_voice_hello_openworld" \
  --tools host \
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
  AGENT_E2E_REQUIRE_VOICE=1 \
  AGENT_E2E_OUT_DIR="${OUT_DIR}/pw" \
  npm run e2e -- observe_voice_hello.spec.ts \
  > "${OUT_DIR}/playwright.stdout.log" 2> "${OUT_DIR}/playwright.stderr.log"
)

echo "[e2e] OK"
