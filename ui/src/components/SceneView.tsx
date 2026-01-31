import React from "react";

import ArtifactView from "./ArtifactView";
import { apiPostSessionUiEvent } from "../api";

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
  onScriptError,
}: {
  entity: SceneEntity;
  onScriptError?: (args: { entity_id: string; entity_kind: string; error: string; script_preview?: string }) => void;
}) {
  const canvasRef = React.useRef<HTMLCanvasElement | null>(null);
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
      const reportError = (e: any) => {
        const msg = String(e || "unknown error");
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
            script_preview: script.slice(0, 800),
          });
        } catch {
          // ignore
        }
      };
      try {
        // Execute the agent-provided script in a "power mode" sandbox.
        //
        // Important compatibility behavior:
        // - We do NOT pass a parameter named `ctx`, because many scripts naturally start with:
        //     const ctx = canvas.getContext('2d');
        //   which would throw "Identifier 'ctx' has already been declared" if `ctx` were also a function parameter.
        // - We expose common names via a `with` scope, so scripts can either use `ctx` directly,
        //   or declare their own `const ctx = ...` without colliding.
        //
        // eslint-disable-next-line no-new-func
        const fn = new Function(
          "__ctx",
          "canvas",
          "width",
          "height",
          "props",
          "args",
          `
try {
  with ({ ctx: __ctx, canvas, width, height, props, args }) {
    ${script}
  }
} catch (e) {
  throw e;
}
`,
        );
        // Allow scripts to be either sync or async (best-effort).
        const maybePromise = fn(ctx, canvas, width, height, props, scriptArgs);
        if (maybePromise && typeof (maybePromise as any).then === "function") {
          void (maybePromise as any).catch((e: any) => reportError(e));
        }
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
  }, [draw, height, width, script, scriptArgsJson]);

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

export default function SceneView({
  baseUrl,
  yolo,
  allowAutoplay,
  client,
  daemonAuthToken,
  sessionId,
  entities,
  onClear,
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
  onClear?: () => void;
  className?: string;
}) {
  const sid = typeof sessionId === "string" ? sessionId.trim() : "";
  const lastSceneErrorRef = React.useRef<Record<string, { ts: number; sig: string }>>({});

  const postSceneError = React.useCallback(
    async (payload: { entity_id: string; entity_kind: string; error: string; script_preview?: string }) => {
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
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => onClear?.()}
            disabled={!onClear || entities.length === 0}
            title="Clear the client-side scene entities (local UI state)."
          >
            Clear
          </button>
        </div>
      </div>
      <div className="min-h-0 flex-1 overflow-auto px-3 pb-3">
        {entities.length === 0 ? (
          <div className="py-6 text-xs text-white/50">No scene entities.</div>
        ) : (
          <div className="space-y-3">
            {entities.map((e) => {
              const title = e.title || `${e.kind}:${e.id}`;
              const entityTid = `scene-entity-${toTestIdPart(e.id)}`;
              const props = e.props ?? {};
              return (
                <div key={e.id} className="rounded-md border border-white/10 bg-black/10 p-3" data-testid={entityTid}>
                  <div className="flex items-center justify-between gap-2">
                    <div className="text-xs font-semibold text-white/80">{title}</div>
                    <div className="text-[11px] text-white/40">
                      <code>{e.id}</code>
                    </div>
                  </div>
                  {e.kind === "canvas2d" ? (
                    <Canvas2DEntityView entity={e} onScriptError={postSceneError} />
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
