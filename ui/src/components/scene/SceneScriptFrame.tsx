import React from "react";
import type { ApiAuth } from "../../api";
import type { SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import { createSceneArtifactUrlResolver, getSceneCleanup, getSceneDaemonContext, isSceneAutoplayUnlocked, makeSceneConsole, safeObject, safeString, safeToString } from "./sceneViewUtils";

type Props = {
  entity: SceneEntity;
  kind: "canvas" | "dom";
  script: string;
  html?: string;
  width?: number;
  height?: number;
  baseUrl?: string;
  yolo?: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  allowAutoplay?: boolean;
  onScriptError?: (args: SceneScriptErrorArgs) => void;
};

// A separate browsing context gives each presentation its own document,
// keyboard events, animation clock and timers. Existing trusted script/API
// capabilities are retained; this is not a boundary against hostile code.
export default function SceneScriptFrame(props: Props) {
  const ref = React.useRef<HTMLIFrameElement>(null);
  const latest = React.useRef(props);
  latest.current = props;
  const cleanup = React.useRef<(() => void) | null>(null);
  const [frameHeight, setFrameHeight] = React.useState(props.height || 240);
  const propsJson = React.useMemo(() => JSON.stringify(safeObject(props.entity.props)), [props.entity.props]);
  const authJson = JSON.stringify(props.daemonAuth || {});
  // Compare execution inputs by value, never transient arrays, callbacks or
  // auth wrapper identities created by unrelated composer renders/polling.
  const document = React.useMemo(() => {
    const id = crypto.randomUUID();
    return { id, source: `<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;min-width:0;font:14px system-ui}body{background:${props.kind === "canvas" ? "#101416" : "white"};color:${props.kind === "canvas" ? "#edf1f2" : "#111827"}}canvas{display:block;max-width:100%;height:auto}:focus-visible{outline:2px solid #9be5cb;outline-offset:-2px}#scene-root{min-width:0;overflow-wrap:anywhere}</style></head><body data-scene-runtime="${id}"></body></html>` };
  }, [props.entity.id, props.kind, props.script, props.html, props.width, props.height, props.baseUrl, props.yolo, props.sessionId, propsJson, authJson]);
  const documentRef = React.useRef(document.id);
  documentRef.current = document.id;

  const dispose = React.useCallback(() => {
    cleanup.current?.();
    cleanup.current = null;
  }, []);
  React.useEffect(() => () => dispose(), [document.id, dispose]);

  const initialize = React.useCallback(() => {
    const frame = ref.current;
    const view = frame?.contentWindow as (Window & typeof globalThis) | null;
    const doc = frame?.contentDocument;
    if (!view || !doc || doc.body.dataset.sceneRuntime !== documentRef.current) return;
    dispose();
    const p = latest.current;
    let active = true;
    let scriptCleanup: (() => void) | null = null;
    const urls = { current: [] as string[] };
    const root = doc.createElement(p.kind === "canvas" ? "canvas" : "div");
    root.id = "scene-root";
    doc.body.replaceChildren(root);
    let ctx: CanvasRenderingContext2D | null = null;
    if (root instanceof view.HTMLCanvasElement) {
      root.width = p.width || 640;
      root.height = p.height || 240;
      root.tabIndex = 0;
      root.setAttribute("aria-label", p.entity.title || "Interactive canvas");
      root.addEventListener("pointerdown", () => root.focus({ preventScroll: true }));
      ctx = root.getContext("2d");
    } else {
      root.innerHTML = p.html || "";
    }
    const observer = new view.ResizeObserver(() => {
      if (!active) return;
      const measured = Math.ceil(root.getBoundingClientRect().height);
      const next = Math.max(32, Math.min(4096, measured || p.height || 240));
      setFrameHeight((previous) => previous === next ? previous : next);
    });
    observer.observe(root);
    cleanup.current = () => {
      active = false;
      observer.disconnect();
      try { scriptCleanup?.(); } catch { /* presentation cleanup is best effort */ }
      urls.current.forEach((url) => URL.revokeObjectURL(url));
      urls.current = [];
    };
    if (!p.script.trim()) return;
    const artifactUrl = createSceneArtifactUrlResolver({ baseUrl: p.baseUrl || "", yolo: !!p.yolo, sessionId: p.sessionId, daemonAuth: p.daemonAuth, blobUrlsRef: urls });
    const api = {
      root, ctx, width: p.width || 640, height: p.height || 240, props: safeObject(p.entity.props),
      daemon: getSceneDaemonContext({ baseUrl: p.baseUrl || "", yolo: !!p.yolo, sessionId: p.sessionId, daemonAuth: p.daemonAuth }),
      artifact: { url: async (path: unknown) => {
        const url = await artifactUrl(path);
        if (!active) { URL.revokeObjectURL(url); throw new Error("Presentation closed"); }
        return url;
      } },
    };
    const script = p.script.trim();
    const functionExpression = /^(?:async\s+)?function\b|^(?:async\s*)?\(|^(?:async\s+)?[A-Za-z_$][\w$]*\s*=>/.test(script);
    const body = functionExpression
      ? `const fn = (${script}); return typeof fn === "function" ? await fn(api, args) : fn;`
      : p.kind === "canvas"
        ? `with ({api,args,ctx:api.ctx,canvas:api.root,width:api.width,height:api.height,props:api.props,artifact:api.artifact,daemon:api.daemon}) { ${script}\n }`
        : script;
    void (async () => {
      try {
        // Compile in the presentation's global realm, never the workspace's.
        const run = new view.Function("api", "args", "__console", `const console = __console; return (async function(){\n${body}\n})();`);
        const result = await run(api, safeObject(api.props.script_args ?? api.props.args), makeSceneConsole());
        const returnedCleanup = getSceneCleanup(result);
        if (!active) { try { returnedCleanup?.(); } catch { /* already detached */ } return; }
        scriptCleanup = returnedCleanup;
        if (p.allowAutoplay && isSceneAutoplayUnlocked()) {
          root.querySelectorAll<HTMLMediaElement>("audio,video").forEach((media) => { if (media.paused) void media.play().catch(() => {}); });
        }
      } catch (error) {
        if (!active) return;
        const message = safeToString(error) || "Presentation could not start";
        if (ctx) { ctx.fillStyle = "#fca5a5"; ctx.font = "12px system-ui"; ctx.fillText(message.slice(0, 200), 8, 16); }
        p.onScriptError?.({ entity_id: p.entity.id, entity_kind: p.entity.kind, error: message, stack_preview: safeString(safeObject(error).stack).slice(0, 1200), script_preview: p.script.slice(0, 800) });
      }
    })();
  }, [dispose]);

  return <iframe ref={ref} title={props.entity.title || "Presentation"} data-testid="scene-script-frame" className="mt-2 block w-full rounded-md border border-white/10" style={{ height: frameHeight }} sandbox="allow-scripts allow-same-origin" allow="autoplay" srcDoc={document.source} onLoad={initialize} />;
}
