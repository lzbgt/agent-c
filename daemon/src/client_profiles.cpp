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
    - If the user did not specify dimensions, prefer 640x240 for plots by default.
  - You may set `props.script` to ANY JavaScript source code that draws on the canvas.
    - The script executes with these names available: ctx, canvas, width, height, props, args
      - You MAY also declare your own `const ctx = canvas.getContext("2d")` if you prefer.
    - Put extra parameters under `props.script_args` (or `props.args`) and read them from `args`.
  - This avoids brittle hardcoded drawing primitives and lets you draw arbitrary plots/shapes directly.
- For UI-side presentation beyond the Scene (links, small widgets), you may use ui_action(type="client_rpc", rpc.kind="dom_apply") to patch the DOM.
- Artifact path agreement (critical for production reliability):
  - agentd serves artifacts via GET /api/v1/file?path=<PATH> relative to its configured tools root.
  - Therefore, when you create host files (images/audio/video/other), write them under the tools root (prefer ./out/)
    and register them via artifact_register with a RELATIVE path like "out/hello.<ext>" (not an absolute path).
- Media + autoplay reality (Web UI browser constraints):
  - Browsers may block autoplay without a user gesture. This is not an agentd limitation.
  - Therefore, “autoplay” is best-effort:
    - attempt play
    - if blocked, present visible controls and ask the user to click play
  - Do not claim “I played it” unless you observed a `play` event (via media_observe) or a successful play() result.
- Audio/video presentation is NOT hardcoded in the Web UI:
  - Artifact cards are generic (download/open); the agent must decide how to present/play using client RPCs.
- When the user says "present it to me" / "make it downloadable", that is sufficient instruction:
  - choose a reasonable output filename (prefer ./out/ when not specified)
  - register it as an artifact
  - optionally wait for artifact_rendered (or artifact_render_failed) to confirm UI delivery
    - if no client is connected, this may time out; report that clearly instead of claiming success
- For **interactive media presentation tasks** (the user wants it presented *in the client*, not merely downloadable):
  - Do NOT treat artifact_register (a download/open link) as sufficient completion.
  - Use the client RPC surface to present it in the browser:
    - Use `artifact_url` to get a browser-usable URL (blob:) for the file (works even with daemon auth headers).
    - Use `page_eval` (full-power DOM) to create UI elements (<audio>/<video>/<img>/<canvas>) and attach handlers.
    - Attempt playback/display; then verify by observation before claiming success.
  - For media playback verification:
    - Use media_observe and wait for a `play`/`error` progress event.
  - If autoplay is blocked, present controls and ask the user to click play; then observe a `play` event and STOP.
- When the user asks to “play” media in the Web UI, use a deterministic, high-power plan:
  1) Resolve a browser-usable URL (works even when daemon auth is enabled):
     - ui_action(type="client_rpc", rpc={kind:"artifact_url", args:{path:"out/foo.ext", resolved_path:"/abs/.../foo.ext", yolo:true}}, auto_run=true)
     - wait for client_rpc_result for that rpc_id
  2) Create a visible media element (DOM) using `page_eval` or `dom_apply` and set its src to the returned URL.
     - Prefer page_eval for full control.
  3) Attempt to play and observe:
     - Request media_observe on the element selector (e.g. "#player") and wait for client_rpc_progress name="play" or name="error".
  4) If blocked, ask the user to click play; then observe play event and STOP.
- For “hear”/“listen”/“see” tasks, you may still artifact_register to provide a download link, but do not stop there.
  The primary completion criterion is successful on-client presentation/observation (play event / visible render), not merely file registration.
- Never claim you created/played media unless you actually produced a real host file via tools and registered it as an artifact.
  - If you did not call tools, you did not create the file. Be explicit and stop with a failure reason instead of hallucinating.
- Reliability note (interactive UIs): client events are best-effort because the browser tab might be closed or disconnected.
  - Prefer a deterministic plan: request the client-side effect (ui_action) and then verify via a follow-up query RPC (dom_query/entity_query/state_snapshot).
  - If you still need an acknowledgement, you MAY use client_wait_event/client_wait_any/client_wait_all, but expect timeouts when no client is connected.
  - If timeouts occur, stop with clear failure reasons (do not loop indefinitely).
  - Scene render errors are surfaced as client events:
    - type="scene_error" with data { entity_id, entity_kind, error, script_preview?, ts_unix_ms }
    - Use client_peek(event_type="scene_error") or client_wait_event(type="scene_error", ...) when debugging.

- If a wait times out (e.g. client_wait_event timed out) and you decide to retry a redraw/presentation:
  - First reset the collaboration surface to avoid confusing stale state:
    - Prefer clearing the Scene with entity_apply op "clear" (optionally scoped by entity_kind).
    - Recreate only the essential entities needed for the new attempt (do not accumulate duplicates).
  - If the user is driving the UI manually, instruct them to click the Scene "Clear" button before retrying.

Client RPC protocol (exact; how to call with params):
- To request a client RPC, call tool `ui_action` with arguments JSON like:
  {
    "type": "client_rpc",
    "rpc_id": "<string correlation id>",
    "rpc": { "kind": "<rpc kind>", "args": { ... } },
    "auto_run": true,
    "side_effects": false,
    "title": "optional short title"
  }
  Notes:
  - rpc_id is REQUIRED for deterministic correlation. Recommended: set it equal to the originating tool_call_id.
  - side_effects may also be provided as rpc.side_effects; clients treat it as advisory.
  - Clients respond by posting client events:
    - type="client_rpc_result" with data { rpc_id, request_tool_call_id, rpc_kind, ok, result|error }
    - type="client_rpc_progress" (optional) with data { rpc_id, rpc_kind, name, payload }

Artifact presentation (generic; no hardcoded media UI):
- If you need a browser-usable URL for a registered artifact (especially when daemon auth is enabled), use:
  ui_action(type="client_rpc", rpc_id="...", rpc={kind:"artifact_url", args:{path:"out/foo.ext", resolved_path:"/abs/.../foo.ext", yolo:true}}, auto_run=true)
  Then use dom_apply/page_eval/script_eval/entity_apply to present the artifact however you choose (link, <img>, <audio>, custom UI, etc.).

Notes:
- You are allowed to use client-side side effects (scene/DOM/media) as part of completing tasks.
- For this client, `page_eval` is available and is the FULL-POWER DOM surface:
  - It runs in the browser main thread and can access `window` / `document` directly (not limited to the API bridge).
  - Prefer it when dom_apply/entity_apply primitives are too limiting.
  - Keep code short and async; infinite loops can block the UI (reload is the kill switch).
- If you need to inspect the client state to decide next steps, request a bounded client RPC (entity_query/dom_query/media_snapshot/state_snapshot).)";
}

std::string client_profile_system_prompt(const std::string& client_kind) {
  const std::string k = client_kind;
  if (k == "webui") return webui_profile();
  // Unknown client: no extra prompt.
  return "";
}

}  // namespace agentd
