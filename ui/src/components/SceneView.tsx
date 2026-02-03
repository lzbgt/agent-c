import React from "react";

import ArtifactView from "./ArtifactView";
import { apiPostSessionUiEvent } from "../api";
import useLocalStorageState from "../hooks/useLocalStorageState";

type CanvasPoint = { x: number; y: number };

type DrawOp =
  | { op: "clear"; color?: string }
  | { op: "polyline"; points: CanvasPoint[]; strokeStyle?: string; lineWidth?: number }
  | { op: "line"; x1: number; y1: number; x2: number; y2: number; strokeStyle?: string; lineWidth?: number }
  | { op: "text"; x: number; y: number; text: string; fillStyle?: string; font?: string };

export type SceneEntity = {
  id: string;
  kind: string;
  title?: string;
  props?: any;
  created_ms?: number;
  updated_ms?: number;
};

function safeToString(v: any): string {
  try {
    if (typeof v === "string") return v;
    if (v && typeof v === "object" && typeof v.message === "string") return v.message;
    return String(v);
  } catch {
    return "";
  }
}

function isAutoplayNotAllowedLog(args: any[]): boolean {
  if (!Array.isArray(args) || args.length === 0) return false;
  const first = typeof args[0] === "string" ? args[0] : "";
  if (!/autoplay prevented/i.test(first)) return false;
  const rest = args.slice(1).map(safeToString).join(" ");
  const combined = `${first} ${rest}`.trim();
  return /NotAllowedError|user didn'?t interact|didn'?t interact|play\\(\\) failed/i.test(combined);
}

function makeSceneConsole(): Console {
  const real: any = (typeof globalThis !== "undefined" ? (globalThis as any).console : undefined) || {};
  const wrap = (fn: any) => {
    return (...args: any[]) => {
      // Browser policy: unmuted audio/video autoplay is often blocked until user interaction.
      // Scene scripts frequently log a noisy "Autoplay prevented: NotAllowedError..." message.
      // This is expected and not actionable for most users; suppress it to avoid "fake errors".
      if (isAutoplayNotAllowedLog(args)) return;
      try {
        if (typeof fn === "function") fn.apply(real, args);
      } catch {
        // ignore
      }
    };
  };
  // Provide a minimally complete console; keep other methods pass-through.
  return {
    log: wrap(real.log),
    info: wrap(real.info),
    warn: wrap(real.warn),
    error: wrap(real.error),
    debug: wrap(real.debug),
    trace: wrap(real.trace),
    // Some code checks console.assert/console.clear/etc; forward best-effort.
    assert: wrap(real.assert),
    clear: wrap(real.clear),
    count: wrap(real.count),
    countReset: wrap(real.countReset),
    dir: wrap(real.dir),
    dirxml: wrap(real.dirxml),
    group: wrap(real.group),
    groupCollapsed: wrap(real.groupCollapsed),
    groupEnd: wrap(real.groupEnd),
    table: wrap(real.table),
    time: wrap(real.time),
    timeEnd: wrap(real.timeEnd),
    timeLog: wrap(real.timeLog),
    timeStamp: wrap(real.timeStamp),
    profile: wrap(real.profile),
    profileEnd: wrap(real.profileEnd),
  } as any;
}

function toTestIdPart(v: string): string {
  // Keep Playwright selectors stable even if ids contain punctuation.
  // data-testid accepts any string, but normalizing avoids surprises.
  return v.replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 120) || "empty";
}

function clampInt(n: any, lo: number, hi: number, def: number): number {
  const v = typeof n === "number" ? n : Number(n);
  if (!Number.isFinite(v)) return def;
  return Math.min(Math.max(Math.trunc(v), lo), hi);
}

function safeString(v: any): string {
  return typeof v === "string" ? v : "";
}

function safeNumber(v: any, def: number): number {
  return typeof v === "number" && Number.isFinite(v) ? v : def;
}

function Canvas2DEntityView({
  entity,
  baseUrl,
  yolo,
  sessionId,
  daemonAuthToken,
  onScriptError,
}: {
  entity: SceneEntity;
  baseUrl?: string;
  yolo?: boolean;
  sessionId?: string;
  daemonAuthToken?: string;
  onScriptError?: (args: {
    entity_id: string;
    entity_kind: string;
    error: string;
    stack_preview?: string;
    script_preview?: string;
  }) => void;
}) {
  const canvasRef = React.useRef<HTMLCanvasElement | null>(null);
  const blobUrlsRef = React.useRef<string[]>([]);
  const cleanupRef = React.useRef<null | (() => void)>(null);
  const props = entity.props ?? {};
  const width = clampInt(props?.width, 64, 4096, 640);
  const height = clampInt(props?.height, 64, 4096, 240);
  const draw: DrawOp[] = Array.isArray(props?.draw) ? props.draw : [];
  const scriptRaw = safeString(props?.script || props?.js || props?.code);
  const script = scriptRaw.length > 100_000 ? scriptRaw.slice(0, 100_000) : scriptRaw;
  const scriptArgs = props?.script_args ?? props?.args ?? {};
  const scriptArgsJson = React.useMemo(() => {
    try {
      const s = JSON.stringify(scriptArgs ?? {});
      return s.length > 32_000 ? s.slice(0, 32_000) : s;
    } catch {
      return "{}";
    }
  }, [scriptArgs]);

  React.useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    // Resize resets state; do it first.
    canvas.width = width;
    canvas.height = height;

    // Default background clear.
    ctx.clearRect(0, 0, width, height);

    // Power mode: execute arbitrary JS provided by the agent (stored in entity props).
    // This is intentionally "no hardcoded ops": the agent can draw anything it wants, as long as it
    // uses the provided {ctx, canvas, width, height} and respects browser policies.
    if (script && script.trim().length > 0) {
      // Stop any previous animation/handlers for this entity.
      try {
        cleanupRef.current?.();
      } catch {
        // ignore
      }
      cleanupRef.current = null;
      blobUrlsRef.current.forEach((u) => {
        try {
          URL.revokeObjectURL(u);
        } catch {
          // ignore
        }
      });
      blobUrlsRef.current = [];

      const reportError = (e: any) => {
        const msg = String(e || "unknown error");
        const stack = e && typeof e === "object" && typeof e.stack === "string" ? e.stack : "";
        try {
          ctx.save();
          ctx.fillStyle = "#fca5a5";
          ctx.font = "12px ui-sans-serif";
          ctx.fillText(`canvas script error: ${msg.slice(0, 200)}`, 8, 16);
          ctx.restore();
        } catch {
          // ignore
        }
        try {
          onScriptError?.({
            entity_id: entity.id,
            entity_kind: entity.kind,
            error: msg,
            stack_preview: typeof stack === "string" ? stack.slice(0, 1200) : "",
            script_preview: script.slice(0, 800),
          });
        } catch {
          // ignore
        }
      };
      try {
        // Execute the agent-provided script in a "power mode" sandbox.
        //
        // Compatibility goals (important):
        // - Support scripts written as an async function BODY (so `await` + `return cleanup` work):
        //     // uses api.root/api.ctx
        //     ...
        //     return () => { ... }
        // - Also support scripts written as a FUNCTION EXPRESSION (common model output):
        //     async (api, args) => { ... return () => {...} }
        //
        // We intentionally avoid pre-declaring `ctx`/`canvas` as variables to prevent
        // collisions with scripts that naturally start with `const ctx = ...`.
        const authToken = safeString(daemonAuthToken).trim();
        const sid = safeString(sessionId).trim();
        const daemon = { base_url: baseUrl || "", yolo: !!yolo, auth_token: authToken, session_id: sid || undefined };
        const api: any = {
          root: canvas,
          ctx,
          width,
          height,
          props,
          daemon,
          artifact: {
            url: async (path: any) => {
              const p = String(path ?? "").trim();
              if (!p) throw new Error("artifact.url requires path");
              if (!baseUrl) throw new Error("artifact.url requires baseUrl");
              const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
              const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(p)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
              const r = await fetch(src, { headers: authToken ? { Authorization: `Bearer ${authToken}` } : {} });
              if (!r.ok) throw new Error(`file fetch failed: ${r.status}`);
              const b = await r.blob();
              const u = URL.createObjectURL(b);
              blobUrlsRef.current.push(u);
              return u;
            },
          },
        };

        const trimmed = script.trim();
        const looksLikeFnExpr =
          trimmed.startsWith("function") ||
          trimmed.startsWith("async function") ||
          trimmed.startsWith("(") ||
          trimmed.startsWith("async (") ||
          trimmed.startsWith("async(") ||
          /^[a-zA-Z_$][\\w$]*\\s*=>/.test(trimmed);

        const body = looksLikeFnExpr
          ? `
const __fn = (${script});
if (typeof __fn === "function") return await __fn(api, args);
return __fn;
`
          : `
with ({ api, args, ctx: api.ctx, canvas: api.root, width: api.width, height: api.height, props: api.props, artifact: api.artifact, daemon: api.daemon }) {
  ${script}
}
`;

        // eslint-disable-next-line no-new-func
        const fn = new Function(
          "api",
          "args",
          "__console",
          `const console = __console; return (async function() {\n${body}\n})();`,
        ) as (api: any, args: any, __console: any) => Promise<any>;

        let cancelled = false;
        void (async () => {
          try {
            const res = await fn(api, scriptArgs, makeSceneConsole());
            if (cancelled) return;
            if (typeof res === "function") cleanupRef.current = res as any;
            else if (res && typeof res === "object" && typeof (res as any).cleanup === "function") cleanupRef.current = (res as any).cleanup;
          } catch (e) {
            if (!cancelled) reportError(e);
          }
        })();

        return () => {
          cancelled = true;
          try {
            cleanupRef.current?.();
          } catch {
            // ignore
          }
          cleanupRef.current = null;
          blobUrlsRef.current.forEach((u) => {
            try {
              URL.revokeObjectURL(u);
            } catch {
              // ignore
            }
          });
          blobUrlsRef.current = [];
        };
      } catch (e) {
        reportError(e);
      }
      return;
    }

    for (const op of draw.slice(0, 2000)) {
      if (!op || typeof op !== "object") continue;
      const kind = (op as any).op;
      if (kind === "clear") {
        const color = safeString((op as any).color) || "";
        if (color) {
          ctx.save();
          ctx.fillStyle = color;
          ctx.fillRect(0, 0, width, height);
          ctx.restore();
        } else {
          ctx.clearRect(0, 0, width, height);
        }
        continue;
      }
      if (kind === "polyline") {
        const pts = Array.isArray((op as any).points) ? ((op as any).points as CanvasPoint[]) : [];
        if (pts.length < 2) continue;
        const strokeStyle = safeString((op as any).strokeStyle) || "#60a5fa";
        const lineWidth = safeNumber((op as any).lineWidth, 2);
        ctx.save();
        ctx.strokeStyle = strokeStyle;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        ctx.moveTo(safeNumber(pts[0].x, 0), safeNumber(pts[0].y, 0));
        for (const p of pts.slice(1, 5000)) {
          ctx.lineTo(safeNumber(p.x, 0), safeNumber(p.y, 0));
        }
        ctx.stroke();
        ctx.restore();
        continue;
      }
      if (kind === "line") {
        const strokeStyle = safeString((op as any).strokeStyle) || "#94a3b8";
        const lineWidth = safeNumber((op as any).lineWidth, 1);
        ctx.save();
        ctx.strokeStyle = strokeStyle;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        ctx.moveTo(safeNumber((op as any).x1, 0), safeNumber((op as any).y1, 0));
        ctx.lineTo(safeNumber((op as any).x2, 0), safeNumber((op as any).y2, 0));
        ctx.stroke();
        ctx.restore();
        continue;
      }
      if (kind === "text") {
        const fillStyle = safeString((op as any).fillStyle) || "#e5e7eb";
        const font = safeString((op as any).font) || "12px ui-sans-serif";
        const text = safeString((op as any).text) || "";
        ctx.save();
        ctx.fillStyle = fillStyle;
        ctx.font = font;
        ctx.fillText(text.slice(0, 400), safeNumber((op as any).x, 0), safeNumber((op as any).y, 0));
        ctx.restore();
        continue;
      }
    }
    return () => {
      try {
        cleanupRef.current?.();
      } catch {
        // ignore
      }
      cleanupRef.current = null;
      blobUrlsRef.current.forEach((u) => {
        try {
          URL.revokeObjectURL(u);
        } catch {
          // ignore
        }
      });
      blobUrlsRef.current = [];
    };
  }, [baseUrl, daemonAuthToken, draw, height, width, script, scriptArgsJson, yolo]);

  return (
    <div className="mt-2 overflow-auto rounded-md border border-white/10 bg-black/20 p-2">
      <canvas ref={canvasRef} className="max-w-full" />
    </div>
  );
}

function JsonEntityView({ entity }: { entity: SceneEntity }) {
  return (
    <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/20 p-2 text-xs text-white/80">
      {JSON.stringify(entity.props ?? {}, null, 2)}
    </pre>
  );
}

function DomEntityView({
  entity,
  baseUrl,
  yolo,
  sessionId,
  daemonAuthToken,
  onScriptError,
}: {
  entity: SceneEntity;
  baseUrl?: string;
  yolo?: boolean;
  sessionId?: string;
  daemonAuthToken?: string;
  onScriptError?: (args: {
    entity_id: string;
    entity_kind: string;
    error: string;
    stack_preview?: string;
    script_preview?: string;
  }) => void;
}) {
  const rootRef = React.useRef<HTMLDivElement | null>(null);
  const cleanupRef = React.useRef<null | (() => void)>(null);
  const blobUrlsRef = React.useRef<string[]>([]);

  const props = entity.props ?? {};
  const htmlRaw = safeString(props?.html ?? props?.markup ?? props?.inner_html ?? props?.innerHTML);
  const html = htmlRaw.length > 1_000_000 ? htmlRaw.slice(0, 1_000_000) : htmlRaw;

  const scriptRaw = safeString(props?.script ?? props?.js ?? props?.code);
  const script = scriptRaw.length > 1_000_000 ? scriptRaw.slice(0, 1_000_000) : scriptRaw;

  const scriptArgs = props?.script_args ?? props?.args ?? {};
  const scriptArgsJson = React.useMemo(() => {
    try {
      const s = JSON.stringify(scriptArgs ?? {});
      return s.length > 128_000 ? s.slice(0, 128_000) : s;
    } catch {
      return "{}";
    }
  }, [scriptArgs]);

  React.useEffect(() => {
    const root = rootRef.current;
    if (!root) return;

    // Reset previous run (cleanup + blob URLs).
    try {
      cleanupRef.current?.();
    } catch {
      // ignore
    }
    cleanupRef.current = null;
    blobUrlsRef.current.forEach((u) => {
      try {
        URL.revokeObjectURL(u);
      } catch {
        // ignore
      }
    });
    blobUrlsRef.current = [];

    // Render HTML into the container (DOM is intentionally "unleashed" for this entity kind).
    try {
      root.innerHTML = html || "";
    } catch {
      // ignore
    }

    const reportError = (e: any) => {
      const msg = String(e || "unknown error");
      const stack = e && typeof e === "object" && typeof e.stack === "string" ? e.stack : "";
      try {
        onScriptError?.({
          entity_id: entity.id,
          entity_kind: entity.kind,
          error: msg,
          stack_preview: typeof stack === "string" ? stack.slice(0, 1200) : "",
          script_preview: script.slice(0, 800),
        });
      } catch {
        // ignore
      }
    };

    if (!baseUrl || typeof yolo !== "boolean") return;
    if (!script || script.trim().length === 0) return;

    const authToken = safeString(daemonAuthToken).trim();
    const sid = safeString(sessionId).trim();
    const daemon = { base_url: baseUrl, yolo: !!yolo, auth_token: authToken, session_id: sid || undefined };
    const api: any = {
      root,
      daemon,
      artifact: {
        url: async (path: any) => {
          const p = String(path ?? "").trim();
          if (!p) throw new Error("artifact.url requires path");
          const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
          const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(p)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
          const r = await fetch(src, { headers: authToken ? { Authorization: `Bearer ${authToken}` } : {} });
          if (!r.ok) throw new Error(`file fetch failed: ${r.status}`);
          const b = await r.blob();
          const u = URL.createObjectURL(b);
          blobUrlsRef.current.push(u);
          return u;
        },
      },
    };

    let cancelled = false;
    void (async () => {
      try {
        // Execute the agent-provided script.
        //
        // Compatibility goals (important):
        // - Support scripts written as an async function BODY (so `await` + `return cleanup` work):
        //     // uses api.root
        //     ...
        //     return () => { ... }
        // - Also support scripts written as a FUNCTION EXPRESSION (common model output):
        //     async (api, args) => { ... return () => {...} }
        //
        // Build the function body with REAL newlines. (Using a literal `\\n` in source breaks parsing.)
        const trimmed = script.trim();
        const looksLikeFnExpr =
          trimmed.startsWith("function") ||
          trimmed.startsWith("async function") ||
          trimmed.startsWith("(") ||
          trimmed.startsWith("async (") ||
          trimmed.startsWith("async(") ||
          /^[a-zA-Z_$][\\w$]*\\s*=>/.test(trimmed);

        const body = looksLikeFnExpr
          ? `
const __fn = (${script});
if (typeof __fn === "function") return await __fn(api, args);
return __fn;
`
          : script;

        // eslint-disable-next-line no-new-func
        const fn = new Function(
          "api",
          "args",
          "__console",
          '"use strict"; const console = __console; return (async function() {\n' + body + "\n})();",
        ) as (
          api: any,
          args: any,
          __console: any,
        ) => Promise<any>;
        const res = await fn(api, scriptArgs, makeSceneConsole());
        if (cancelled) return;
        if (typeof res === "function") cleanupRef.current = res as any;
        else if (res && typeof res === "object" && typeof (res as any).cleanup === "function") cleanupRef.current = (res as any).cleanup;
      } catch (e) {
        if (cancelled) return;
        reportError(e);
      }
    })();

    return () => {
      cancelled = true;
      try {
        cleanupRef.current?.();
      } catch {
        // ignore
      }
      cleanupRef.current = null;
      blobUrlsRef.current.forEach((u) => {
        try {
          URL.revokeObjectURL(u);
        } catch {
          // ignore
        }
      });
      blobUrlsRef.current = [];
    };
  }, [baseUrl, daemonAuthToken, entity.id, entity.kind, html, onScriptError, script, scriptArgsJson, yolo]);

	return (
		// Render DOM entities on a light surface: most tool-generated HTML assumes light backgrounds and
		// default (black) text. The app shell is dark, so without an explicit text color the content may
		// inherit `text-white` and become unreadable on white panels inside the entity.
		<div className="mt-2 overflow-auto rounded-md border border-black/10 bg-white p-2 text-slate-900">
			<div ref={rootRef} />
		</div>
	);
}

export default function SceneView({
  baseUrl,
  yolo,
  allowAutoplay,
  client,
  daemonAuthToken,
  sessionId,
  entities,
  className,
}: {
  baseUrl?: string;
  yolo?: boolean;
  allowAutoplay?: boolean;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  client?: any;
  daemonAuthToken?: string;
  sessionId?: string;
  entities: SceneEntity[];
  className?: string;
}) {
  const sid = typeof sessionId === "string" ? sessionId.trim() : "";
  const lastSceneErrorRef = React.useRef<Record<string, { ts: number; sig: string }>>({});
  const defaultExpandedCount = 1;

  const sortedEntities = React.useMemo(() => {
    const copy = [...entities];
    copy.sort((a: any, b: any) => {
      const ta = typeof a?.updated_ms === "number" ? a.updated_ms : typeof a?.created_ms === "number" ? a.created_ms : 0;
      const tb = typeof b?.updated_ms === "number" ? b.updated_ms : typeof b?.created_ms === "number" ? b.created_ms : 0;
      if (ta !== tb) return tb - ta;
      return String(b?.id || "").localeCompare(String(a?.id || ""));
    });
    return copy;
  }, [entities]);

  const expandedKey = React.useMemo(() => {
    const base = typeof baseUrl === "string" ? baseUrl.trim() : "";
    const sidKey = typeof sessionId === "string" ? sessionId.trim() : "";
    return `agentui.scene.expandedById:${base}::${sidKey}`;
  }, [baseUrl, sessionId]);
  const [expandedById, setExpandedById] = useLocalStorageState<Record<string, boolean>>(expandedKey, {});
  React.useEffect(() => {
    // Keep UX stable as the Scene updates:
    // - Newly appeared entities default to expanded if they are among the latest N.
    // - Old entries remain in the user's chosen expanded/collapsed state.
    // - Removed entities are pruned from the map.
    setExpandedById((prev) => {
      const next: Record<string, boolean> = {};
      const seen = new Set<string>();
      for (let i = 0; i < sortedEntities.length; i++) {
        const id = String(sortedEntities[i]?.id || "");
        if (!id) continue;
        seen.add(id);
        if (Object.prototype.hasOwnProperty.call(prev, id)) next[id] = !!prev[id];
        else next[id] = i < defaultExpandedCount;
      }
      return next;
    });
  }, [defaultExpandedCount, sid, sortedEntities]);

  const postSceneError = React.useCallback(
    async (payload: { entity_id: string; entity_kind: string; error: string; stack_preview?: string; script_preview?: string }) => {
      if (!baseUrl) return;
      if (!sid) return;
      const cid = client && typeof client === "object" ? client : { id: "webui", kind: "webui" };
      const sig = `${payload.entity_id}:${payload.error}`;
      const now = Date.now();
      const prev = lastSceneErrorRef.current[payload.entity_id];
      // De-dupe noisy repeated errors (e.g. repeated renders) for 10 seconds unless the error changes.
      if (prev && prev.sig === sig && now - prev.ts < 10_000) return;
      lastSceneErrorRef.current[payload.entity_id] = { ts: now, sig };

      try {
        await apiPostSessionUiEvent(
          baseUrl,
          {
            session_id: sid,
            type: "scene_error",
            client: cid,
            data: { ...payload, ts_unix_ms: now },
            append_to_session: false,
          },
          daemonAuthToken,
        );
      } catch {
        // ignore
      }
    },
    [baseUrl, client, daemonAuthToken, sid],
  );
  return (
    <div
      className={["flex min-h-0 flex-col rounded-lg border border-white/10 bg-white/5", className].filter(Boolean).join(" ")}
      data-testid="scene"
    >
      <div className="flex shrink-0 items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold" data-testid="scene-header">
          Scene
        </div>
        <div className="flex items-center gap-2">
          <div className="text-[11px] text-white/40" data-testid="scene-session">
            {sid ? `session=${sid}` : ""}
          </div>
        </div>
	      </div>
	      <div className="min-h-0 flex-1 overflow-auto px-3 pb-3">
	        {entities.length === 0 ? (
	          <div className="py-6 text-xs text-white/50">No scene entities.</div>
	        ) : (
	          <div className="space-y-3">
              <div className="flex items-center justify-between gap-2">
                <div className="text-[11px] text-white/40">
                  Showing latest {defaultExpandedCount} expanded; older collapsed.
                </div>
                <div className="flex items-center gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    onClick={() =>
                      setExpandedById((prev) => {
                        const next = { ...prev };
                        for (const e of sortedEntities) next[e.id] = true;
                        return next;
                      })
                    }
                    disabled={sortedEntities.length === 0}
                    title="Expand all scene entities."
                  >
                    Expand all
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    onClick={() =>
                      setExpandedById((prev) => {
                        const next = { ...prev };
                        for (const e of sortedEntities) next[e.id] = false;
                        return next;
                      })
                    }
                    disabled={sortedEntities.length === 0}
                    title="Collapse all scene entities."
                  >
                    Collapse all
                  </button>
                </div>
              </div>

	            {sortedEntities.map((e, idx) => {
	              const title = e.title || `${e.kind}:${e.id}`;
	              const entityTid = `scene-entity-${toTestIdPart(e.id)}`;
	              const props = e.props ?? {};
                const expanded = Object.prototype.hasOwnProperty.call(expandedById, e.id) ? !!expandedById[e.id] : idx < defaultExpandedCount;
                const ts = typeof e.updated_ms === "number" ? e.updated_ms : typeof e.created_ms === "number" ? e.created_ms : 0;
	              return (
	                <div key={e.id} className="rounded-md border border-white/10 bg-black/10 p-3" data-testid={entityTid}>
	                  <button
                      className="flex w-full items-center justify-between gap-2 text-left"
                      type="button"
                      onClick={() => setExpandedById((prev) => ({ ...prev, [e.id]: !expanded }))}
                      title={expanded ? "Collapse" : "Expand"}
                    >
	                    <div className="text-xs font-semibold text-white/80">{title}</div>
	                    <div className="flex items-center gap-2 text-[11px] text-white/40">
                        {ts > 0 ? <span>{new Date(ts).toLocaleString()}</span> : null}
	                      <code>{e.id}</code>
	                    </div>
	                  </button>

                    {!expanded ? (
                      <div className="mt-2 text-[11px] text-white/40">Collapsed</div>
                    ) : e.kind === "canvas2d" ? (
                      <Canvas2DEntityView
                        entity={e}
                        baseUrl={baseUrl}
                        yolo={yolo}
                        sessionId={sessionId}
                        daemonAuthToken={daemonAuthToken}
                        onScriptError={postSceneError}
                      />
                    ) : e.kind === "dom" ? (
                      <DomEntityView
                        entity={e}
                        baseUrl={baseUrl}
                        yolo={yolo}
                        sessionId={sessionId}
                        daemonAuthToken={daemonAuthToken}
                        onScriptError={postSceneError}
                      />
                    ) : e.kind === "artifact" && baseUrl && typeof yolo === "boolean" ? (
                      <div className="mt-2">
                        <ArtifactView
                          baseUrl={baseUrl}
                          yolo={yolo}
                          artifact={props?.artifact ?? props}
                          allowAutoplay={!!allowAutoplay}
                          sessionId={sessionId}
                          client={client}
                          daemonAuthToken={daemonAuthToken}
                        />
                      </div>
                    ) : (
                      <JsonEntityView entity={e} />
                    )}
                  </div>
	              );
	            })}
	          </div>
	        )}
	      </div>
	    </div>
  );
}
