import React from "react";

import { daemonFetchInit, daemonHeaders, type ApiAuth } from "../../api";
import type { DrawOp, SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import { clampInt, makeSceneConsole, safeNumber, safeString } from "./sceneViewUtils";

export default function Canvas2DEntityView({
  entity,
  baseUrl,
  yolo,
  sessionId,
  daemonAuth,
  onScriptError,
}: {
  entity: SceneEntity;
  baseUrl?: string;
  yolo?: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  onScriptError?: (args: SceneScriptErrorArgs) => void;
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
    canvas.width = width;
    canvas.height = height;
    ctx.clearRect(0, 0, width, height);

    if (script && script.trim().length > 0) {
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
        const sid = safeString(sessionId).trim();
        const tokenRaw = safeString((daemonAuth as any)?.token).trim();
        const agentdTokenRaw = safeString((daemonAuth as any)?.agentdToken).trim();
        const daemon = {
          base_url: baseUrl || "",
          yolo: !!yolo,
          auth_token: tokenRaw.replace(/^bearer\\s+/i, "").trim(),
          agentd_auth_token: agentdTokenRaw.replace(/^bearer\\s+/i, "").trim(),
          headers: daemonHeaders(daemonAuth),
          session_id: sid || undefined,
        };
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
              const r = await fetch(src, daemonFetchInit(daemonAuth));
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
        const pts = Array.isArray((op as any).points) ? ((op as any).points as any[]) : [];
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
  }, [baseUrl, daemonAuth, draw, entity.id, entity.kind, height, onScriptError, props, script, scriptArgs, scriptArgsJson, sessionId, width, yolo]);

  return (
    <div className="mt-2 overflow-auto rounded-md border border-white/10 bg-black/20 p-2">
      <canvas ref={canvasRef} className="max-w-full" />
    </div>
  );
}
