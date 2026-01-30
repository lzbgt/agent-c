#include "client_profiles.h"

namespace agentd {

static std::string webui_profile() {
  // Keep this short and operational: it is part of the model context.
  // This profile is intentionally "powerful by default": the Web UI is treated as a collaboration surface
  // where side effects are expected (scene updates, DOM patches, media playback).
  return R"(CLIENT_PROFILE=webui (collaboration surface)

- The Web UI has a top "Scene" panel (a shared blackboard) plus a message timeline below it.
- When the user asks for visualization, prefer updating the Scene via ui_action(type="client_rpc", rpc.kind="entity_apply").
  - IMPORTANT: for this client, `entity_apply` expects `rpc.args.ops` as an array of operations:
    - create: { op:"create", id, entity_kind, title?, props? }
    - update: { op:"update", id, props }
    - action: { op:"action", id, action, args? }  (actions are data; not necessarily executed)
    - delete: { op:"delete", id }
    - clear:  { op:"clear", entity_kind? }
  - Use stable entity ids when helpful so the user can refer to them later.

- Canvas2D entities (entity_kind="canvas2d") are intentionally "powerful":
  - Set `props.width`/`props.height` to define the canvas dimensions.
  - You may set `props.script` to ANY JavaScript source code that draws on the canvas.
    - The script is executed with: (ctx, canvas, width, height, props, args)
    - Put any extra parameters under `props.script_args` (or `props.args`) and read them from `args`.
  - This avoids brittle hardcoded drawing primitives and lets you draw arbitrary plots/shapes directly.
- For UI-side presentation beyond the Scene (links, small widgets), you may use ui_action(type="client_rpc", rpc.kind="dom_apply") to patch the DOM.
- When you create host files (images/audio/video/other), register them via artifact_register so the client can download/preview them.
- Definition of Done (DoD): if you requested a client RPC or showed an artifact, wait for the corresponding acknowledgement event:
  - client_rpc_result (correlated by rpc_id), or artifact_rendered / ui_action_shown.
  - Use client_wait_event / client_wait_any / client_wait_all to wait deterministically.
  - After DoD is satisfied, STOP (do not loop or "retry" the same presentation indefinitely).

Notes:
- You are allowed to use client-side side effects (scene/DOM/media) as part of completing tasks.
- If you need to inspect the client state to decide next steps, request a bounded client RPC (entity_query/dom_query/media_snapshot/state_snapshot).)";
}

std::string client_profile_system_prompt(const std::string& client_kind) {
  const std::string k = client_kind;
  if (k == "webui") return webui_profile();
  // Unknown client: no extra prompt.
  return "";
}

}  // namespace agentd
