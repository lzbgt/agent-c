import React from "react";
import type { ApiAuth } from "../../api";
import type { SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import SceneScriptFrame from "./SceneScriptFrame";
import { clampInt, safeObject, safeString, safeNumber } from "./sceneViewUtils";

type Props = { entity: SceneEntity; baseUrl?: string; yolo?: boolean; sessionId?: string; daemonAuth?: ApiAuth; onScriptError?: (args: SceneScriptErrorArgs) => void };

function StaticCanvas({ width, height, draw }: { width: number; height: number; draw: unknown[] }) {
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  React.useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas?.getContext("2d");
    if (!canvas || !ctx) return;
    canvas.width = width; canvas.height = height;
    ctx.clearRect(0, 0, width, height);
    for (const drawValue of draw.slice(0, 2000)) {
      const op = safeObject(drawValue);
      const kind = safeString(op.op);
      if (kind === "clear") {
        const color = safeString(op.color) || "";
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
        const pts = Array.isArray(op.points) ? op.points.map((point) => safeObject(point)) : [];
        if (pts.length < 2) continue;
        const strokeStyle = safeString(op.strokeStyle) || "#60a5fa";
        const lineWidth = safeNumber(op.lineWidth, 2);
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
        const strokeStyle = safeString(op.strokeStyle) || "#94a3b8";
        const lineWidth = safeNumber(op.lineWidth, 1);
        ctx.save();
        ctx.strokeStyle = strokeStyle;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        ctx.moveTo(safeNumber(op.x1, 0), safeNumber(op.y1, 0));
        ctx.lineTo(safeNumber(op.x2, 0), safeNumber(op.y2, 0));
        ctx.stroke();
        ctx.restore();
        continue;
      }
      if (kind === "text") {
        const fillStyle = safeString(op.fillStyle) || "#e5e7eb";
        const font = safeString(op.font) || "12px ui-sans-serif";
        const text = safeString(op.text) || "";
        ctx.save();
        ctx.fillStyle = fillStyle;
        ctx.font = font;
        ctx.fillText(text.slice(0, 400), safeNumber(op.x, 0), safeNumber(op.y, 0));
        ctx.restore();
      }
    }

  }, [draw, width, height]);
  return <canvas ref={canvasRef} className="mt-2 max-w-full rounded-md border border-white/10" />;
}

export default function Canvas2DEntityView(props: Props) {
  const values = safeObject(props.entity.props);
  const width = clampInt(values.width, 64, 4096, 640);
  const height = clampInt(values.height, 64, 4096, 240);
  const script = safeString(values.script || values.js || values.code).slice(0, 100_000);
  const draw = React.useMemo(() => Array.isArray(values.draw) ? values.draw : [], [values.draw]);
  return script.trim() ? <SceneScriptFrame {...props} kind="canvas" script={script} width={width} height={height} /> : <StaticCanvas width={width} height={height} draw={draw} />;
}
