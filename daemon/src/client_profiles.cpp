#include "client_profiles.h"

namespace agentd {

static std::string webui_profile() {
  // Keep this short and operational: it is part of the model context.
  // This profile is intentionally "powerful by default" while steering toward durable, server-owned presentation.
  return R"(CLIENT_PROFILE=webui

- Web UI layout: a top **Scene** panel (shared blackboard) + a message timeline below.

- Prefer the **durable Scene** for presentation (server-owned, refresh-proof):
  - Use `scene_apply` with an `ops` array:
    - { op:"create", id, entity_kind, title?, props? }
    - { op:"update", id, props }
    - { op:"delete", id }
    - { op:"clear", entity_kind? }
  - Use stable ids when useful (e.g. "voice-player") so updates replace rather than duplicate.
  - Avoid `op:"clear"` unless the user explicitly asked to reset the Scene; prefer targeted updates/deletes.

- Scene entity kinds:
  - entity_kind="dom": arbitrary HTML + JS inside a dedicated container.
    - props.html inserts markup.
    - props.script runs as an **async function body** with (api, args):
      - api.root: container element
      - api.artifact.url(path): async; returns a Promise that resolves to a blob: URL (auth-safe). You MUST `await` it.
      - api.daemon.{base_url,yolo,auth_token}
      - args: props.script_args / props.args
    - The script may `await` and may `return () => { ... }` as cleanup.
  - entity_kind="canvas2d": 2D canvas; props.width/height + props.script (JS) with ctx/canvas/width/height/props/args.
    - For consistency with "dom", treat props.script as an async function body with (api, args):
      - api.root is the <canvas> element
      - api.ctx is the 2D rendering context
      - api.width/api.height are the canvas dimensions
      - You may `await` and `return () => { ... }` as cleanup.

- Artifacts (critical for reliability):
  - Write outputs under the tools root (prefer `./out/`).
  - Register artifacts with a **relative** path (e.g. "out/hello.mp3") using `artifact_register`.
  - For audio/video/images, prefer presenting via the Scene. (agentd may also add a basic player for audio artifacts.)
  - Do not create voice/audio presentations unless the user explicitly asked for voice/audio output.

- Client RPC is **ephemeral** (lost on refresh) but powerful. Use it only when needed:
  - Call `ui_action` with:
    { "type":"client_rpc", "rpc_id":"<id>", "rpc":{"kind":"<kind>","args":{...}}, "auto_run":true }
  - Results arrive as client events:
    - type="client_rpc_result" (data.rpc_id correlates)
    - type="client_rpc_progress" (optional)

- Avoid brittle “ack waits” for normal presentation:
  - `client_wait_event(type="artifact_rendered")` is best-effort and may time out if the user hasn’t opened the Scene.
  - Do not use it as a completion gate unless the user explicitly asked you to confirm rendering.
  - For debugging, Scene script failures are posted as client events type="scene_error" (use `client_peek`/`client_wait_event` on that type).)";
}

std::string client_profile_system_prompt(const std::string& client_kind) {
  const std::string k = client_kind;
  if (k == "webui") return webui_profile();
  // Unknown client: no extra prompt.
  return "";
}

}  // namespace agentd
