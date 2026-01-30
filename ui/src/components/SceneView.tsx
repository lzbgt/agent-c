import React from "react";

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

function Canvas2DEntityView({ entity }: { entity: SceneEntity }) {
  const canvasRef = React.useRef<HTMLCanvasElement | null>(null);
  const props = entity.props ?? {};
  const width = clampInt(props?.width, 64, 4096, 640);
  const height = clampInt(props?.height, 64, 4096, 240);
  const draw: DrawOp[] = Array.isArray(props?.draw) ? props.draw : [];

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
  }, [draw, height, width]);

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
  sessionId,
  entities,
  onClear,
}: {
  sessionId?: string;
  entities: SceneEntity[];
  onClear?: () => void;
}) {
  const sid = typeof sessionId === "string" ? sessionId.trim() : "";
  return (
    <div className="mt-6 rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold">Scene</div>
        <div className="flex items-center gap-2">
          <div className="text-[11px] text-white/40">{sid ? `session=${sid}` : ""}</div>
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
      <div className="px-3 pb-3">
        {entities.length === 0 ? (
          <div className="text-xs text-white/50">No scene entities.</div>
        ) : (
          <div className="space-y-3">
            {entities.map((e) => {
              const title = e.title || `${e.kind}:${e.id}`;
              return (
                <div key={e.id} className="rounded-md border border-white/10 bg-black/10 p-3">
                  <div className="flex items-center justify-between gap-2">
                    <div className="text-xs font-semibold text-white/80">{title}</div>
                    <div className="text-[11px] text-white/40">
                      <code>{e.id}</code>
                    </div>
                  </div>
                  {e.kind === "canvas2d" ? <Canvas2DEntityView entity={e} /> : <JsonEntityView entity={e} />}
                </div>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}

